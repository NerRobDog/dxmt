/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"
#include "dxgi_interfaces.h"
#include "dxmt_scaler.hpp"
#include "dxmt_statistics.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_likely.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>
#include <unistd.h>

namespace dxmt {

constexpr auto kCommandQueueSize = 32u;

class MTLD3D12CommandQueueImpl : public MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory> {

  D3D12_COMMAND_QUEUE_DESC desc_;

  WMT::Reference<WMT::CommandQueue> queue_;
  WMT::Reference<WMT::Fence> fence_;

  std::atomic_uint64_t inflight_cmdbuf_seq_ = 1;
  std::atomic_uint64_t inflight_cmdbuf_count_ = 0;
  std::atomic_uint64_t inflight_cmdbuf_stop_ = 0;

  struct InflightCommandBuffer {
    WMT::Reference<WMT::CommandBuffer> cmdbuf{};
    HANDLE semaphore{};
  };

  std::array<InflightCommandBuffer, kCommandQueueSize> inflight_cmdbuf_pool_;
  dxmt::thread inflight_cmdbuf_wait_thread_;

  // DXMT_FRAME_LOG per-frame CSV logger state (d3d12 variant)
  bool frame_log_checked_ = false;
  std::FILE *frame_log_ = nullptr;
  std::chrono::steady_clock::time_point frame_log_last_{};
  uint64_t frame_log_frame_ = 0;
  uint64_t frame_log_compiles_ = 0;
  uint64_t frame_log_encode_us_ = 0;
  uint64_t frame_log_submits_ = 0;
  uint64_t frame_log_encoders_ = 0;

  // cumulative ExecuteCommandLists stats (updated on the encode worker in
  // async mode, on the calling thread in sync mode)
  std::atomic_uint64_t stat_encode_us_{0};
  std::atomic_uint64_t stat_submits_{0};
  std::atomic_uint64_t stat_encoders_{0};

  dxmt::mutex mutex_commit_;

  /*
   * Deferred submission executed by the encode worker thread. All submission
   * kinds (execute/signal/wait/present) go through the same FIFO queue so the
   * original commit order of command buffers is preserved.
   */
  struct Submission {
    enum class Type { Execute, Signal, Wait, Present } type;
    // Execute: snapshots of per-list encoder chains, taken at
    // ExecuteCommandLists time. The chain nodes live in the command
    // allocator's bump heap, which the app must keep alive (and must not
    // Reset) until the GPU is done - and GPU completion is observed via a
    // fence signal that is queued strictly after this submission, so FIFO
    // order upholds the contract. The Null head node lives in the
    // allocator's encoder list vector, which may grow while the app records
    // further command lists, so we snapshot head->next instead of walking
    // through the head on the worker.
    std::vector<EncoderData *> chains;
    // Signal / Wait: keep the underlying fence alive even if the app
    // releases the ID3D12Fence right after the call returns.
    Rc<Fence> fence;
    uint64_t fence_value = 0;
    // Present: the swapchain (and thus the presenter and backbuffer) may be
    // released by the app after Present returns; hold strong references
    // until the worker has encoded the present.
    Rc<Presenter> presenter;
    Com<ID3D12Resource> backbuffer;
    // Present with MetalFX spatial upscaling: both stay null on the plain
    // path. Strong references for the same reason as above - ResizeBuffers
    // may drop the swapchain's scaler/texture while the worker still has a
    // pending present that uses them.
    Rc<SpatialScaler> scaler;
    Com<ID3D12Resource> upscaled;
    HANDLE semaphore = nullptr;
    double present_after = 0;
  };

  bool sync_encode_ = false;
  dxmt::mutex mutex_worker_;
  dxmt::condition_variable cv_worker_;
  std::deque<Submission> worker_queue_;
  bool worker_stop_ = false;
  dxmt::thread encode_worker_thread_;

