#!/usr/bin/env bash
# Compiles every public Astra header on its own (-fsyntax-only), so a header
# that only builds because something else happened to include its
# dependencies first fails here instead of in a consumer.
#
#   ci/check-headers.sh <c++ compiler> <debug|release>
#
# Flags mirror AstraTest's gcc/clang warning set (premake5.lua).
set -euo pipefail

CXX="${1:?usage: ci/check-headers.sh <c++ compiler> <debug|release>}"
CONFIG="${2:?usage: ci/check-headers.sh <c++ compiler> <debug|release>}"

case "$CONFIG" in
    debug)   DEFINES="-DASTRA_BUILD_DEBUG -D_DEBUG" ;;
    release) DEFINES="-DASTRA_BUILD_RELEASE -DNDEBUG" ;;
    *) echo "unknown config: $CONFIG" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/include"

check_one() {
    local header="$1"
    if ! output="$(printf '#include <%s>\n' "$header" | "$CXX" -std=c++20 -fsyntax-only -mavx \
            -Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers \
            -Wno-unknown-warning-option -Wno-maybe-uninitialized \
            $DEFINES -I. -I"$ROOT/vendor/Mosaic/include" -x c++ - 2>&1)"; then
        printf 'FAIL %s\n%s\n' "$header" "$output"
        return 1
    fi
}
export -f check_one
export CXX DEFINES ROOT

headers=$(find Astra -name '*.hpp' | sort)
count=$(printf '%s\n' "$headers" | wc -l)
if printf '%s\n' "$headers" | xargs -P "$(nproc)" -I{} bash -c 'check_one "$@"' _ {}; then
    echo "check-headers: all $count headers compile standalone ($CXX, $CONFIG)"
else
    echo "check-headers: standalone header failures ($CXX, $CONFIG)" >&2
    exit 1
fi
