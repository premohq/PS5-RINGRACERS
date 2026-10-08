#!/usr/bin/env bash
# Build Dr. Robotnik's Ring Racers for the PlayStation 5, from nothing.
#
#   tools/ps5/build.sh [--assets /path/to/RingRacers] [-j N]
#
# Fetches the toolchain (tools/ps5/deps.sh), configures and builds with
# cmake/ps5/Toolchain-PS5.cmake into build/ps5, and packages the title
# (tools/ps5/package.sh). See docs/PS5.md.

set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${PS5_BUILD_DIR:-"$repo/build/ps5"}
jobs=$(nproc 2>/dev/null || echo 4)
package_args=()

while (( $# > 0 )); do
	case "$1" in
		--assets) package_args+=(--assets "${2:?}"); shift 2 ;;
		-j) jobs=${2:?}; shift 2 ;;
		-j*) jobs=${1#-j}; shift ;;
		*) echo "usage: $0 [--assets /path/to/RingRacers] [-j N]" >&2; exit 2 ;;
	esac
done

command -v cmake >/dev/null || { echo "missing required command: cmake" >&2; exit 2; }
command -v ninja >/dev/null || { echo "missing required command: ninja" >&2; exit 2; }

# A plain assignment, so that a failure in deps.sh stops this script.
deps_env=$("$repo/tools/ps5/deps.sh")
eval "$deps_env"

cmake -S "$repo" -B "$build" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$repo/cmake/ps5/Toolchain-PS5.cmake" \
	-DPS5_PAYLOAD_SDK="$PS5_PAYLOAD_SDK" \
	-DPS5_OPENGL_PREFIX="$PS5_OPENGL_PREFIX" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DSRB2_SDL2_EXE_NAME=ringracers

cmake --build "$build" -j "$jobs"

PS5_BUILD_DIR="$build" "$repo/tools/ps5/package.sh" "${package_args[@]}"
