#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
docker build -t cosmic-ps2-linux-builder -f tools/linux/Dockerfile .
docker run --rm --init --cpus=2 --memory=3g --memory-swap=3g \
  --security-opt label=disable -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/src" cosmic-ps2-linux-builder bash -c '
    set -euo pipefail
    cmake -S PS2Recomp -B build/cmake -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
      -DCMAKE_CXX_FLAGS="-msse4.1 -fno-strict-aliasing" \
      -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF \
      -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF \
      -DPS2X_ENABLE_IOP_RPC_TRACE=OFF -DPS2X_ENABLE_PCH=OFF
    cmake --build build/cmake --target ps2_runtime ps2_recomp ps2_analyzer --parallel 2
    python3 tools/linux/prepare_compiler.py
  '
printf '%s\n' 'Kit aberto criado em artifacts/compiler.'
