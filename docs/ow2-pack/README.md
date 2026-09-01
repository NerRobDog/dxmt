# DXMT ow2-pack v0.2

DXMT build with memory-footprint fixes, tuned for **Overwatch 2 under CrossOver on Apple Silicon**.
Based on upstream DXMT v0.80 (commit `19e24ee`) plus this fork's patches.

**What's different from stock DXMT / CrossOver 26.3:**

- `d3d11.releaseShaderIR` (default **on**): DXMT no longer keeps the parsed IR of every
  shader resident forever. On OW2 this cuts the game process footprint from ~13 GB to ~4 GB
  on a 16 GB Mac and eliminates swap-induced stutter (killcam/respawn hitches).
- `dxgi.customVideoMemory`: optional cap for the VRAM amount reported to the game.
- `DXMT_FRAME_LOG=<path-prefix>`: per-frame CSV telemetry (frame interval, encode/drawable/
  latency breakdown, shader compiles per frame) — near-zero overhead, great for hunting stutter.
- Everything else is upstream DXMT v0.80 behaviour.

Source code: this repository, tag of this release (LGPL 2.1 — see COPYING.LIB in the repo).

## Install (CrossOver)

**Easy way:** quit all CrossOver apps (incl. Battle.net), then from the unpacked package:
```
./install.sh
```
It backs up the stock DXMT libs to `~/dxmt-stock-backup` first. Rollback anytime with `./uninstall.sh`.
Then do steps 4–6 below (env vars + config). Manual install:

1. Quit the game, Battle.net and make sure `wineserver` is not running.
2. Back up the stock files:
   ```
   cp -R "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/dxmt" ~/dxmt-stock-backup
   ```
3. Copy this package over it (from the package directory):
   ```
   CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/dxmt"
   cp i386-windows/*   "$CX/i386-windows/"
   cp x86_64-unix/winemetal.so "$CX/x86_64-unix/"
   cp x86_64-windows/* "$CX/x86_64-windows/"
   ```
4. In the bottle's `cxbottle.conf` `[EnvironmentVariables]` make sure you have at least:
   ```
   "CX_GRAPHICS_BACKEND" = "dxmt"
   "DXMT_USE_DEFAULT_METAL_CACHE" = "1"
   "DXMT_CONFIG_FILE" = "/Users/YOU/dxmt.conf"
   ```
5. Copy `dxmt.conf.example` somewhere, point `DXMT_CONFIG_FILE` at it, adjust to taste.
6. In-game (OW2): use DirectX 11, uncap the in-game FPS limit (e.g. 120/300) and let
   `d3d11.preferredMaxFrameRate` do the pacing.

**Rollback:** copy your backup back over `$CX`.

CrossOver updates overwrite these files — re-apply after updating CrossOver.

## Recommended config (see dxmt.conf.example)

```
d3d11.preferredMaxFrameRate = 60      # display-rate divisor; flat frametime
d3d11.releaseShaderIR = True          # the memory fix (default True in this build)
d3d11.metalSpatialUpscaleFactor = 1.33 # optional: sharper output via MetalFX
                                       # requires env DXMT_METALFX_SPATIAL_SWAPCHAIN=1
```

**Shader cache expectations:** the persistent cache starts empty and builds as you play.
Every new hero, skin, ability effect, and map introduces shader variants that compile once
(a brief hitch) and are cached forever. The first sessions hitch noticeably more; after a
few days of varied play the cache converges and the game stays smooth. Just keep playing —
it gets better on its own.

---

## Кратко по-русски

Сборка DXMT для Overwatch 2 под CrossOver на Apple Silicon. Главное отличие от стока:
`d3d11.releaseShaderIR` — DXMT больше не держит распарсенный IR всех шейдеров вечно;
на 16 ГБ Маке футпринт игры падает с ~13 до ~4 ГБ, своп и связанные с ним фризы
(киллкам, респаун) исчезают. Установка: закрыть все приложения CrossOver и запустить `./install.sh`
(сам сделает бэкап), затем прописать `DXMT_CONFIG_FILE` и конфиг из
`dxmt.conf.example`. Откат — `./uninstall.sh`.
Про кэш шейдеров: он наполняется игрой — каждый новый герой, скин, способность,
карта компилируются один раз (короткий фриз) и навсегда остаются в кэше. Первые
сессии фризят заметнее, через несколько дней разнообразной игры всё устаканивается.
Апдейт CrossOver перезатирает файлы — накатить пакет заново.
