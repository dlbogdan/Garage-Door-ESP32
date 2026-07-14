#!/bin/zsh
set -e

ACTIVATE_IDF="/Users/dlbogdan/.espressif/tools/activate_idf_v5.5.4.sh"

if [ ! -r "$ACTIVATE_IDF" ]; then
  echo "error: cannot read $ACTIVATE_IDF" >&2
  exit 1
fi

# shellcheck disable=SC1090
. "$ACTIVATE_IDF"
set -u
"$(dirname "$0")/verify-environment.sh"

UI_ASSET="components/provisioning/assets/app.js"
if [ -f webui/package-lock.json ] && {
     [ ! -f "$UI_ASSET" ] ||
     [ -n "$(find webui/src webui/package.json webui/package-lock.json webui/vite.config.js webui/index.html -newer "$UI_ASSET" -print -quit 2>/dev/null)" ];
   }; then
  "$(dirname "$0")/build-ui.sh"
else
  echo "Embedded UI assets are current; skipping npm install/check/build"
fi

if [ ! -f sdkconfig ] || ! grep -q '^CONFIG_IDF_TARGET="esp32"$' sdkconfig; then
  python "$IDF_PATH/tools/idf.py" set-target esp32
else
  echo "ESP-IDF target is already esp32; preserving incremental build cache"
fi
python "$IDF_PATH/tools/idf.py" build "$@"