  void
  CommandBufferWaitingThread() {
    env::setThreadName("dxmt-cmdbuf-waiting-thread");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    uint64_t internal_seq = 1;
    for (;;) {
      inflight_cmdbuf_seq_.wait(internal_seq, std::memory_order_acquire);
      if (inflight_cmdbuf_stop_.load() == internal_seq)
        break;
      auto &inflight = inflight_cmdbuf_pool_[internal_seq % kCommandQueueSize];

      if (inflight.cmdbuf.status() <= WMTCommandBufferStatusScheduled)
        inflight.cmdbuf.waitUntilCompleted();
      if (inflight.cmdbuf.status() == WMTCommandBufferStatusError)
        ERR("Device error: ", inflight.cmdbuf.error().description().getUTF8String());

      if (inflight.semaphore)
        ReleaseSemaphore(inflight.semaphore, 1, nullptr);

      inflight = {};

      inflight_cmdbuf_count_.fetch_sub(1, std::memory_order_release);
      inflight_cmdbuf_count_.notify_one();

      internal_seq++;
    }
  };

  void
  EncodeWorkerThread() {
    env::setThreadName("dxmt-d3d12-encode-thread");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    for (;;) {
      Submission sub;
      {
        std::unique_lock<dxmt::mutex> lock(mutex_worker_);
        cv_worker_.wait(lock, [this] { return worker_stop_ || !worker_queue_.empty(); });
        if (worker_queue_.empty())
          break; // stopped and fully drained
        sub = std::move(worker_queue_.front());
        worker_queue_.pop_front();
      }
      ProcessSubmission(sub);
    }
  }

  struct CommittingScope {
    MTLD3D12CommandQueueImpl *queue;
    InflightCommandBuffer &inflight;
    std::lock_guard<dxmt::mutex> lock;
    WMT::Reference<WMT::Object> pool;

    CommittingScope(MTLD3D12CommandQueueImpl *queue, uint64_t seq) :
        queue(queue),
        inflight(queue->inflight_cmdbuf_pool_[seq % kCommandQueueSize]),
        lock(queue->mutex_commit_),
        pool(WMT::MakeAutoreleasePool()) {
      inflight.cmdbuf = queue->queue_.commandBuffer();
    };

    ~CommittingScope() {
      inflight.cmdbuf.commit();
      queue->inflight_cmdbuf_seq_.fetch_add(1, std::memory_order_release);
      queue->inflight_cmdbuf_seq_.notify_one();
      queue->inflight_cmdbuf_count_.fetch_add(1, std::memory_order_relaxed);
    }
  };

  CommittingScope
  StartCommitting() {
    inflight_cmdbuf_count_.wait(kCommandQueueSize, std::memory_order_acquire);
    return CommittingScope(this, inflight_cmdbuf_seq_.load(std::memory_order_relaxed));
  }

