#!/usr/bin/env bash
# source tools/dev-env.sh，然后 cmake / ctest。正式安装不依赖本脚本。
SNAPLING_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export CMAKE_PREFIX_PATH="$SNAPLING_ROOT/.deps/usr${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PKG_CONFIG_PATH="$SNAPLING_ROOT/.deps/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$SNAPLING_ROOT/.deps/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export TESSDATA_PREFIX="$SNAPLING_ROOT/.deps/usr/share/tesseract-ocr/5/tessdata"
export PATH="$SNAPLING_ROOT/.deps/usr/bin:$PATH"
