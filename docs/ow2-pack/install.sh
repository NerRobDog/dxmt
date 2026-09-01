#!/bin/bash
# DXMT ow2-pack installer for CrossOver on Apple Silicon.
# Backs up the stock DXMT libs, then installs this package's binaries.
set -euo pipefail

CX_APP="${CROSSOVER_APP:-/Applications/CrossOver.app}"
CX_LIB="$CX_APP/Contents/SharedSupport/CrossOver/lib/dxmt"
HERE="$(cd "$(dirname "$0")" && pwd)"
BACKUP=~/dxmt-stock-backup

bold() { printf '\033[1m%s\033[0m\n' "$*"; }

[ -d "$CX_LIB" ] || { echo "ERROR: $CX_LIB not found. Is CrossOver installed? (set CROSSOVER_APP to override)"; exit 1; }
[ -f "$HERE/x86_64-windows/d3d11.dll" ] || { echo "ERROR: run this from the unpacked package directory."; exit 1; }

CX_VER=$(defaults read "$CX_APP/Contents/Info" CFBundleShortVersionString 2>/dev/null || echo "unknown")
echo "CrossOver: $CX_VER at $CX_APP"
case "$CX_VER" in
  26.*) ;;
  *) echo "WARNING: tested with CrossOver 26.x, you have $CX_VER. Continue at your own risk (Ctrl-C to abort, Enter to continue)"; read -r ;;
esac

if pgrep -q wineserver; then
  echo "ERROR: wineserver is running. Quit all CrossOver apps (incl. Battle.net) first."; exit 1
fi

if [ ! -d "$BACKUP" ]; then
  bold "Backing up stock DXMT -> $BACKUP"
  cp -R "$CX_LIB" "$BACKUP"
else
  echo "Backup already exists at $BACKUP (keeping it — that's your original CrossOver DXMT)"
fi

bold "Installing..."
cp "$HERE"/i386-windows/*   "$CX_LIB/i386-windows/"
cp "$HERE"/x86_64-windows/* "$CX_LIB/x86_64-windows/"
cp "$HERE"/x86_64-unix/winemetal.so "$CX_LIB/x86_64-unix/"

bold "Done. Next steps:"
cat <<'EOF'
1. Copy dxmt.conf.example somewhere, e.g.:  cp dxmt.conf.example ~/dxmt.conf
2. In your bottle's cxbottle.conf, [EnvironmentVariables] section, ensure:
     "CX_GRAPHICS_BACKEND" = "dxmt"
     "DXMT_USE_DEFAULT_METAL_CACHE" = "1"
     "DXMT_CONFIG_FILE" = "/Users/YOU/dxmt.conf"
3. Fully restart Battle.net (env vars are read at bottle start).
4. In game: DirectX 11, uncap in-game FPS (e.g. 120/300); pacing comes from the config.

NOTE: a CrossOver update overwrites these files — just run install.sh again.
Rollback anytime: ./uninstall.sh
EOF