  /* Encode one command list's encoder chain into cmdbuf; returns the number
   * of encoders processed. Runs on the encode worker in async mode. */
  uint64_t
  EncodeSubmission(WMT::CommandBuffer cmdbuf, EncoderData *current) {
    uint64_t encoders = 0;
    while (current) {
      encoders++;
      switch (current->type) {
      case EncoderType::Null:
        break;
      case EncoderType::Clear: {
        auto data = static_cast<ClearEncoderData *>(current);
        {
          WMTRenderPassInfo info;
          WMT::InitializeRenderPassInfo(info);
          if (data->clear_dsv) {
            if (data->clear_dsv & 1) {
              info.depth.clear_depth = data->depth_stencil.first;
              info.depth.texture = data->attachment.texture();
              info.depth.load_action = WMTLoadActionClear;
              info.depth.store_action = WMTStoreActionStore;
              info.depth.depth_plane = data->depth_plane;
            }
            if (data->clear_dsv & 2) {
              info.stencil.clear_stencil = data->depth_stencil.second;
              info.stencil.texture = data->attachment.texture();
              info.stencil.load_action = WMTLoadActionClear;
              info.stencil.store_action = WMTStoreActionStore;
              info.stencil.depth_plane = data->depth_plane;
            }
            info.render_target_width = data->width;
            info.render_target_height = data->height;
          } else {
            info.colors[0].clear_color = data->color;
            info.colors[0].texture = data->attachment.texture();
            info.colors[0].load_action = WMTLoadActionClear;
            info.colors[0].store_action = WMTStoreActionStore;
            info.colors[0].depth_plane = data->depth_plane;
          }
          info.render_target_array_length = data->array_length;
          auto encoder = cmdbuf.renderCommandEncoder(info);
          encoder.setLabel(WMT::String::string("ClearPass", WMTUTF8StringEncoding));
          encoder.waitForFence(fence_, WMTRenderStageFragment);
          encoder.updateFence(fence_, WMTRenderStageFragment);
          encoder.endEncoding();
        }
        break;
      }
      case EncoderType::Render: {
        auto data = static_cast<RenderEncoderData *>(current);
        WMTRenderPassInfo render_pass_info;
        WMT::InitializeRenderPassInfo(render_pass_info);
        {
          for (unsigned i = 0; i < std::size(render_pass_info.colors); i++) {
            auto &color_data = data->colors[i];
            if (!color_data.attachment)
              continue;
            auto &color_info = render_pass_info.colors[i];
            color_info.texture = color_data.attachment.texture();
            color_info.load_action = color_data.load_action;
            color_info.store_action = color_data.store_action;
            color_info.level = color_data.level;
            color_info.slice = color_data.slice;
            color_info.depth_plane = color_data.depth_plane;
            color_info.clear_color = color_data.clear_color;
            color_info.resolve_texture = color_data.resolve_attachment.texture();
            color_info.resolve_level = color_data.resolve_level;
            color_info.resolve_slice = color_data.resolve_slice;
            color_info.resolve_depth_plane = color_data.resolve_depth_plane;
          }
          if (data->depth.attachment) {
            auto &depth_info = render_pass_info.depth;
            auto &depth_data = data->depth;
            depth_info.texture = depth_data.attachment.texture();
            depth_info.load_action = depth_data.load_action;
            depth_info.store_action = depth_data.store_action;
            depth_info.level = depth_data.level;
            depth_info.slice = depth_data.slice;
            depth_info.depth_plane = depth_data.depth_plane;
            depth_info.clear_depth = depth_data.clear_depth;
          }
          if (data->stencil.attachment) {
            auto &stencil_info = render_pass_info.stencil;
            auto &stencil_data = data->stencil;
            stencil_info.texture = stencil_data.attachment.texture();
            stencil_info.load_action = stencil_data.load_action;
            stencil_info.store_action = stencil_data.store_action;
            stencil_info.level = stencil_data.level;
            stencil_info.slice = stencil_data.slice;
            stencil_info.depth_plane = stencil_data.depth_plane;
            stencil_info.clear_stencil = stencil_data.clear_stencil;
          }
          render_pass_info.default_raster_sample_count = data->default_raster_sample_count;
          render_pass_info.render_target_array_length = data->render_target_array_length;
          render_pass_info.render_target_width = data->render_target_width;
          render_pass_info.render_target_height = data->render_target_height;
        }
        auto encoder = cmdbuf.renderCommandEncoder(render_pass_info);
        encoder.waitForFence(fence_, WMTRenderStageVertex);
        encoder.encodeCommands(&data->cmd_head);
        encoder.updateFence(fence_, WMTRenderStageFragment);
        encoder.endEncoding();
        break;
      }
      case EncoderType::Blit: {
        auto data = static_cast<BlitEncoderData *>(current);
        auto encoder = cmdbuf.blitCommandEncoder();
        encoder.waitForFence(fence_);
        encoder.encodeCommands(&data->cmd_head);
        encoder.updateFence(fence_);
        encoder.endEncoding();
        break;
      }
      case EncoderType::Compute: {
        auto data = static_cast<ComputeEncoderData *>(current);
        auto encoder = cmdbuf.computeCommandEncoder(false);
        encoder.waitForFence(fence_);
        encoder.encodeCommands(&data->cmd_head);
        encoder.updateFence(fence_);
        encoder.endEncoding();
        break;
      }
      case EncoderType::Resolve: {
        auto data = static_cast<ResolveEncoderData *>(current);

        WMTRenderPassInfo info;
        WMT::InitializeRenderPassInfo(info);
        info.colors[0].texture = data->src.texture();
        info.colors[0].load_action = WMTLoadActionLoad;
        info.colors[0].store_action = WMTStoreActionStoreAndMultisampleResolve;
        info.colors[0].resolve_texture = data->dst.texture();

        auto encoder = cmdbuf.renderCommandEncoder(info);
        encoder.waitForFence(fence_, WMTRenderStageFragment);
        encoder.setLabel(WMT::String::string("ResolvePass", WMTUTF8StringEncoding));
        encoder.updateFence(fence_, WMTRenderStageFragment);
        encoder.endEncoding();

        break;
      }
      }
      current = current->next;
    }
    return encoders;
  }

