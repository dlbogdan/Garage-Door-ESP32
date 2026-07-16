#!/bin/zsh
set -eu

if [ ! -f webui/package-lock.json ]; then
  echo "error: webui/package-lock.json does not exist yet" >&2
  exit 1
fi

cd webui
npm ci
npm run check
npm test -- --run
npm run build

ASSET_DIR="../components/management_server/assets"
mkdir -p "$ASSET_DIR"
cp dist/index.html dist/app.js dist/app.css "$ASSET_DIR/"
rm -f "$ASSET_DIR/app 2.js" "$ASSET_DIR/app 2.css"
