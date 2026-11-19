#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -d .deps/usr ]]; then source tools/dev-env.sh; fi
cmake --build build --parallel "${SNAPLING_JOBS:-4}"
cmake --install build --strip --prefix "$PWD/dist/SnapLing"
python3 tools/bundle-linux.py dist/SnapLing
cp README.md ARCHITECTURE.md BUILD.md TESTING.md LIMITATIONS.md THIRD_PARTY.md LICENSE dist/SnapLing/share/snapling/
tar -C dist -czf dist/SnapLing-0.1.0-linux-$(uname -m).tar.gz SnapLing
# DEB 使用目标系统的 Qt 和 OCR 库，避免从打包机复制 glibc。
(cd build && cpack -G DEB -D CPACK_DEBIAN_PACKAGE_SHLIBDEPS=OFF -D 'CPACK_DEBIAN_PACKAGE_DEPENDS=libqt6widgets6 (>= 6.4), libqt6network6 (>= 6.4), libqt6concurrent6, libqt6dbus6, libtesseract5, libx11-6, libxrandr2, libxcb1, libpipewire-0.3-0t64, libzxing3, qt6-wayland, qt6-qpa-plugins, fonts-noto-cjk')
cp build/snapling-*.deb dist/
