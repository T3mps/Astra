#!/usr/bin/env bash
# Generates and builds Astra on Linux and macOS -- the POSIX twin of
# scripts/build.ps1, with the same arguments:
#
#   scripts/build.sh <Debug|Release|Dist> [premake options...]
#
#   scripts/build.sh Debug
#   scripts/build.sh Debug --sanitize=address
#   scripts/build.sh Release --no-rtti
#
# Fetches the premake pinned in scripts/premake.lock into .tools/ (SHA-256
# checked), generates GNU makefiles, and builds AstraTest plus the
# AstraCompile16/64 entity-width checks. The compiler comes from CC / CXX
# (default: the generator's, i.e. gcc on Linux and clang on macOS).
set -euo pipefail

usage() { echo "usage: scripts/build.sh <Debug|Release|Dist> [premake options...]" >&2; exit 2; }
[ $# -ge 1 ] || usage
CONFIG="$1"; shift
case "$CONFIG" in
    Debug)   MAKE_CONFIG=debug ;;
    Release) MAKE_CONFIG=release ;;
    Dist)    MAKE_CONFIG=dist ;;
    *) usage ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

case "$(uname -s)" in
    Linux)  PLATFORM=linux ;;
    Darwin) PLATFORM=macosx ;;
    *) echo "build.sh: unsupported host $(uname -s)" >&2; exit 1 ;;
esac

lock_value() { awk -v key="$1" '$1 == key { print $2 }' "$ROOT/scripts/premake.lock"; }
PREMAKE_VERSION="$(lock_value version)"
PREMAKE_SHA256="$(lock_value "$PLATFORM")"
PREMAKE_DIR="$ROOT/.tools/premake/$PREMAKE_VERSION"
PREMAKE="$PREMAKE_DIR/premake5"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{ print $1 }'
    else shasum -a 256 "$1" | awk '{ print $1 }'; fi
}

if [ ! -x "$PREMAKE" ]; then
    URL="https://github.com/premake/premake-core/releases/download/v$PREMAKE_VERSION/premake-$PREMAKE_VERSION-$PLATFORM.tar.gz"
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    echo "build.sh: fetching $URL"
    curl -fsSL --retry 4 -o "$TMP/premake.tar.gz" "$URL"
    ACTUAL="$(sha256_of "$TMP/premake.tar.gz")"
    if [ "$ACTUAL" != "$PREMAKE_SHA256" ]; then
        echo "build.sh: SHA-256 mismatch for $URL (expected $PREMAKE_SHA256, got $ACTUAL) -- refusing it" >&2
        exit 1
    fi
    tar -xzf "$TMP/premake.tar.gz" -C "$TMP"
    mkdir -p "$PREMAKE_DIR"
    install -m 0755 "$TMP/premake5" "$PREMAKE"
fi

cd "$ROOT"
"$PREMAKE" gmake "$@"

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
echo "build.sh: $CONFIG with ${CXX:-the default C++ compiler} on $PLATFORM ($JOBS jobs)"
make config="$MAKE_CONFIG" AstraTest AstraCompile16 AstraCompile64 -j"$JOBS"