  void
  ProcessSubmission(Submission &sub) {
    switch (sub.type) {
    case Submission::Type::Execute: {
      auto stat_t0 = std::chrono::steady_clock::now();
      uint64_t stat_encoders = 0;
      auto scope = StartCommitting();
      auto &cmdbuf = scope.inflight.cmdbuf;
      for (auto chain : sub.chains) {
        // +1 accounts for the skipped Null head node, keeping the encoders
        // stat comparable with the previous synchronous implementation
        stat_encoders += 1 + EncodeSubmission(cmdbuf, chain);
      }
      stat_encoders_.fetch_add(stat_encoders, std::memory_order_relaxed);
      stat_submits_.fetch_add(1, std::memory_order_relaxed);
      stat_encode_us_.fetch_add(
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - stat_t0).count(),
          std::memory_order_relaxed);
      break;
    }
    case Submission::Type::Signal: {
      auto scope = StartCommitting();
      sub.fence->signal(scope.inflight.cmdbuf, sub.fence_value);
      break;
    }
    case Submission::Type::Wait: {
      auto scope = StartCommitting();
      sub.fence->wait(scope.inflight.cmdbuf, sub.fence_value);
      break;
    }
    case Submission::Type::Present: {
      auto scope = StartCommitting();
      auto &cmdbuf = scope.inflight.cmdbuf;

      auto g = reinterpret_cast<MTLD3D12Resource *>(sub.backbuffer.ptr());
      auto &view = g->texture->view(g->texture->fullView);
      WMT::Texture present_texture = view.texture;

      if (sub.scaler && sub.upscaled) {
        auto u = reinterpret_cast<MTLD3D12Resource *>(sub.upscaled.ptr());
        auto &upscaled_view = u->texture->view(u->texture->fullView);

        // The MetalFX scaler is not fence-aware beyond its own fence: bridge
        // the queue fence into the scaler fence and back, so the scale pass
        // is ordered after the game's rendering to the backbuffer and before
        // the presenter's blit from the upscaled texture.
        auto begin_scaler = cmdbuf.blitCommandEncoder();
        begin_scaler.setLabel(WMT::String::string("BeginScaler", WMTUTF8StringEncoding));
        begin_scaler.waitForFence(fence_);
        begin_scaler.updateFence(sub.scaler->fence());
        begin_scaler.endEncoding();

        cmdbuf.encodeSpatialScale(sub.scaler->scaler(), view.texture, upscaled_view.texture, sub.scaler->fence());

        auto end_scaler = cmdbuf.blitCommandEncoder();
        end_scaler.setLabel(WMT::String::string("EndScaler", WMTUTF8StringEncoding));
        end_scaler.waitForFence(sub.scaler->fence());
        end_scaler.updateFence(fence_);
        end_scaler.endEncoding();

        present_texture = upscaled_view.texture;
      }

      auto state = sub.presenter->synchronizeLayerProperties();
      auto drawable = sub.presenter->encodeCommands(
          cmdbuf, present_texture, state.metadata,
          [&](auto encoder) { encoder.waitForFence(fence_, WMTRenderStageFragment); },
          [&](auto encoder) { encoder.updateFence(fence_, WMTRenderStageFragment); }
      );

      if (sub.present_after > 0)
        cmdbuf.presentDrawableAfterMinimumDuration(drawable, sub.present_after);
      else
        cmdbuf.presentDrawable(drawable);
      scope.inflight.semaphore = sub.semaphore;
      break;
    }
    }
  }

