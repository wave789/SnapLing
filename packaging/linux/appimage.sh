#!/usr/bin/env bash
# linuxdeploy 和 Qt 插件从官方发行页取得，校验后放入 PATH。
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v linuxdeploy >/dev/null
command -v linuxdeploy-plugin-qt >/dev/null
cmake --install build --prefix "$PWD/dist/AppDir/usr"
export QMAKE="${QMAKE:-qmake6}"
export EXTRA_QT_PLUGINS="wayland-graphics-integration-client;wayland-shell-integration;wayland-decoration-client"
linuxdeploy --appdir dist/AppDir --desktop-file packaging/linux/io.snapling.SnapLing.desktop --icon-file resources/snapling.svg --plugin qt --output appimage
