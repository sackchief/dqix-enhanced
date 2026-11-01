# Where the project stands

Git tag `v33` marks the current stable version.
The ones before: `v32`, `v31`, `v30`, `v29`, `v28`, `v27`, `v26`. To go back: `git checkout v32`.

## Stable and nightly

- **Stable** = git branch `stable` (now v33, tag `v33`): the
  version to play your main save on. Changes only by promoting a proven
  nightly, or a small safe fix (made on `stable`, then merged into `main`).
  Keep it in its own folder, built with BUILD.bat.
- **Nightly** = branch `main`, versions `v34-nightly1`, `-nightly2`... (next)
  Experimental. Its own folder and saves.
- **Promoting**: when a nightly is solid, it becomes the next stable: set the
  version to `v27`, bookmark (tag), move `stable` to it, make the stable zips.

## What works (v26)

- **The whole game** recompiled to C (both CPUs) and running natively on
  Windows: graphics (Classic = pixel-exact DS, Enhanced = GPU 3D at 1-4x),
  sound, input (keyboard, mouse touch, gamepad), saves compatible with the DS.
- **Menu bar** (Game, Graphics, Screen, View, Wireless, Help); `settings.ini`.
- **DS Lite look**: LCD filter (colours, pixel grid, motion blur, backlight),
  drawn console in six colours, the Jet Black photo (`skins/`), actual size.
- **Bug reports** (F10, with a description box): screenshot, inputs, saves,
  log, perf; deterministic replay of the session (see README).
- **Wireless**: the DS Wi-Fi chip (from melonDS), the access point DQIX-PC,
  libslirp for the internet, and the **built-in Nintendo WFC server**
  (`runtime/wfc.c`): Sellma's download works offline, all 64 DLC quests,
  DQVC's 52 weekly sales on the original calendar (PC clock).
- **One-click build** from a ROM: `BUILD.bat` -> `game/`. Zips for sharing:
  `tools/make-release.py` -> `release/` (project; extras separately).

## Key facts found along the way

- DQIX download files: RC4, key `XENLONPROJECTKEY`; last 4 bytes make the
  encrypted file (big-endian number) divisible by 0xC2A030D4 (ov023 021f6324).
  Stock file = pool of `0x68` records (item, price, 1, qty); the game shows 6.
- Login needs a `Date:` header; NoSSL = https:// -> http:// in overlay 31 RAM.
- Save: two 32 KB slots, "DRAGON QUEST IX"; quest status nibbles at 12300
  (bit 3 = DLC received); guests bitmask at 11528; DQVC stock at 27840 (40-byte
  records); news text near 23188.
- DS firmware Wi-Fi slots at FW_USER-0x400; the game uses slot 1 ("DQIX-PC").
- The decomp's `config/usa/arm9` (public domain) + `tools/extract_rom.py`
  reproduce `dsd`'s extraction byte for byte; generated C is identical.

## Nightly: wireless between DSes and playing far apart (v27-nightly3)

- **The air** (`runtime/air.c`): copies of the game on one PC share memory;
  every console counts the same 8 us ticks, a frame arrives at its sender's
  tick + preamble, a listening console waits until the others are within 11
  ticks, frames are taken in (tick, console) order: deterministic.
