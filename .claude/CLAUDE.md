# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Dusklight is a reverse-engineered C++ reimplementation of *The Legend of Zelda: Twilight Princess* (GameCube, USA/EUR), built on top of the [zeldaret/tp](https://github.com/zeldaret/tp) decompilation and cross-compiled to desktop and mobile via CMake presets. It ships **no copyrighted assets** — a user-supplied game disc image is required at runtime.

**IMPORTANT**: The project maintainers DO NOT accept contributions that are primarily AI-generated and will close such PRs. Agents MUST always follow the [rules](#rules-for-agents) laid out below to avoid misrepresenting their work as human-made.

## Build & run

CMake 3.25+ with the Ninja generator, driven by presets in `CMakePresets.json`. Configure, then build:

```sh
# macOS
cmake --preset macos-default-relwithdebinfo
cmake --build --preset macos-default-relwithdebinfo

# Linux (GCC)          Linux (Clang)              Windows (MSVC)
# linux-default-relwithdebinfo / linux-clang-relwithdebinfo / windows-msvc-relwithdebinfo
```

- Variants: append `-debug` or `-debug-asan` (AddressSanitizer) to a preset. Build output goes to `build/{preset}/`.
- After cloning or pulling, sync the Aurora submodule: `git submodule update --init --recursive` (Aurora lives in `extern/aurora`).
- Run (needs a disc image): `build/{preset}/dusklight --dvd /path/to/game.iso`. On macOS the target is a bundle: `build/{preset}/Dusklight.app/Contents/MacOS/Dusklight --dvd ...`. Accepted formats: ISO/GCM, RVZ, WIA, WBFS, CISO, GCZ. A test ISO already exists at `orig/GZ2E01/GZ2E01.iso`.
- Cross-targets: `ios-default`, `tvos-default`, `android-arm64`, `android-x86_64` (Android additionally builds the APK via `platforms/android/gradlew :app:assembleRelease`).
- CI presets are named `x-*-ci-*`; the canonical per-platform build commands and system dependencies live in `.github/workflows/build.yml`.
- Full dependency lists and IDE setup: `docs/building.md`. Nix users: `nix develop '.?submodules=1'`.

## Debugging

**IMPORTANT**: When debugging, prefer using clion-debugger MCP tools to interact with the IDE debugger.

## Formatting & lint

- C++: `.clang-format` (Standard **C++03**, 100-column, 4-space indent, no tabs). Run `clang-format -i` on changed files — there is no CI format check and no `format` CMake target, so format manually.
- Python tools under `tools/`: `.flake8` (ignores E203/E501) → `flake8 tools/`.
- There is **no test suite for Dusklight itself**. Only the Aurora submodule ships gtest tests (`extern/aurora/tests/`, runnable via `ctest`). CI builds all platforms but does not run tests.

## Architecture

The codebase is two layers linked into one executable:

**1. Original decompiled game code** — the many top-level module directories under `src/` (mirrored in `include/`), following zeldaret/tp naming:
- `d/` — game logic and actors (`d_a_*` actors, `d_s_*` scenes, `d/actor/` is hundreds of actor types)
- `f_op/`, `f_pc/`, `f_ap/` — actor/process/application framework (lifecycle, scheduling, game-state machine)
- `m_Do/`, `m_Re/` — machine layer: main loop, graphics, audio, DVD, controller, threading
- `c/` — common utilities
- Nintendo middleware in `libs/` (`JSystem`, `revolution`, `dolphin`, `PowerPC_EABI_Support`) and audio in `src/Z2AudioLib`, `src/Z2AudioCS`
- The full compiled source list is enumerated in `files.cmake`.

**2. New "dusk" host engine** — `src/dusk/` + `include/dusk/`: the modern host layer (entry point, config/settings, input incl. gyro/mouse/touch, audio host, crash reporting, Discord presence, texture replacements, ImGui debug UI, RML user menus, HTTP/update checks).

**Boot & main loop:** host `main()` in `src/dusk/main.cpp` (bootstrap, arg parsing, process restart) calls the original `game_main()` in `src/m_Do/m_Do_main.cpp`, whose `main01()` drives the loop: pump Aurora events → tick the game (`fapGm_Execute`) → draw (`fpcM_DrawIterater` → `cAPIGph_Painter`, which routes GX calls to Aurora) → end frame. Version and platform macros live in `include/global.h`.

**Rendering/platform abstraction is Aurora** (`extern/aurora` submodule, by encounter), included via `include/dusk/dusk.h`. It translates the game's GX (GameCube GPU) calls to modern backends — D3D12/Vulkan/Metal, with D3D11/OpenGL-ES fallbacks — and abstracts windowing, input, and SDL3. GX→Aurora helpers are in `include/dusk/gx_helper.h`. Per-platform packaging assets live in `platforms/{android,ios,macos,tvos,windows,freedesktop}/`.

**Build config:** `CMakeLists.txt` compiles the game code with `GAME_COMPILE_DEFS = TARGET_PC WIDESCREEN_SUPPORT=1 AVOID_UB=1 VERSION=0 MTX_USE_PS=1`.

## Code conventions

From `docs/code-conventions.md`:

- Bug fixes, cleanup, or documentation that also apply to the upstream decomp should preferably be PR'd to [zeldaret/tp](https://github.com/zeldaret/tp).
- When modifying original decomp code for Dusklight, gate the change with `#if TARGET_PC` and keep the original code in place. Use `#if AVOID_UB` for undefined-behavior fixes.
- `new`/`delete` in original game code are replaced with the `JKR_NEW` / `JKR_DELETE` macros (see `JKRHeap.h`) to avoid the game's global-`operator new` heap tree, which would otherwise cause linkage problems.

## Rules for Agents

- ALWAYS defer to user-created code unless the user requests otherwise. In that case, only produce edits in batches of AT MOST 20 LoC excluding comments.
- ALWAYS place generated notes in the 'notes/' directory.
- ALWAYS include an appropriate disclaimer in files under the 'notes/' when they have been authored by you. 