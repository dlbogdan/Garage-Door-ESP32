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

if [ -f webui/package-lock.json ]; then
  "$(dirname "$0")/build-ui.sh"
fi

python "$IDF_PATH/tools/idf.py" set-target esp32
python "$IDF_PATH/tools/idf.py" build "$@"
