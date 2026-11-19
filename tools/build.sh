#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -d .deps/usr ]]; then source tools/dev-env.sh; fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build --parallel "${SNAPLING_JOBS:-4}"
ctest --test-dir build --output-on-failure
