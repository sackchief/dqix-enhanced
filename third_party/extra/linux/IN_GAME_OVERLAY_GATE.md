# In-game SDL2 overlay integration gate

**Status: Linux-only overlay event/pause wiring and aspect-fit GL/SDL_Renderer presentation are implemented. Run [40842120096](https://github.com/rakmahefa/dqix-enhanced/actions/runs/40842120096) passed all three jobs: six source patches applied without fuzz, frontend/overlay objects compiled, all five CTest cases and the full temporary AppImage E2E passed. Live-game acceptance remains pending.**

## Findings from the existing Linux build path

- `runtime/frontend.h` exposes the frontend lifecycle: `frontend_init()`, `frontend_frame()` and `frontend_shutdown()`.
- `runtime/frontend_sdl.c` owns the game's SDL2 event loop, presentation and pause/resume behavior. The runtime has an OpenGL path and an SDL/software fallback.
- The launcher is a separate process and cannot share the game's window, event queue or pause state merely because both use SDL2.
- Root `build.mk` selects `runtime/frontend_$(FRONTEND).c`; its runtime object paths and explicit frontend rule are rooted under `runtime/`.
- Root `tools/build-game.sh` calls root `build.mk` directly. There is no existing argument to inject a Linux-only lifecycle hook.
- Root `runtime/frontend_sdl.c`, `build.mk` and `tools/build-game.sh` are shared with the Windows/MSYS2 path. Editing those canonical files directly would dirty the Windows/upstream path.

## Selected architecture: patch-derived Linux source mirror

Keep canonical files untouched and compile a small, deterministic Linux mirror under `third_party/extra/linux/src/`.

The expected first mirrored inputs are:

- `runtime/frontend_sdl.c` — narrow in-process lifecycle/event/render/pause hook.
- `build.mk` — Linux-specific source/object selection for the mirrored frontend and Linux-only overlay objects.
- `tools/build-game.sh` — the Linux build entry-point copy, configured to use the mirrored makefile and an explicit workspace root.

Implementation rules:

1. **Implemented:** ordered patches and checked-in mirrors live under `third_party/extra/linux/`; patches target copies only.
2. **Implemented:** `tools/sync_linux_sources.py` copies canonical inputs to a temporary directory, applies patches with no fuzz, and validates canonical and resulting Git-blob hashes.
3. **CI integrated:** development CI checks mirror integrity and syntax-compiles the patched frontend; production CI checks mirror integrity. Drift or patch failure stops validation.
4. **Implemented:** BuildJob requires mirrored `src/tools/build-game.sh`, which selects `src/build.mk` and the patched frontend. No fallback to root scripts is allowed.
5. Keep mirrors minimal and rebase deliberately when upstream changes a tracked input.

This approach is worthwhile because a small patch set can modify Linux runtime behavior while keeping Windows sources and the upstream-facing diff clean. Its key cost is upstream drift; source-manifest checks and deterministic regeneration are mandatory, not optional hygiene.

## Runtime integration constraints

- Extend the game's existing window, event and pause/resume lifecycle. Do not launch a second SDL process as a substitute for an in-game overlay.
- Render through the runtime's active path. Do not assume an `SDL_Renderer` can simply be layered over an OpenGL context, and do not create another window or renderer.
- Keep overlay interfaces/implementation under `third_party/extra/linux/`.
- Do not use SDL symbol interposition, `LD_PRELOAD`, a new UI toolkit, SDL3, Vulkan or a hand-maintained whole-runtime fork.
- Keep root `runtime/`, root `build.mk` and root `tools/build-game.sh` unchanged for Windows/upstream use.

## Renderer diagnostics (implemented; manual check pending)

The patch-derived Linux frontend writes a `renderer-diagnostics` block to stderr at startup; the launcher captures it in workspace-root `devlog.log`. It explicitly reports `game_renderer_fallback=false` for OpenGL, `true` for SDL_Renderer fallback, or `not-applicable` in headless mode. It records the OpenGL skip/failure reason, SDL compile/runtime versions and video driver, environment overrides, window/display details, GL vendor/renderer/version/GLSL/context attributes, or SDL_Renderer name/flags/formats/errors/output size and streaming texture format/access/dimensions. OpenGL library loading is now probed explicitly. If the automatically selected Wayland backend fails to create the GL window and XWayland is available (`DISPLAY` is set), the frontend retries on X11 only when no explicit `SDL_VIDEODRIVER` override was supplied; if the retry fails, it restores Wayland before using the existing SDL_Renderer fallback. A real AppImage/game run is still needed to confirm the output in practice.

## Overlay rendering and runtime wiring (implementation added; acceptance pending)

`overlay/linux_overlay.c` produces a transparent 512×768 RGBA menu canvas with a project-owned 5×7 bitmap font. The GL and SDL_Renderer paths dim the complete gameplay window independently, then draw the menu into a centered 2:3 aspect-fit rectangle to avoid stretching in fullscreen/widescreen windows. Mouse hit-testing maps through the same fitted rectangle. The GL helper restores the renderer state it changes. The ROM-free raster test now also checks fitted geometry for native, 16:9 and 4:3 viewports.

Run 40842120096 confirms source-mirror regeneration without fuzz, Makefile-driven compilation of the patched frontend plus both overlay objects, all five launcher/project CTest cases, and AppImage cold/warm startup and ELF audit. It does **not** link a game containing ROM-generated code or execute the in-game overlay. The required GL, software-renderer, input, focus, fullscreen, shutdown and real-game checks below must pass before this gate can be marked complete.

## Required acceptance tests

Do not mark this gate passed until all are checked:

1. CI proves root canonical source/build files are unchanged and identifies the mirrored source actually compiled by the Linux build.
2. Mirror regeneration, patch apply, source hashes and clean-workspace Linux build pass.
3. Escape opens/closes the overlay without triggering a DS action; keyboard/gamepad focus and input capture work.
4. Pause/resume demonstrably stops/restarts game ticks.
5. The overlay works with the OpenGL path and has correct behavior with the SDL/software fallback.
6. Fullscreen, resize, focus loss, shutdown, error paths and ordinary gameplay with the overlay closed work; the menu keeps its aspect ratio and click targets remain aligned after resizing.
7. Synthetic/dry-run tests pass without a ROM; real-game validation happens locally only. No ROM, save or protected asset enters CI artifacts.

## Current development logging

Workspace-root `devlog.log` includes timestamped build messages and game stdout/stderr under a session header. This captures renderer messages already emitted by the runtime; it does **not** synthesize every frame's in-memory performance counters. A future supported runtime export or user-triggered report is required to persist that performance ring.
