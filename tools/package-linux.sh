#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -d .deps/usr ]]; then source tools/dev-env.sh; fi
cmake --build build --parallel "${SNAPLING_JOBS:-4}"
python3 tools/prepare-paddleocr-runtime.py --output build/paddleocr-runtime
cmake --install build --strip --prefix "$PWD/dist/SnapLing"
python3 tools/bundle-linux.py dist/SnapLing
cp README.md ARCHITECTURE.md BUILD.md TESTING.md LIMITATIONS.md THIRD_PARTY.md LICENSE dist/SnapLing/share/snapling/
tar -C dist -czf dist/SnapLing-0.5.4-linux-$(uname -m).tar.gz SnapLing
# 保留系统资源的可读权限，并从实际构建推导运行库依赖。
(cd build && umask 022 && cpack -G DEB)
cp build/snapling-*.deb dist/