- **The chip** (`runtime/wifi.c`): the multiplayer half added (melonDS
  Wifi.cpp): frames/beacons between consoles, MP CMD with reply windows,
  client auto-replies, ACK. Tested: tag mode (both counters go to 1, the
  other's title and name shown) and co-op through Pavo ("Player A has
  arrived!", guest in the host's world) between two copies.
- **Sessions** (`runtime/session.c`, `upnp.c`): every PC runs every player's
  console (others hidden: same exe, `DQIX_HIDDEN=1`, Windows job object kills
  them with the window); TCP via the host carries only buttons (lockstep, input
  delay from pings, 3+ frames) plus RAM fingerprints every 120 frames. Save +
  128 KB firmware of each player go to all at the start; RTC = host's. A
  player who leaves is switched off at the same frame everywhere
  (`AIR_INPUT_END`). Lobby on the DS screens (stb_truetype, system font),
  Wireless > Play with friends far away (Host / Join / Start / Leave / name).
  Tested: co-op between two simulated PCs over TCP, 87/87 fingerprints equal;
  windowed lobby + start under Xvfb.
- How DQIX's wireless starts: tag mode = Erinn > Canvass for guests > save >
  "Go get 'em!" > **A** (only then the radio comes on; it alternates parent
  (beacons) and scanning on channels 1/7/13 at random intervals). Co-op =
  Pavo (Quester's Rest, right end of the counter, 1st floor): host "Invite
  friends from other worlds", guest "Visit another world" (alone).
- **nightly3**: the other players' DSes run in the player's own process, on
  one thread, from copies of `dqixcore.dll` (the same objects linked as a
  library; `hosting.c`: per-copy settings instead of getenv, exit = this
  console only, yield = fiber switch, shared ROM). 3 consoles in tag mode on
  2 cores: fine (12000 frames in 2m20s headless) vs >25 min as processes.
  Fallback to processes if the library is missing. `DQIX_SESSION=local:N`
  tests N consoles on one PC.
- **nightly4**: fixed PCs drifting apart after a hiccup (a PC pausing a few
  seconds while a radio was on): a console with its radio off doesn't wait
  for the others, so frames sent during that time could still be in the
  air when its radio came back on, and were then heard or not depending on
  timing. Now frames that reached a console while its radio was off are
  never heard (`g_deaf_until` in air.c). Before: a 6 s pause in tag mode =
  out of sync within 20 s; after: 74/74 checks the same. Also:
  `session\front.log` (every console's progress every 10 s, each sync
  check, what a console is stuck waiting for after 1.5 s, the network), the
  title bar says what it's waiting for, build fingerprints in HELLO/WELCOME
  (log only), sessions no longer write `wireless.bin`, build-pc.bat builds
  dqixcore.dll too.
- **nightly5**: the built-in Wi-Fi (DQVC, downloads) works in sessions, for
  every console: the host's `wfc/` files go out in the start message and
  every PC uses them from `session/wfc`; the virtual network is sealed
  (`net_seal`: only the built-in server, DHCP and ARP; the router's clock is
  game time; the server's random numbers are its own). Tested: a DQVC
  purchase in a 2-PC session, 70/70 sync checks the same, the guest without
  any files of its own. Other servers stay off in sessions. Also: a host /
  join window on Windows (address with Copy, players, Start the game,
  Cancel), the DS Lite photo is the default console until one is picked
  (`console_color_chosen`), long message boxes are wrapped.
- **nightly6**: DQVC like the original service: each year's cycle starts on
  the Friday nearest 15 July and every sale keeps its offset from it (2011
  dates, so sales start on Fridays), days start at 08:00 UTC, and each day
  serves 12 of the sale's items picked by the date (the original changed
  daily; its daily picks aren't recorded), serial = 1000 + days since
  2010-07-01 (the game refuses serials past 16 bits: "I don't seem to have
  DQVC's latest product information"). With the same pool the game showed
  the same six on every day, so the daily share is what varies it. UTC
  offset: `rtc_utc_offset`, carried by sessions and reports
  (`DQIX_UTC_OFFSET`; old replays: 0).
- **nightly7**: 3-4 players with different saves (Player A, Player B, Player C,
  Player D; local:N). Tag mode: everyone canvasses everyone (11 of 12 pairs;
  Player A never gets Player D, in any order or alone with him - most likely
  something in those saves, every other direction works). Co-op: a second
  visitor could never join (the host's answers to auth/assoc only go out
  between its beacon and IRQ13, ~9 ms; a joining DS answers ~1 frame late
  here, so it timed out: AID never set, then "unexpected communication
  error"). Fix: a host's post-beacon count (W_BeaconCount2) gets +24 ms.
  Now 3 and 4 players meet in the host's world. DQVC: two saves on the
  same day got the same six from the same file (the game doesn't pick per
  player; the original's "different for each player" was the server).
- **nightly8**: DQVC per DS: the daily share is picked by the day and the
  console's MAC (firmware 0x36, sent with each console in a session), so
  friends get different shops; Player A and Player B on the same Friday: 2 items
  in common out of 6. Visiting: a visitor (Player C, in Player A's world) talks
  to Sellma ("one of young Player A's friends"), sees the host's stock and
  buys from it (skull ring: 1 -> 0, 950 G). Separate "PCs" (processes over
  TCP, `DQIX_NETDELAY=ms` holds every message that long): 3 PCs in tag
  mode, everyone canvassed both others, 184/184 sync checks the same.
  4 PCs in co-op (Player C hosts; Player B, Player A, Player D visit) first froze
  for good: Player D's console came back on air 1.4 s ahead of the others
  and waited for them without reading its rings, while the host's console
  waited for room in its ring to Player D (full of frames sent while
  Player D was deaf). Fix: waiting consoles drop the frames they could
  never hear (`drain_deaf` in air.c). Then: all four in Player C's world on
  every PC, 206-235 sync checks each, all the same.
- **nightly9**: going public as github.com/sackchief/dqix-enhanced
  (GPL-3.0-or-later: wifi.c and gpu3d.c translate melonDS code). README for
  players (features, getting started, multiplayer, updating); the old
  README is docs/TECHNICAL.md. In-game updates (`runtime/update.c`,
  Windows/WinHTTP): at start, the stable branch's runtime/version.h vs this
  version (asks: Update / Skip this version / Later; `update_check`,
  `update_skip` in settings.ini); Help > Check for updates offers stable and
  nightly (main). Installing downloads the branch zip from codeload into
  `update\`, writes `update\apply.bat` (waits for the game to exit, copies
  the project files to `update\previous`, unpacks with tar, copies over,
  runs BUILD.bat with DQIX_NOPAUSE=1, starts game\play.bat) and
  `update\rollback.bat`. BUILD.bat remembers the ROM (rom\last-rom.txt).
  Only works in a BUILD.bat project (game\ one below BUILD.bat).
- **nightly10**: the built-in Wi-Fi needs no files. DQVC's schedule is built
  in as numbers (runtime/dqvc_schedule.h from tools/dqvc_to_c.py; no item
  names or sale titles); the quest download is built from 64 quest + 23
  guest numbers (dlc_quest_file: byte for byte Nintendo's quest52e.bin of
  2010-06-04, checked); without a _list.txt the server lists those two plus
  infomes_47.bin when present, which build-game.sh fetches from
  raw.githubusercontent.com/InternalLoss/DLS1-Files/master/YDQE/ (the only US
  news that survives; Square Enix's text, so not kept here). Tested: same
  requests, same screens and the same save as with the original files; no
  files at all (2 files, DQVC works); a session with only the news.
  Next: hand-written weekly news (by the owner, by hand).
- **nightly11**: Modern menu. *Walk in any direction*: the game walks by
  stylus drag (touch anywhere on the field, drag; the hero walks the drag's
  direction, its length doesn't matter). input.c: while the left stick is
  past WALK_DEAD and the walk flag at 0x02109DA4 is 1 (it is 1 while the hero
  is free to walk, 2 in dialogue/menus, 0 while loading; checked over 19
  states; written by 020c814c), it touches (128,100) for 2 frames then drags
  48 px the stick's way; otherwise the stick is the D-pad. Not yet checked:
  battles. DQIX_STICK=from,to,x,y + DQIX_ANALOG=1 for tests. *Corner
  layout*: canvas 341x192, top screen at 1/3 in the top left
  (`layout = corner`, F2 cycles). With both off: replays unchanged.
- **nightly12**: the program's icon is the game's own: the 32x32 banner
  icon (ROM header 0x68 -> banner; bitmap +0x20, 4x4 tiles of 8x8 at 4 bpp;
  palette +0x220, BGR555, colour 0 clear). tools/rom_icon.py (standard
  library only) writes build/icon/dqix.ico (16-256, nearest pixel) and
  build-game.sh compiles it with windres into a resource (RES= in build.mk,
  plus a VERSIONINFO "Dragon Quest IX"); SDL takes the exe's first icon for
  the window. Without the resource (Linux, build-windows.sh) frontend_sdl.c
  window_icon() sets it from g_rom at 64x64. exe_main.c: no arguments ->
  the exe's folder, data/dqix.nds data/code 0 1 dqix.sav, and on Windows the
  console is freed (output to last-run.log; restarts use CREATE_NO_WINDOW).
  Tested: the icon read back from the exe's resources and from the X window;
  no-argument start on Linux. Not tested: the Windows no-console start.
- **nightly13**: *Widescreen* (Modern / View, `layout = wide`, Enhanced
  only). gpu3d.c: with g_wide_side = S > 0 every polygon is also clipped a
  second time with x scaled by 256 / (256 + 2S) (wide_submit, after the
  facing test, from tmp_vtx), screen x mapped onto 0..256+2S; polygons with a
  smaller viewport are copied unwidened, shifted by S. Its own banks and
  y-sorted list (wrender_list), used by gl_frame when the whole frame was
  made with the same S; the normal list, polygon counts, box tests and the
  software renderer are untouched (checked: same save and 135 identical 2D
  frames with the stacked and wide layouts). gl_render.c: 3D buffers and
  the composed picture (256+2S)*scale wide; FS_COMP continues the 3D at the
  sides on lines where the 8 edge columns show 3D (with that pixel's blend /
  fade), black otherwise; the other screen composed in the middle.
  Frontend: the 3D screen big and wide, the other a third in the top left
  (minimap); when 2D covers that corner (>5% of it: menus, title) it eases
  to the corner layout (g_wide_mix); 3D on the top screen (battles): the
  touch screen at half size bottom left; a black small screen isn't drawn.
  S = 43 (16:9), fullscreen: the monitor's aspect (max 128).
  *Stylus arrow hidden while stick-walking*: tags.txt + recomp --tags wrap
  the 4 draw calls of func_02096c38 with gx_tag_begin/end; the game queues
  its GX commands (0x0210CF78 -> count + 192 words, sent later), so
  gx_tag_end notes the TEXIMAGE_PARAMs queued during the call and polygons
  with those textures that frame carry the tag; hidden (HPoly.hidden) only in
  what's drawn, while analog walk is on and the mouse isn't held. Checked:
  only the arrow's pixels differ, save identical.
- **nightly14**: walking with the stick is on by default (frontend_init sets
  it before settings_load; the setting is now `walk_any_direction`, so older
  settings files, which all said analog_walk = 0, start with it on). The
  arrow is hidden only while the stick walks (and 8 frames after), so mouse
  and touch-screen walking keep it.
- **nightly15**: a new install (no settings.ini) starts with the DS Lite
  screen filter and the console photo (g_opt.lcd, g_opt.shell = 1);
  settings.ini always holds dslite_filter and console, so updates keep each
  player's choice.
- **nightly16**: FS_SCREEN samples with textureGrad and derivatives worked
  out from the layout (g_duv, g_dp), not fwidth(): on a screen's first row
  the 2x2 pixel quad reached into the console branch, the derivatives were
  garbage and the coarsest mipmap was used (a flat-coloured line). Samples
  are also kept inside each screen's part of the texture (keep_in), half a
  texel of the mipmap level in use. The plain view is pixel for pixel the
  same as before.
- **nightly17**: SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS = 0 (SDL2 reports
  Nintendo controllers by label by default: A/B and X/Y were swapped on
  Switch controllers); Joy-Con pairs combined. Not tested with real pads
  here.
- **nightly18**: the right stick (x) presses L / R while the walk flag is 1
  and neither is held (input.c stick_camera): the game turns the camera
  while L/R is held, as far as each area allows (towns and rooms ~45 deg
  each way, the world map not at all), and keeps it on release; duty 25% to
  100% by how far the stick is pushed, so a light push turns slowly
  (checked: full push = holding L). DQIX_RSTICK=from,to,x for tests.
  Double-click in fullscreen = leave it (nightly19: only outside the touch
  screen, where clicks are the stylus; to_touch decides).
- **nightly20**: freeze fix. threads_after_irq (the thread switch an IRQ
  decided, done after the handler returns) runs game code (the divider
  context save/restore); a second pending IRQ was taken there, its own
  switch ran inside the first, OSi_ThreadInfo.current and the running
  coroutine parted, a thread's OS_SleepThread then put the idle thread to
  sleep: every ARM9 thread waiting, IRQs masked, frozen (a report: tap the
  logos on frame 466, Continue -> frozen at 1810). irq_service is held off
  (irq_depth += 100) until the switch is done; replays otherwise unchanged.
  threads_dump (DQIX_THREADS=1) also shows the IPC FIFOs and IE/IF/IME.
  Sticks: the walk flag (02109DA4) is 1 on the title menu too; walking and
  the camera now also need the field pointer at 020F33E0 (0 until a save is
  loaded).
- **nightly21**: `runtime/portmap.c`: PCP (RFC 6887, MAP v2) then NAT-PMP
  (RFC 6886) when UPnP fails; IPv6: UPnP WANIPv6FirewallControl AddPinhole,
  else PCP MAP from the global v6 address; renew_thread renews at half the
  lifetime. Tested against stand-in routers (DQIX_GATEWAY,
  DQIX_GATEWAY6, DQIX_UPNP_LOCATION, DQIX_TEST_V6). Join takes a bare IPv6
  address. choice_box (menu_win32.c): dialogs whose buttons are sized to
  their text (update window, bug report). Send a bug report: report_zip
  (stored zip, saves optional) + a prefilled GitHub issue
  (.github/ISSUE_TEMPLATE). Setup: `Install Dragon Quest IX.bat` ->
  tools/setup-wizard.ps1 (WinForms, PowerShell 5.1: ROM version from the header (no checksum; any ROM can be tried), folder
  checks, robocopy, Unblock-File, BUILD.bat with DQIX_NOPAUSE, shortcuts,
  HKCU Uninstall entry -> tools/uninstall.ps1). apply.bat starts
  game\dqix.exe. Earlier versions: annotated tags v17-v25,
  v27-nightly1-20 (release-notes/past/index.txt; no releases for them).
- **v27 stable** (2026-10-18): v27-nightly21 promoted (version "v27",
  release-notes/v27.md; `stable` = `main`). Nightly is the same until
  v28-nightly1.
- **v28-nightly1/2**: session/front.log from hosting/joining on (live).
  Relay: tools/relay/worker.js (Cloudflare Worker + SQLite Durable Object
  per code, free plan; deploy-relay.bat), runtime/relay.c (WinHTTP
  WebSocket on Windows, own ws:// client for tests; host: each friend =
  a TCP connection to 127.0.0.1:port; friend: a local port to connect to;
  30 ms coalescing, keepalive 25 s), relay_url.h. Host window by reach
  (direct / code / home only). connect_within 10 s; home_address warning.
  Tested with a Node stand-in for the Workers runtime.
- **v34-nightly1** (2026-11-01): the in-game menu (menu_items.c: every
  option once, the Windows bar built from it; menu_ingame.c: the list,
  messages, value rows, lists, typing with an on-screen keyboard;
  gl_set_overlay). Esc / Home / Select+Start open it. Controls, save
  import / export, typing and the session lobby inside the game.
  Tests: DQIX_MENU_TEST=frame:keys (menuNN / dialogNN / listNN / textNN.ppm).
- **v33 stable** (2026-10-31): fixes from v32's reports, straight to
  stable. Battle HUD: panels found by their lines on the left (submenus
  cross them on the right; then one piece), kept in the party's order, a
  party of one (its white bottom line; a panel higher); 3D battle messages
  too wide to pass the panels move up (HPoly ui_dy, their wide lines too).
  2D dialogue band: only small windows (runs <= 12 cells) in the row above.
  Krak Pot: overlay 6 opened from the field (alchemy_track) never swaps.
  Big-object box test out to 2 wide widths, not 3 (DQIX_BIG_DIV). The
  Gortress turnstile is still drawn displaced with a stretched arm, in
  Classic too (report v33-20261031-180958 at frame 10748): a core bug.
- **v32 stable** (2026-10-27): v32-nightly3 and the work after it promoted.
  European ROM (tools/region.py moves the USA address lists; wrap.txt names
  its hooks so the runtime's wrap_<name> doesn't depend on the address);
  Mods (texture/music packs, song export: music.c, music_export.c); HD
  text; Dynamic Widescreen redone: 3D windows (flat w=16384 polys,
  gpu3d.c ui_shift_classify) and 2D windows by layer group (gl_render.c
  ui_classify, u_layer/u_move per pixel), the dialogue box kept centred,
  menu screens 4:3 (a full-screen picture, or no 3D scene with a 2D
  background), battle HUD panels; display captures of the 3D drawn on the
  CPU too (the inn's guests); the Alchemimicon (overlay 6, no 3D) swaps
  the screens. Tests: DQIX_UILOG, DQIX_UICELLS, DQIX_HUDLOG, DQIX_CAPLOG,
  DQIX_OVLLOG.
- **v31 stable** (2026-10-19): v31-nightly1 promoted. Basic widescreen
  (corner/size/opacity) and dynamic (experimental); single screen; triggers
  and stick clicks; reset (relaunch, DQIX_WINPOS); volume; controls.c /
  controls.ini remapping with a Windows Controls window; Enhanced 3D over a
  2nd blend target blends by its own alpha (equipment screen).
- **v30 stable** (2026-10-19): v30-nightly1 promoted. Widescreen: a
  BOX_TEST outside the DS's view is retried against the wide view
  (Model3D::TestVisible culls with it), so the sides keep their people,
  doors, items and monsters. Off in sessions.
- **v29 stable** (2026-10-19): v29-nightly4 promoted. Enhanced: DS
  translucency/fog rules (stencil), soft outlines, Auto resolution up to
  12x, smooth textures, sharp bilinear output, widescreen backdrop stretch;
  screens: swap (F9), two windows (second has a Fullscreen bar), widescreen
  pop-out, battle commands top left, see-through small screen (O).
- **v28 stable** (2026-10-18): v28-nightly2 promoted after a real session
  between two apartments far apart through the relay (ping ~350 ms, input
  delay 20 = the maximum, 29,000+ frames in sync).
- Not done yet: 3-4 players over the internet (tested on one PC, local:N:
  the same consoles and air as a session); joining a session that has
  started; a relay for hosts who can't open a port (virtual LAN for now).

## Later ideas

- DLC quests one per week in their release order (quest file format known).
- DS Lite filter tuned against photos of a real DS Lite; save states;
  Enhanced extras (widescreen, top-screen map); EU/JP ROMs in BUILD.bat.
