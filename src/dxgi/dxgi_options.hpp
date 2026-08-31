#pragma once

#include "config/config.hpp"

namespace dxmt {

/**
 * \brief DXGI options
 *
 * Per-app options that control the
 * behaviour of some DXGI classes.
 */
struct DxgiOptions {
  DxgiOptions(const Config &config);

  /// Override PCI vendor and device IDs reported to the
  /// application. This may make apps think they are running
  /// on a different GPU than they do and behave differently.
  int32_t customVendorId;
  int32_t customDeviceId;
  std::string customDeviceDesc;
  bool forceSDR;

  /// Override the amount of video memory reported to the
  /// application, in megabytes. Affects both the adapter
  /// descriptor and the budget returned by
  /// QueryVideoMemoryInfo. 0 or negative keeps the default
  /// behaviour. Useful on unified-memory systems where the
  /// default budget lets games oversubscribe system RAM.
  int32_t customVideoMemory;
};

} // namespace dxmt