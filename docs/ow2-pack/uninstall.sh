#!/bin/bash
# Restores the stock CrossOver DXMT libs from the backup made by install.sh.
set -euo pipefail

CX_APP="${CROSSOVER_APP:-/Applications/CrossOver.app}"
CX_LIB="$CX_APP/Contents/SharedSupport/CrossOver/lib/dxmt"
BACKUP=~/dxmt-stock-backup

[ -d "$BACKUP" ] || { echo "ERROR: no backup at $BACKUP — nothing to restore."; exit 1; }
[ -d "$CX_LIB" ] || { echo "ERROR: $CX_LIB not found."; exit 1; }

if pgrep -q wineserver; then
  echo "ERROR: wineserver is running. Quit all CrossOver apps first."; exit 1
fi

cp -R "$BACKUP"/. "$CX_LIB"/
echo "Stock DXMT restored from $BACKUP."
echo "You can remove the backup with: rm -rf $BACKUP"
