# v32 Linux feature and settings parity audit

**Date:** 2026-10-29
**Branch:** `plan/v32-linux-launcher-settings`
**Status:** source/compile audit complete; full in-game acceptance is **not** complete.

## Executive result

The Linux build selects the project's existing SDL2 gameplay frontend from `third_party/extra/linux/src/runtime/frontend_sdl.c`, produced by an ordered patch series. It does not replace the game renderer with a Linux-specific reimplementation. The runtime still attempts OpenGL 3.3 for Enhanced rendering and falls back to SDL_Renderer plus software 3D when OpenGL initialization is skipped or fails.

The latest full temporary E2E run, [40842120096](https://github.com/rakmahefa/dqix-enhanced/actions/runs/40842120096), passed all three jobs on the aspect-fit overlay revision. It verified the six source patches then present without fuzz, Makefile-driven frontend/overlay compilation, five CTest cases including aspect-fit geometry, the locked SDL2 build, AppImage cold/warm startup, toolchain corruption rejection and ELF audit (718 ELF files, zero findings). That run predates patch 0007 and does not validate the Wayland/X11 retry. These checks also provide **no proof that every game feature works during a ROM-backed session**: CI intentionally does not package a real ROM or link/run a completed game build.

## Runtime feature parity

The feature catalogue in the root README covers these existing runtime features. Since the Linux build retains the same SDL2 frontend and core runtime, the features are expected to be present at source level, subject to the same prerequisites and runtime behavior as the existing SDL2 implementation:

| Feature area | Expected Linux runtime path | Conditions / caveats |
|---|---|---|
| Classic and Enhanced 3D | Classic CPU/software renderer; Enhanced OpenGL 3.3 renderer | Enhanced requires a working GL 3.3 core context and successful `gl_render_init`; fallback selects SDL_Renderer plus software 3D. |
| Scaling and layouts | Auto/1–12x 3D scale, window scale, stacked/side-by-side/corner/widescreen/dynamic widescreen/two windows, fullscreen, swap/single screen | Must be checked on a live display; some options depend on window/display dimensions. |
| Input and audio | Keyboard, mouse touch input, SDL gamepad, controls.ini remapping, game audio, volume/mute, fast-forward/pause/reset/screenshots/bug reports | The launcher Controls page is informational; the existing runtime owns control capture and `controls.ini`. |
| Display effects | DS Lite filter, pixel grid, LCD colours, motion blur, backlight, screen gap, console shell/skins, integer scaling/actual size | Screen/console effects are renderer-dependent; most are not editable from the current launcher pages. |
| Widescreen | Basic/dynamic widescreen, small-screen corner/size/opacity, single screen, pop-out map, experimental battle HUD | Dynamic widescreen and battle HUD are experimental upstream runtime features; live testing is needed. |
| Mods | Texture packs, xBR texture upscaling, HD text, outlines, soft shadows, smooth shading/textures, music packs/song names/volume | Depends on matching user-provided assets and, for GPU-only options, Enhanced/OpenGL. |
| Wireless and content | Built-in offline DQVC/DLC, community WFC server configuration, sessions/co-op/tag mode, custom player name and MAC | Network reachability, firewall/NAT, peer compatibility and local settings affect the result; CI does not run a live session. |
| System/diagnostics | Settings persistence, update checks, quit confirmation, language/DLC preferences, renderer diagnostics and bug-report path | Must be checked with real `settings.ini` and `devlog.log`; CI does not execute the game runtime. |

The Linux-only code changes add renderer diagnostics and the in-game overlay without modifying canonical root `runtime/`, root `build.mk`, or root `tools/build-game.sh`. The new overlay presentation keeps its fixed canvas ratio at any output size and dims the entire window in a separate pass. The 2026-10-29 live test exposed a Wayland OpenGL-window creation failure (`SDL_CreateWindow(SDL_WINDOW_OPENGL): Invalid window`); patch `0007-linux-wayland-opengl-retry.patch` now adds an OpenGL loader preflight and a guarded X11/XWayland retry when SDL auto-selected Wayland. CI and a real AppImage retest remain required before Enhanced mode and the overlay can be marked accepted.

## Settings audit

The settings model defines **57 known `settings.ini` keys**. The launcher can edit **18 unique keys** directly; it displays `wfc_server` and `player_name`, but their text editing is not implemented. The remaining **37 schema keys** are not directly editable in the launcher. They remain runtime settings or hotkey/menu options where supported and can be configured through `settings.ini` before starting the game.

### Editable in the launcher now

- `mode`
- `smooth_textures`
- `hd_text`
- `soft_shadows`
- `smooth_shading`
- `fullscreen`
- `integer_scaling`
- `dslite_filter`
- `lcd_colors`
- `actual_size`
- `window_scale`
- `mute`
- `volume`
- `music_packs`
- `music_song_names`
- `texture_packs`
- `upscale_textures`
- `wireless`

Read-only on the Wireless page: `wfc_server`, `player_name`. The Controls page explicitly states that keyboard/gamepad remapping is not implemented in the launcher; the runtime's existing `controls.ini`/controls menu remains the path for remapping.

### Present in the schema/runtime but absent from launcher editing

`update_check`, `confirm_quit`, `fast_forward`, `update_skip`, `session_port`, `session_consoles`, `resolution_3d`, `render_scale`, `layout`, `single_screen`, `small_screen_corner`, `small_screen_size`, `small_screen_hidden`, `small_screen_opacity_chosen`, `swap_screens`, `widescreen_popout`, `widescreen_battle_hud`, `outlines`, `music_pack_volume`, `discord`, `discord_app_id`, `hd_text_shadows`, `small_screen_opacity`, `second_window`, `second_window_fullscreen`, `walk_any_direction`, `pixel_grid`, `motion_blur`, `backlight`, `screen_gap`, `console`, `console_color`, `console_color_chosen`, `monitor_width_mm`, `downloadable_content`, `language`, `wireless_mac`

The schema test previously compared the canonical root source rather than the exact mirrored frontend compiled by Linux. This change directs CTest to `src/runtime/frontend_sdl.c` and checks in both directions: every runtime-loaded key must exist in the schema, every schema key must be loaded, and runtime-written keys must be recognized by both. A concrete persistence gap was also found: `discord_app_id` was read but not written during runtime settings saves. Patch `0005-linux-settings-persistence.patch` now preserves an explicitly configured override in the Linux mirror, with a regression assertion. The schema limits newly edited IDs to 31 characters to match the runtime’s `g_app_id[32]` buffer; tests accept a normal 19-digit ID and reject a 32-character value. The field is still not exposed in the current launcher UI.

**Important:** writing a setting does not prove the feature is active. Restart-required values, renderer-dependent values, unavailable assets, network conditions and settings rewritten by the game itself need actual runtime tests.

## Vulkan feasibility

**Conclusion: a native Vulkan gameplay renderer is not a quick or low-risk patch. Do not implement it as a tiny addition to the current overlay patch.**

The existing `runtime/gl_render.c` is a custom OpenGL 3.3 renderer, not just an SDL window blitter. It owns a large set of GL entry points, GLSL shaders, framebuffers and multiple render targets, texture/sampler caches, depth/stencil and blend semantics, post-processing, high-resolution composition of DS 2D and 3D layers, widescreen transforms and HD text. A Vulkan backend would need to replace or abstract the graphics operations and reproduce those semantics, plus device/queue/swapchain creation, memory/resource lifetime, render passes or dynamic rendering, pipeline creation, command buffers and frame synchronization. SDL2 can create a Vulkan surface for a window, but this is only platform/window integration, not conversion of the existing GL renderer ([SDL2 Vulkan surface API](https://wiki.libsdl.org/SDL2/SDL_Vulkan_CreateSurface), [Vulkan render-loop overview](https://vulkan-tutorial.com/Overview)).

If a native Vulkan backend becomes a real requirement, the practical path is a separate renderer-backend project with:
1. An explicit renderer interface below `frontend_sdl.c`, leaving Classic/software rendering intact.
2. A Vulkan prototype for a minimal frame/compositor first, with SPIR-V shaders and a documented dependency policy.
3. Feature-by-feature porting of the 3D raster path, blend/stencil rules, post-processing, screen composition, overlays and texture cache.
4. Automated golden-image/render tests and a live gameplay matrix before making it selectable.

For a low-effort experiment, Mesa **Zink** can implement desktop OpenGL over Vulkan on supported host systems; it is a host graphics-driver option, not a project Vulkan renderer and not guaranteed by the AppImage ([Mesa Zink documentation](https://docs.mesa3d.org/drivers/zink.html)). The current OpenGL 3.3 path plus the existing SDL/software fallback is the safer default until a concrete compatibility or performance problem justifies a backend rewrite.

## Recommended acceptance order before declaring v32 parity

1. Run the latest AppImage with a real local ROM/workspace and inspect `devlog.log` for `renderer-diagnostics`, `game_renderer_fallback`, GL vendor/renderer/version or SDL_Renderer fallback details.
2. Exercise Classic/Enhanced and fallback, hotkeys/controls, window/display modes, audio/mod assets, and (where possible) a local/online session.
3. Exercise launcher settings after restart and after the game saves its own settings; verify each UI toggle changes runtime-effective behavior, not only the INI text.
4. Finish overlay acceptance on OpenGL and SDL_Renderer fallback, including Escape, pause ticks, gamepad, focus loss, resize/fullscreen and shutdown.
5. Keep Vulkan as a separate feasibility milestone; do not mix it into the v32 settings/overlay acceptance patch.
