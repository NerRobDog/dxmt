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

# ---- bottle setup (optional, interactive) ----
BOTTLES_DIR="$HOME/Library/Application Support/CrossOver/Bottles"
CONF_DST="$HOME/dxmt.conf"

if [ ! -f "$CONF_DST" ]; then
  cp "$HERE/dxmt.conf.example" "$CONF_DST"
  echo "Config installed: $CONF_DST"
else
  echo "Config already exists: $CONF_DST (keeping yours)"
fi

shopt -s nullglob
BOTTLES=("$BOTTLES_DIR"/*/cxbottle.conf)
if [ ${#BOTTLES[@]} -gt 0 ]; then
  bold "Set up a bottle to use DXMT + this config?"
  i=1
  for b in "${BOTTLES[@]}"; do
    echo "  $i) $(basename "$(dirname "$b")")"
    i=$((i+1))
  done
  echo "  0) skip (set env vars manually)"
  printf "Choice: "; read -r CH
  if [ "${CH:-0}" -ge 1 ] 2>/dev/null && [ "$CH" -le "${#BOTTLES[@]}" ]; then
    BC="${BOTTLES[$((CH-1))]}"
    cp "$BC" "$BC.bak-dxmt"
    add_env() { # append key to [EnvironmentVariables] only if missing
      grep -q "^\"$1\"" "$BC" || printf '"%s" = "%s"\n' "$1" "$2" >> "$BC"
    }
    grep -q '^\[EnvironmentVariables\]' "$BC" || printf '\n[EnvironmentVariables]\n' >> "$BC"
    add_env CX_GRAPHICS_BACKEND dxmt
    add_env DXMT_USE_DEFAULT_METAL_CACHE 1
    add_env DXMT_CONFIG_FILE "$CONF_DST"
    echo "Bottle configured: $(basename "$(dirname "$BC")") (backup: cxbottle.conf.bak-dxmt)"
  else
    echo "Skipped bottle setup."
  fi
fi

bold "Done. Final steps:"
cat <<'EOF'
1. Fully restart Battle.net (env vars are read when the bottle starts).
2. In game: DirectX 11, uncap the in-game FPS limit (e.g. 120/300) —
   the config paces frames to a flat 60.

First days: the shader cache starts empty. New heroes/skins/maps compile once
(a brief hitch) and are cached forever — it gets smoother the more you play.

NOTE: a CrossOver update overwrites the installed libs — just run install.sh again.
Rollback anytime: bash uninstall.sh
EOF
