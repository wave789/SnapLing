#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -d .deps/usr ]]; then source tools/dev-env.sh; fi
export QT_QPA_PLATFORMTHEME=generic
export QT_QPA_PLATFORM=xcb
export SNAPLING_TEST_X11=1
export XDG_SESSION_TYPE=x11
xvfb-run -a -s '-screen 0 1920x1080x24' build/snapling_desktop_test "$@"
