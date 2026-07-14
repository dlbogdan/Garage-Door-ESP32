#!/bin/zsh
set -eu

EXPECTED_IDF_VERSION="v5.5.4"

if [ -z "${IDF_PATH:-}" ]; then
  echo "error: IDF_PATH is unset" >&2
  echo "source /Users/dlbogdan/.espressif/tools/activate_idf_v5.5.4.sh" >&2
  exit 1
fi

IDF_PY="$IDF_PATH/tools/idf.py"
if [ ! -r "$IDF_PY" ]; then
  echo "error: cannot read $IDF_PY" >&2
  exit 1
fi

actual_version="$(python "$IDF_PY" --version | awk '{print $2}')"
if [ "$actual_version" != "$EXPECTED_IDF_VERSION" ]; then
  echo "error: ESP-IDF $EXPECTED_IDF_VERSION is required; found $actual_version" >&2
  exit 1
fi

target="$(python "$IDF_PY" --list-targets 2>/dev/null | grep -x esp32 || true)"
if [ "$target" != "esp32" ]; then
  echo "error: the installed ESP-IDF does not report the esp32 target" >&2
  exit 1
fi

echo "ESP-IDF environment verified: $actual_version, target esp32"
