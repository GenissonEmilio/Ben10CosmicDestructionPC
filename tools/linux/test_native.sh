#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
mkdir -p artifacts/native-regression
g++ -std=c++20 -O1 -msse4.1 -fno-strict-aliasing \
  -DPS2_RUNTIME_LOGS=1 -DPS2X_ENABLE_IOP_RPC_TRACE=0 \
  -IPS2Recomp/ps2xRuntime/include -IPS2Recomp/ps2xIOP/include \
  -IPS2Recomp/ps2xRuntime/src/lib/Kernel -IPS2Recomp/ps2xIOP/src \
  -I"$kit/source/raylib" \
  tests/native_regressions.cpp PS2Recomp/ps2xTest/src/test_function_table.cpp \
  overrides/cosmic_overrides.cpp \
  PS2Recomp/ps2xIOP/src/native/iop_native_overrides.cpp \
  PS2Recomp/ps2xIOP/src/emulator/iop_emulator.cpp \
  PS2Recomp/ps2xRuntime/src/lib/gow2_desktop.cpp \
  PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/CD.cpp \
  PS2Recomp/ps2xRuntime/src/lib/Kernel/Syscalls/RPC.cpp \
  -Wl,--start-group "$kit/lib/libps2_runtime.a" "$kit/lib/libps2_iop.a" \
  "$kit/lib/liblibmpeg2.a" "$kit/lib/libraylib.a" -Wl,--end-group \
  -lGL -lX11 -pthread -ldl -lm -static-libstdc++ -static-libgcc \
  -o artifacts/native-regression/check
timeout --kill-after=3s 20s artifacts/native-regression/check artifacts/native-regression/disc
