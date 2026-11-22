#!/usr/bin/env bash
# Install the current Linux build for this user and expose its desktop launcher to GNOME.
set -euo pipefail
cd "$(dirname "$0")/.."

snapling_prefix="$HOME/.local/snapling"
if [[ "$#" == 1 && "$1" == --help ]]; then
    printf '用法: %s [--prefix 绝对路径]\n默认安装目录: %s\n' "$0" "$snapling_prefix"
    exit 0
fi
if (( $# )); then
    if [[ "$#" != 2 || "$1" != --prefix || "$2" != /* ]]; then
        printf '用法: %s [--prefix 绝对路径]\n' "$0" >&2
        exit 1
    fi
    snapling_prefix="$2"
fi

snapling_data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
if [[ "$snapling_data_home" != /* ]]; then
    printf '%s\n' 'XDG_DATA_HOME 必须是绝对路径。' >&2
    exit 1
fi
if [[ ! -x build/snapling || ! -f build/cmake_install.cmake ]]; then
    printf '%s\n' '找不到可安装的构建，请先运行 ./tools/build.sh。' >&2
    exit 1
fi
snapling_bindir="$(sed -n 's/^CMAKE_INSTALL_BINDIR:[^=]*=//p' build/CMakeCache.txt)"
if [[ "$snapling_bindir" != bin ]]; then
    printf '%s\n' '用户安装使用 bin 目录。请用 ./tools/build.sh -DCMAKE_INSTALL_BINDIR=bin 重新生成构建。' >&2
    exit 1
fi

if [[ -d .deps/usr ]]; then source tools/dev-env.sh; fi
python3 tools/prepare-paddleocr-runtime.py --output build/paddleocr-runtime
mkdir -p -- "$snapling_prefix"
snapling_prefix="$(cd "$snapling_prefix" && pwd -P)"
env -u DESTDIR cmake --install build --prefix "$snapling_prefix"
snapling_executable="$snapling_prefix/bin/snapling"
if [[ -d .deps/usr ]]; then
    # Install runtime libraries alongside development builds so menu launches work outside the repo.
    python3 tools/bundle-linux.py "$snapling_prefix"
    snapling_executable="$snapling_prefix/snapling"
fi
snapling_applications="$snapling_prefix/share/applications"
env -u DESTDIR cmake \
    -DSNAPLING_DESKTOP_SOURCE_RUN=ON \
    "-DSNAPLING_DESKTOP_EXECUTABLE=$snapling_executable" \
    "-DSNAPLING_DESKTOP_ICON=$snapling_prefix/share/icons/hicolor/scalable/apps/snapling.svg" \
    "-DSNAPLING_DESKTOP_DESTINATION=$snapling_applications" \
    "-DSNAPLING_DESKTOP_STAGE=$PWD/build/install-user-desktop" \
    -P packaging/linux/desktop-entry.cmake

mkdir -p -- "$snapling_data_home/applications"
snapling_desktop=io.snapling.SnapLing.desktop
if [[ "$snapling_applications/$snapling_desktop" != "$(cd "$snapling_data_home/applications" && pwd -P)/$snapling_desktop" ]]; then
    ln -sfnT -- "$snapling_applications/$snapling_desktop" "$snapling_data_home/applications/$snapling_desktop"
fi
printf '\nSnapLing 已安装到 %s\n可从应用菜单启动，或运行：%s\n' "$snapling_prefix" "$snapling_executable"