  /* Returns the queue depth observed at enqueue time (0 in sync mode). */
  uint64_t
  EnqueueSubmission(Submission &&sub) {
    if (sync_encode_) {
      ProcessSubmission(sub);
      return 0;
    }
    uint64_t qdepth;
    {
      std::unique_lock<dxmt::mutex> lock(mutex_worker_);
      qdepth = worker_queue_.size();
      worker_queue_.push_back(std::move(sub));
    }
    cv_worker_.notify_one();
    return qdepth;
  }

public:
  MTLD3D12CommandQueueImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory>(pDevice),
      inflight_cmdbuf_wait_thread_([this]() { this->CommandBufferWaitingThread(); }) {
    sync_encode_ = env::getEnvVar("DXMT_D3D12_SYNC_ENCODE") == "1";
    if (!sync_encode_)
      encode_worker_thread_ = dxmt::thread([this]() { this->EncodeWorkerThread(); });
  }

  ~MTLD3D12CommandQueueImpl() {
    // drain and stop the encode worker first, so no further commits happen
    if (encode_worker_thread_.joinable()) {
      {
        std::unique_lock<dxmt::mutex> lock(mutex_worker_);
        worker_stop_ = true;
      }
      cv_worker_.notify_all();
      encode_worker_thread_.join();
    }
    std::lock_guard<dxmt::mutex> lock(mutex_commit_);
    inflight_cmdbuf_stop_.store(inflight_cmdbuf_seq_.fetch_add(1));
    inflight_cmdbuf_seq_.notify_one();
    inflight_cmdbuf_wait_thread_.join();
  }

