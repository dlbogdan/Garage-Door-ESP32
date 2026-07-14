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
