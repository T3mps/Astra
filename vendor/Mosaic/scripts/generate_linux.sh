#!/usr/bin/env bash
# Generate Linux gmake2 makefiles. Scaffold for the cross-platform burn-down
# (g++/clang green + Linux CI), which is a follow-up. Requires a Linux premake5
# on PATH (the vendored premake5.exe is Windows-only).
set -euo pipefail
premake5 gmake2