  HRESULT
  Initialize(const D3D12_COMMAND_QUEUE_DESC *pDesc) {
    // TODO: validate and normalize
    desc_ = *pDesc;
    desc_.NodeMask = 1; // typically 1 GPU only

    auto metal_device = device_->GetMTLDevice();
    queue_ = metal_device.newCommandQueue(kCommandQueueSize);
    if (!queue_)
      return E_FAIL;
    queue_.addResidencySet(device_->GetGlobalResidencySet());

    fence_ = metal_device.newFence();

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandQueue)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IMTLSwapChainFactory)) {
      *ppvObject = ref_and_cast<IMTLSwapChainFactory>(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12CommandQueue), riid)) {
      WARN("D3D12CommandQueue: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  void STDMETHODCALLTYPE UpdateTileMappings(
      ID3D12Resource *resource, UINT region_count, const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
      const D3D12_TILE_REGION_SIZE *region_sizes, ID3D12Heap *heap, UINT range_count,
      const D3D12_TILE_RANGE_FLAGS *range_flags, const UINT *heap_range_offsets, const UINT *range_tile_counts,
      D3D12_TILE_MAPPING_FLAGS flags
  ) {
    // tiled resources unsupported; see GetResourceTiling
    static bool warned = false;
    if (!std::exchange(warned, true))
      WARN("UpdateTileMappings: ignored (tiled resources not supported)");
  };

  void STDMETHODCALLTYPE CopyTileMappings(
      ID3D12Resource *dst_resource, const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
      ID3D12Resource *src_resource, const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags
  ) {
    static bool warned = false;
    if (!std::exchange(warned, true))
      WARN("CopyTileMappings: ignored (tiled resources not supported)");
  };

  void STDMETHODCALLTYPE
  ExecuteCommandLists(UINT Count, ID3D12CommandList *const *ppCommandLists) {
    Submission sub;
    sub.type = Submission::Type::Execute;
    sub.chains.reserve(Count);
    for (unsigned i = 0; i < Count; i++) {
      auto pCommandList = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
      // snapshot past the Null head node: the head lives in the allocator's
      // encoder list vector which may relocate on later recordings, while
      // the rest of the chain lives in stable bump-heap memory
      sub.chains.push_back(pCommandList->entry ? pCommandList->entry->next : nullptr);
    }
    EnqueueSubmission(std::move(sub));
  };

  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE EndEvent() {};

  HRESULT STDMETHODCALLTYPE
  Signal(ID3D12Fence *pFence, UINT64 Value) {
    Submission sub;
    sub.type = Submission::Type::Signal;
    sub.fence = static_cast<MTLD3D12Fence *>(pFence)->fence;
    sub.fence_value = Value;
    EnqueueSubmission(std::move(sub));
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  Wait(ID3D12Fence *pFence, UINT64 Value) {
    Submission sub;
    sub.type = Submission::Type::Wait;
    sub.fence = static_cast<MTLD3D12Fence *>(pFence)->fence;
    sub.fence_value = Value;
    EnqueueSubmission(std::move(sub));
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetTimestampFrequency(UINT64 *pFrequency) {
    // FIXME: stub
    if (pFrequency)
      *pFrequency = 1;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) {
    return E_NOTIMPL;
  };

  D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_COMMAND_QUEUE_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };

  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGISwapChain1 **ppSwapChain
  ) {
    return dxmt::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain);
  }

  HRESULT
  Present(
      Presenter *presenter, ID3D12Resource *backbuffer, HANDLE hLantecyWaitable, double after,
      SpatialScaler *pScaler, ID3D12Resource *pUpscaled
  ) {
    Submission sub;
    sub.type = Submission::Type::Present;
    sub.presenter = presenter;
    sub.backbuffer = backbuffer;
    sub.scaler = pScaler;
    sub.upscaled = pUpscaled;
    sub.semaphore = hLantecyWaitable;
    sub.present_after = after;
    uint64_t qdepth = EnqueueSubmission(std::move(sub));

    FrameLogTick(qdepth);

    return S_OK;
  }

  // Per-frame CSV logger, enabled by env DXMT_FRAME_LOG=<path-prefix>.
  // Slimmer than the d3d11 variant: no FrameStatistics on this path, so only
  // present-to-present delta, in-flight command buffer count and PSO compiles.
  // Always called on the thread calling Present; in async mode the encode
  // stats are produced on the worker, so their per-frame deltas may shift by
  // a frame relative to the synchronous mode.
  void
  FrameLogTick(uint64_t qdepth) {
    if (unlikely(!frame_log_checked_)) {
      frame_log_checked_ = true;
      if (const char *prefix = std::getenv("DXMT_FRAME_LOG")) {
        char path[1024];
        std::snprintf(path, sizeof(path), "%s-d3d12-%d.csv", prefix, (int)getpid());
        frame_log_ = std::fopen(path, "w");
        if (frame_log_) {
          std::setvbuf(frame_log_, nullptr, _IOFBF, 1 << 20);
          std::fputs("frame,dt_us,inflight,compiles,encode_us,submits,encoders,qdepth\n", frame_log_);
        }
      }
    }
    if (likely(!frame_log_))
      return;
    auto now = std::chrono::steady_clock::now();
    if (frame_log_frame_ > 0) {
      long dt_us = (long)std::chrono::duration_cast<std::chrono::microseconds>(now - frame_log_last_).count();
      uint64_t compiles_total = g_compiled_shader_variants.load(std::memory_order_relaxed);
      uint64_t encode_total = stat_encode_us_.load(std::memory_order_relaxed);
      uint64_t submits_total = stat_submits_.load(std::memory_order_relaxed);
      uint64_t encoders_total = stat_encoders_.load(std::memory_order_relaxed);
      std::fprintf(
          frame_log_, "%llu,%ld,%llu,%llu,%llu,%llu,%llu,%llu\n", (unsigned long long)frame_log_frame_, dt_us,
          (unsigned long long)inflight_cmdbuf_count_.load(std::memory_order_relaxed),
          (unsigned long long)(compiles_total - frame_log_compiles_),
          (unsigned long long)(encode_total - frame_log_encode_us_),
          (unsigned long long)(submits_total - frame_log_submits_),
          (unsigned long long)(encoders_total - frame_log_encoders_),
          (unsigned long long)qdepth);
      frame_log_compiles_ = compiles_total;
      frame_log_encode_us_ = encode_total;
      frame_log_submits_ = submits_total;
      frame_log_encoders_ = encoders_total;
      if ((frame_log_frame_ & 0xff) == 0)
        std::fflush(frame_log_);
    } else {
      // baseline so the first row doesn't absorb all load-time compiles
      frame_log_compiles_ = g_compiled_shader_variants.load(std::memory_order_relaxed);
    }
    frame_log_last_ = now;
    frame_log_frame_++;
  }
};

HRESULT
CreateCommandQueue(MTLD3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
  auto command_queue = Com(new MTLD3D12CommandQueueImpl(pDevice));
  HRESULT hr = command_queue->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  return command_queue->QueryInterface(riid, ppCommandQueue);
};

} // namespace dxmt
