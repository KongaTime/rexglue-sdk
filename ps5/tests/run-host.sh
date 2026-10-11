#!/usr/bin/env bash
# PS5_ReXGlue's PS5 tests on the build host: src/core/memory_ps5.cpp against
# PS5_PayloadSDK's platform layer (shm.c, placement.c) and its host model of
# the console's kernel (platform/tests/host_kernel.c).
#
#   ps5/tests/run-host.sh
#
# PS5_PayloadSDK is the checkout beside this repository (../PS5_PayloadSDK) or
# PS5_PAYLOAD_SDK_FORK. Needs a C compiler and clang++ with C++23, and the
# fmt and simde submodules.
#
# SPDX-License-Identifier: BSD-3-Clause
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk_fork=${PS5_PAYLOAD_SDK_FORK:-$(dirname "$root")/PS5_PayloadSDK}
platform=$sdk_fork/platform
[[ -f $platform/src/shm.c && -f $platform/tests/host_kernel.c ]] ||
    { echo "error: no PS5_PayloadSDK platform layer at $platform (set PS5_PAYLOAD_SDK_FORK)" >&2; exit 1; }
[[ -f $root/thirdparty/fmt/include/fmt/format.h ]] ||
    { echo "error: the fmt submodule is not checked out" >&2; exit 1; }
cc=${CC:-cc}
cxx=${CXX:-clang++}
out=$root/out/ps5-tests
mkdir -p "$out"

"$cc" -std=gnu11 -O1 -g -I"$platform/include" -I"$platform/src" -pthread \
    -c "$platform/src/shm.c" -o "$out/shm.o"
"$cc" -std=gnu11 -O1 -g -I"$platform/include" -I"$platform/src" -pthread \
    -c "$platform/src/placement.c" -o "$out/placement.o"
"$cc" -std=gnu11 -O1 -g -I"$platform/include" -I"$platform/src" -pthread \
    -c "$platform/tests/host_kernel.c" -o "$out/host_kernel.o"
# REX_PLATFORM_PS5 by hand: the host is not the console, but memory_ps5.cpp is
# what is under test
cxxflags=(-std=c++23 -O1 -g -Wall -Wextra -DREX_PLATFORM_PS5=1 -DFMT_HEADER_ONLY
    -I"$root/ps5/tests/stub" -I"$root/include" -I"$root/thirdparty/fmt/include"
    -I"$root/thirdparty/simde" -I"$platform/include" -I"$platform/tests")
"$cxx" "${cxxflags[@]}" -c "$root/src/core/memory_ps5.cpp" -o "$out/memory_ps5.o"
"$cxx" "${cxxflags[@]}" -c "$root/ps5/tests/memory_ps5_test.cpp" -o "$out/memory_ps5_test.o"
"$cxx" -o "$out/memory_ps5_test" "$out/memory_ps5_test.o" "$out/memory_ps5.o" \
    "$out/shm.o" "$out/placement.o" "$out/host_kernel.o" -pthread
"$out/memory_ps5_test"
