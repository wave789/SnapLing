#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -d .deps/usr ]]; then source tools/dev-env.sh; fi
if [[ ! -x build/snapling ]]; then
    printf '%s\n' '找不到 build/snapling，请先运行 ./tools/build.sh。' >&2
    exit 1
fi
snapling_prepare_desktop=1
for snapling_argument in "$@"; do
    case "$snapling_argument" in
        --quit|-h|--help|--help-all|-v|--version) snapling_prepare_desktop=0 ;;
    esac
done
if (( snapling_prepare_desktop )); then
    ./tools/install-desktop.sh
fi
exec build/snapling "$@"
