#!/bin/zsh
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$ROOT/test/host" -B "$ROOT/build-host" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$ROOT/build-host"
ctest --test-dir "$ROOT/build-host" --output-on-failure

