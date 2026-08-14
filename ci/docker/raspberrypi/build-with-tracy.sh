#!/usr/bin/env bash

set -e

cmake -S /workspace/src -B /workspace/build \
-G Ninja \
-D CMAKE_C_COMPILER_LAUNCHER=sccache \
-D CMAKE_CXX_COMPILER_LAUNCHER=sccache \
-DTRACY_ENABLE=ON \
-DAURORA_SDL3_PROVIDER=vendor \
-DSDL_WAYLAND=ON \
-DSDL_X11=OFF \
-DSDL_KMSDRM=OFF \
-DSDL_OFFSCREEN=OFF \
-DSDL_VIVANTE=OFF \
-DSDL_ROCKCHIP=OFF \
-DDUSK_ENABLE_DISCORD=OFF

cmake --build /workspace/build
