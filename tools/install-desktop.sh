#!/usr/bin/env bash
# Prepare only this source tree's user launcher, before Qt or Portal looks up the app ID.
set -euo pipefail
cd "$(dirname "$0")/.."
snapling_source_root="$PWD"
if [[ -d .deps/usr ]]; then source tools/dev-env.sh; fi
if [[ ! -x build/snapling ]]; then
    printf '%s\n' '找不到 build/snapling，请先运行 ./tools/build.sh。' >&2
    exit 1
fi
snapling_data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
if [[ "$snapling_data_home" != /* ]]; then
    printf '%s\n' 'XDG_DATA_HOME 必须是绝对路径，无法准备 SnapLing 桌面入口。' >&2
    exit 1
fi
# This is a live user launcher, never a package staging install.
# Point to run.sh so desktop-menu launches also load any local development libraries.
if ! env -u DESTDIR cmake \
    -DSNAPLING_DESKTOP_SOURCE_RUN=ON \
    "-DSNAPLING_DESKTOP_EXECUTABLE=$snapling_source_root/tools/run.sh" \
    "-DSNAPLING_DESKTOP_ICON=$snapling_source_root/resources/snapling.svg" \
    "-DSNAPLING_DESKTOP_DESTINATION=$snapling_data_home/applications" \
    "-DSNAPLING_DESKTOP_STAGE=$snapling_source_root/build/install-source-desktop" \
    -P packaging/linux/desktop-entry.cmake >/dev/null; then
    printf '无法准备 SnapLing 桌面入口：%s/applications/io.snapling.SnapLing.desktop\n' "$snapling_data_home" >&2
    exit 1
fi
