#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -d .deps/usr ]]; then source tools/dev-env.sh; fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build --parallel "${SNAPLING_JOBS:-4}"
if [[ "${SNAPLING_SKIP_OCR_RUNTIME:-0}" != 1 ]]; then
    python3 tools/prepare-paddleocr-runtime.py --output build/paddleocr-runtime
fi
ctest --test-dir build --output-on-failure
