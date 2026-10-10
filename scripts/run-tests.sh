#!/usr/bin/env bash
# Runs AstraTest for one configuration -- the POSIX twin of
# scripts/run-tests.ps1, with the same arguments:
#
#   scripts/run-tests.sh <Debug|Release|Dist> [--rng-seed N]
#
# Without --rng-seed the suite runs in declaration order; with it, GoogleTest
# shuffles test order with seed N (1..99999), so a failure reproduces by
# passing the same seed. JUnit XML goes to test-results/. Run
# scripts/build.sh <Config> first.
set -euo pipefail

usage() { echo "usage: scripts/run-tests.sh <Debug|Release|Dist> [--rng-seed N]" >&2; exit 2; }
[ $# -ge 1 ] || usage
CONFIG="$1"; shift
case "$CONFIG" in Debug|Release|Dist) ;; *) usage ;; esac

SEED=""
while [ $# -gt 0 ]; do
    case "$1" in
        --rng-seed) [ $# -ge 2 ] || usage; SEED="$2"; shift 2 ;;
        *) usage ;;
    esac
done
if [ -n "$SEED" ]; then
    case "$SEED" in ''|*[!0-9]*) usage ;; esac
    if [ "$SEED" -lt 1 ] || [ "$SEED" -gt 99999 ]; then
        echo "run-tests.sh: --rng-seed must be 1..99999 (GoogleTest's range)" >&2; exit 2
    fi
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "$(uname -s)" in
    Linux)  PLATFORM=linux ;;
    Darwin) PLATFORM=macosx ;;
    *) echo "run-tests.sh: unsupported host $(uname -s)" >&2; exit 1 ;;
esac

# bin/<Config>-<system>-<architecture>/AstraTest/AstraTest
BINARY=""
for candidate in "$ROOT/bin/$CONFIG-$PLATFORM-"*/AstraTest/AstraTest; do
    [ -x "$candidate" ] || continue
    if [ -n "$BINARY" ]; then
        echo "run-tests.sh: more than one $CONFIG AstraTest under bin/ ($BINARY, $candidate)" >&2; exit 1
    fi
    BINARY="$candidate"
done
if [ -z "$BINARY" ]; then
    echo "run-tests.sh: no $CONFIG AstraTest under bin/ -- run scripts/build.sh $CONFIG first" >&2; exit 1
fi

mkdir -p "$ROOT/test-results"
REPORT="$ROOT/test-results/AstraTest-$PLATFORM-$CONFIG${SEED:+-seed$SEED}.xml"
ARGS=(--gtest_brief=1 "--gtest_output=xml:$REPORT")
[ -n "$SEED" ] && ARGS+=(--gtest_shuffle "--gtest_random_seed=$SEED")

cd "$ROOT"
echo "run-tests.sh: $BINARY ${ARGS[*]}"
exec "$BINARY" "${ARGS[@]}"
