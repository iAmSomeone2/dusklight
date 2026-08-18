#!/usr/bin/env bash

set -e

cmake -S /workspace/src -B /workspace/build \
-G Ninja \
-D CMAKE_C_COMPILER_LAUNCHER=sccache \
-D CMAKE_CXX_COMPILER_LAUNCHER=sccache \
-D CMAKE_BUILD_TYPE=RelWithDebInfo \
-D CMAKE_INSTALL_PREFIX=/workspace/build/install \
-D BUILD_SHARED_LIBS=OFF \
-D TRACY_ENABLE=ON \
-D AURORA_SDL3_PROVIDER=vendor \
-D SDL_WAYLAND=ON \
-D SDL_X11=OFF \
-D SDL_KMSDRM=OFF \
-D SDL_OFFSCREEN=OFF \
-D SDL_VIVANTE=OFF \
-D SDL_ROCKCHIP=OFF \
-D DUSK_ENABLE_DISCORD=OFF

cmake --build /workspace/build
cmake --install /workspace/build
