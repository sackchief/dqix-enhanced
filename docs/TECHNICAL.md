# DQIX Enhanced: technical notes

How the PC version works, what each part does, and the tools for testing it.
The README is the short version for players.

Turns Dragon Quest IX's DS machine code (both CPUs) into C that compiles
natively, plus the runtime that stands in for the DS hardware.

* Milestone 1 (done): the whole game recompiles, compiles and links.
* Milestone 2 (done): the game boots on PC with both CPUs.
* Milestone 3 (done): graphics. 2D engines and a software 3D renderer. Logos,
  opening movie, title, adventure-log menu, character creation, the story
  opening in Angel Falls and the first battle all render.
* Milestone 4 (done): a playable PC build. Window, keyboard / mouse / gamepad,
  sound, frame pacing (SDL2). Built on Windows with `BUILD.bat` from your ROM.
* Milestone 5 (done): Classic mode, the game exactly as a DS shows and plays
  it (see "Classic and Enhanced" below), and Enhanced mode, with 3D drawn on
  the graphics card at 1-4x resolution.
* Milestone 6 (nightly): local wireless between DSes, and **playing with
  friends far away**: tag mode and visiting/inviting (Pavo) for up to 4
  players across the internet (see "Playing with friends far away" below and
  `docs/STATE.md`, which also says where the project stands).

## Getting started (Windows)

You need your own Dragon Quest IX cartridge's ROM (the USA or the
European version; the build tries any version), Windows
10 or 11 (64-bit), about 3 GB of free space, and the internet the first time.

The easy way is **`Install Dragon Quest IX.bat`**: a setup window that does
the steps below for you (into Documents\DQIX-Enhanced by default), adds
shortcuts and an entry in Settings > Apps. By hand:

1. Put your ROM (`.nds`) in the `rom` folder. (Or skip this and choose it
   when step 2 asks.)
2. Double-click **`BUILD.bat`**. The first time, it installs MSYS2 (free
   build tools: a C compiler, SDL2 and Python, via winget, into C:\msys64),
   which takes a few minutes. Then it checks the ROM, takes the game's code
   out of it, turns it into C and compiles it (10-20 minutes the first time,
   much less after updates).
3. Play with **`game\dqix.exe`** or `game\play.bat` (make a shortcut to it if you like). The
   `game` folder holds everything the game needs, including your save
   (`dqix.sav`) and settings (`settings.ini`), so you can move it anywhere.

**Built-in downloads.** The built-in Wi-Fi server needs no files: DQVC's 52
sales are built in as numbers (`runtime/dqvc_schedule.h`, made by
`tools/dqvc_to_c.py`), and the quest download is built from the list of
quest and guest numbers it held (byte for byte the file Nintendo served on
4 June 2010). Only DQVC's news is Square Enix's text: `BUILD.bat` fetches the
one message that survives (`infomes_47.bin`) from the fans' archive of
Nintendo's download server. Original files in `extras\wfc\YDQE\` (with a
`_list.txt`) are still used instead, if present; see `extras\README.txt`.

Nothing from the game is in this project: `BUILD.bat` makes everything from
your ROM on your PC (`tools/extract_rom.py` takes the code out of it,
`recomp/` turns it into C). The function names and boundaries come from the
public-domain DQIX decompilation project (`config/usa/arm9`, its
`symbols.txt` and `relocs.txt` files). **Don't share the `game` or `build`
folders**: they contain the game.

`build-pc.bat` is the older developer build: it uses a dqix-decomp checkout
one folder up (`..\extract`, `..\config`) and builds `..\build\pc`.

| DS | Keyboard | Controller |
|---|---|---|
| D-pad | arrow keys | D-pad / left stick |
| A / B / X / Y | X / Z / S / A | right / bottom / top / left face button |
| L / R | Q / W | shoulders |
| START / SELECT | Enter / Right Shift | Start / Back |
| touch screen | left mouse button on the bottom screen | |

Every setting is in the **menu bar** at the top of the window (Game,
Graphics, Screen, View, Help); the keys are shortcuts: Tab (hold) = fast
forward, P = pause, F5 = Classic / Enhanced, F4 = 3D resolution in Enhanced,
F6 = DS Lite screen, F7 = DS Lite console, F8 = actual size, F2 = screens side by side,
F3 = window size, F11 or Alt+Enter = fullscreen (move the mouse to the top
edge for the menu bar), F12 = screenshot, F10 = bug report, Esc = leave
fullscreen, or quit.

**Classic and Enhanced.** The game starts in **Classic** mode: the two
screens exactly as a DS draws them (256x192 each, 6-bit colour, the DS's
blending, fading and 3D rasterizer, scaled up with sharp square pixels), with
DS sound (the console's mixer, volume and pitch tables, 10-bit output) and
DS timing (CPU speed, cartridge loading speed). Frames were checked against a
reference DS emulator and match pixel for pixel. **Enhanced** (F5) draws the
3D on the graphics card at 1-4x resolution (F4); the 2D is unchanged. The game
itself runs the same in both modes, so you can switch at any time.

**DS Lite screen (Screen menu, F6).** Makes the screens look like a DS
Lite's instead of exact pixels: the LCD's colours (Pokefan531's measured DS
Lite profile), the red/green/blue stripes and dark lines between pixels
(visible at 3x and larger), the slight motion blur of the LCD, its never
fully black backlight, and four backlight levels. Each part can be turned off.
**View > Show the DS Lite (F7)** draws the console around the screens, at
real proportions (the screens 21.5 mm apart), in six colours; **Gap between
the screens** keeps the real gap without the console. **Console photos:** a
picture in `skins/` (copied next to the game by the build) with a `.txt` of
the same name saying where its screens are (`top x y w h`, `bottom x y w h`,
`name ...`) appears under View > Console colour; the game draws its screens
into the photo's. **View > Actual size
(F8)** shows everything as big as a real DS Lite (each screen 61 mm wide),
using the size your monitor reports; if that is wrong, set
`monitor_width_mm` in `settings.ini`. The drawn pixel grid needs about 3
monitor pixels per DS pixel; at actual size that takes a very dense screen
(a high-resolution laptop or tablet). On most desktop monitors the monitor's
own pixels are about as fine as the DS Lite's, so they provide the grid. The game underneath is
still Classic or Enhanced; with the filter off, Classic is pixel-exact.

**Wireless menu.** The DS's wireless chip is emulated (from melonDS's
Wifi.cpp), and the PC plays a wireless access point called `DQIX-PC`. Turn on
*Connect*, then talk to Sellma at the DQVC counter in Stornway's Quester's
Rest, as on the DS: "Downloading data", the new quests and DQVC's stock.

*Server > Built-in* (the default) needs no internet: `runtime/wfc.c` answers
the game itself (the connection test, Nintendo's login server and download
server) with DQIX's downloads as Nintendo last served them, preserved by
fans (`wfc/`, see `wfc/README.txt`). The last quest file switches on all 64
downloadable quests. The game logs in over HTTPS with the DS's own SSL; as in
the community's "NoSSL" patches, its login and download addresses are
switched to `http://` in memory (the ROM is not changed). Nintendo's login
code also needs a `Date` header in the reply.

**DQVC's weekly stock** follows the original calendar: the 52 sales that ran
every year from July 2010 to 2014 (Opening Sale, Slime Sale, Halloween
Fayre, Winter Fayre, Valentine Fayre, Bridal Fayre, Orb Week...), each
starting on the date it did, picked by your PC's clock. Each sale starts on a
Friday at 08:00 UTC, like the original, and the stock changes every day at
08:00 UTC: each DS gets its own share of the sale's lineup (picked by the
date and the console's MAC address; the original's stock was "generally
different for each player"), and the game shows six of them. Visitors to
your world buy from your Sellma, out of your stock. `wfc/YDQE/dqvc.txt` is the schedule (week
start, item, price, quantity), made by `tools/dqvc_build.py` from the lists
on yab's DQ9 Tools; quantities aren't listed there, so they are 1 for
equipment and 3 for other items unless a preserved file says otherwise. The
download files are RC4-encrypted with the key `XENLONPROJECTKEY` (the
game's code name) and end with 4 bytes that make the whole encrypted file,
read as one number, divisible by 0xC2A030D4; the server builds and seals
each week's file when the game asks.

The other servers go through the internet via libslirp (a user-mode network
stack: DHCP, DNS and TCP become ordinary sockets; nothing on the PC is
changed), with the DS's DNS pointed at a community replacement for Nintendo
Wi-Fi Connection: AltWFC (172.104.88.237) or Wiimmfi (164.132.44.106), or
`wfc_server = <address>` in `settings.ini`. In October 2026 neither served
DQIX any more (Wiimmfi passes it on to Nintendo, which answers "service
discontinued", error 20110). With *Connect* off, the game behaves like a DS
with no access point (Sellma offers DQVC without connecting). Each copy
makes up its own MAC address the first time (`wireless_mac` in
`settings.ini`). F10 reports record the connection: every name looked up,
every request to the built-in server, every connection.

*Unlock all downloadable content now* skips Sellma: the 64
downloadable quests and 23 special guests are on the cartridge, and Nintendo
Wi-Fi Connection only switched them on in the save, so this switches them on
in the save each time the game starts (save layout from the open-source DQIX
save editor).

**Playing with friends far away (Wireless menu).** Tag mode (Erinn's
"Canvass for guests") and visiting or inviting other players (Pavo) need DS
systems next to each other. Here, friends anywhere can do it, up to 4
players. One hosts (*Wireless > Play with friends far away > Host*), the
others *Join* with the address the host's game shows; the host presses *Start the game*
when everyone is in. All the games restart together, each from its own save.

How it works: the DS's wireless needs answers within microseconds, which no
internet connection can give, so *every PC runs every player's DS*: yours in
the window, your friends' unseen (from `dqixcore.dll`, the game built a
second time as a library: one copy per friend's DS, all taking turns on one
thread, so 4 players need about 2 CPU cores), all talking through the emulated air on
that PC (`runtime/air.c`: deterministic, every console counts the same 8 us
ticks and a frame arrives at its sender's tick plus the preamble). Only the
buttons travel between the PCs (`runtime/session.c`, TCP via the host): each
press is used a few frames later (the input delay, chosen from the pings) on
every PC at once. The game is deterministic, so every PC computes exactly
the same; fingerprints of memory every 2 seconds catch a PC that doesn't
(a different build, say). The wireless chip now does what DSes do between
themselves: frames, beacons, the MP CMD / REPLY / ACK exchanges (ported from
melonDS's Wifi.cpp).

To let friends in, the host's game asks the home router to forward a port
(UPnP) and shows the internet address to give out (copied to the
clipboard). If the router won't, forward TCP port 27900 (`session_port` in
`settings.ini`) to the host's PC, or all of you use a free virtual LAN
(Tailscale, ZeroTier, Radmin VPN) and use the host's address there. Everyone
needs the same version of the game. The built-in Wi-Fi (DQVC, the
downloads) works in sessions, with the host's files for everyone; other
online servers are off then. On Windows, hosting or joining opens a window
with the address (and a Copy button), who has joined, and Start the game.
If a session stands still, the title
bar says which DS it is waiting for, and `session\front.log` next to the game
records the session (send it with a bug report). Your name in sessions: *Your name...*
(`player_name` in `settings.ini`). Tests: `DQIX_SESSION=host[:port[:players]]`
or `join:address[:port]`, or `local:N` (N consoles on one PC, the others'
buttons from `DQIX_INPUT_1..3`); `DQIX_AIR=name,index,count` connects copies
on one PC without a session; `DQIX_PROCESSES=1` runs the hidden DSes as
separate processes instead of `dqixcore`. In tag mode, press A once more after Erinn's "Go get
'em!": the canvassing starts then.

**Settings.** The game remembers every menu choice in `settings.ini` next to `dqix.exe` (delete it to go
back to the defaults). Enhanced needs OpenGL 3.3 (nearly every PC from the
last 12 years); without it, the game stays in Classic. `set DQIX_SOFTWARE=1`
in `play.bat` forces Classic.

**Versions.** Every update has a version number (in the title bar, Help >
About and `runtime/version.h`).

**Bug reports.** F10 (or a crash) writes `reports/<version>-<date-time>/`
next to the game, so you can see which update a report came from. After F10 a
box opens to describe what went wrong (saved as `description.txt`). Each
report has a screenshot, every input since launch, the save as it was at launch and
now, the clock start, the log, and `perf.txt` (how long each of the last 30
seconds of frames took, split into game code, 2D, 3D and display). The
emulation is deterministic, so replaying a report on the same build
reproduces the session exactly (reports from older builds may play out
differently, because timing changed):

```
DQIX_RTC=<rtc_base from info.txt> DQIX_INPUT_FILE=input.txt ./dqix <rom> <extract> <frame+1> 1 <copy of boot.sav>
```

**Patches.** `patches.txt` overrides individual instructions when the game is
recompiled (the ROM is never changed). It currently removes the anti-piracy
memory leak in the file loader that froze the game after the first battle.

## Status

| | ARM9 (game) | ARM7 (services) |
|---|---|---|
| Functions translated | 14,815 (main, ITCM, 35 overlays) | 962 (found by our own discovery) |
| Instructions translated | 728k, none left untranslated | 38.8k, none left untranslated |
| Decoder vs LLVM disassembler | 723,757 checked, 0 disagreements | 38,704 checked, 0 disagreements |

Behaviour tests (clang-compiled ARM and Thumb functions vs the same C compiled
natively, 24 functions × random inputs): recompiler 192,000 checks and
interpreter 48,000 checks, 0 failures.

## Runtime status

| Piece | State |
|---|---|
| Boot like the DS firmware, both CPUs (binaries, header, chip ID, user settings) | done |
| Firmware flash: Wi-Fi calibration, 3 access-point slots, user settings (with CRCs); writes kept for the session | done |
| ARM9 memory: main RAM, ITCM/DTCM, shared WRAM per WRAMCNT, palette, OAM, VRAM banks | done |
| ARM7 memory: ARM7 WRAM, shared WRAM, VRAM C/D | done |
| CPU modes with banked sp/lr, CP15 | done |
| BIOS (both): Div, Sqrt, CpuSet, CpuFastSet, CRC16, LZ77, RLE, BitUnPack, IntrWait, Halt, WaitByLoop; ARM7 sound tables | done |
| Interrupts, timers (incl. cascade) and DMA per CPU; display timing | done |
| IPC: sync register + both FIFOs with interrupts | done |
| Cartridge: ROM reads (B7/B8) for either CPU, card DMA | done |
| Save chip (64 KB EEPROM on backup SPI), saved to a .sav file | done |
| ARM7 devices: firmware flash (user settings), touch screen, power management, RTC (PC clock + emulated time) | done |
| Game threads on native coroutines, both CPUs (SaveContext / LoadContext / IRQ switch replaced) | done |
| Two CPUs: the ARM7 runs as a coroutine whenever the ARM9 waits or time advances | done |
| Interpreter for code that changes at run time (encrypted anti-piracy, mislabeled code) | done |
| Timing: per-instruction cycle counts (recompiler), cartridge transfer speed, BIOS call cost | done |
| Sound: 16 channels (PCM8/16, IMA-ADPCM, PSG, noise), FIFO timing, capture (reverb), mixer, BIOS volume/pitch tables | done |
| 2D graphics: both engines, all BG modes, sprites, windows, 6-bit blending/brightness, mosaic, display capture | done |
| 3D: geometry engine (FIFO, matrices, lighting, clipping), rasterizer with DS fixed-point rules, textures (all formats), toon/highlight, alpha blending, shadow volumes, anti-aliasing, fog, edge marking | done |
| Input (keyboard, mouse as touch, gamepad) and scripted input for testing | done |

## Running it headless (Linux, for testing)

```
python3 recomp/build.py --extract <extract/usa> --config <config/usa/arm9> --out gen --hle hle.txt --interpret interpret.txt
python3 recomp/arm7.py  --extract <extract/usa> --out gen7 --hle hle7.txt
make -f build.mk GEN=gen GEN7=gen7 DEFS=-DRECOMP_TRACE
./dqix <baserom_dqix_usa.nds> <extract/usa> [frames] [log level 1-3] [save file]
make -f build.mk ... FRONTEND=sdl OUT=dqix-sdl     # with a window
tools/where.sh      # if it hangs: backtrace + both CPUs' state
```

Testing aids (environment variables):

```
DQIX_INPUT="1780:touch=128,160:10;2400:A:20"   scripted input (frame:buttons+touch=x,y:frames)
DQIX_INPUT_FILE=input.txt                      the same from a file (reports use this)
DQIX_WATCH=addr                                log every ARM9 write to that word, with a call trace
DQIX_DUMP=dir,30                               write every 30th frame as .ppm
DQIX_RTC=1760000000                            fixed clock start (reproducible runs)
DQIX_PROF=from,to                              function-entry profile for those frames
DQIX_THREADS=1                                 at exit: every game thread's recent calls
DQIX_TRACEFN=addr,count[,from]                 call trace when a function is entered
DQIX_POLYDUMP=frame,x,y                        3D polygons covering a pixel
DQIX_WAV=out.wav                               record the sound (headless build)
DQIX_SNDLOG=1                                  log sound channel starts
DQIX_CRC=out.txt                               a checksum of every frame (compare runs)
DQIX_ONLINE=builtin / DQIX_ONLINE=1.2.3.4      connect: the built-in server / through this DNS (tests)
DQIX_WFC_DIR=path                              the built-in server's files (default: wfc)
DQIX_WFCDEBUG=1                                print every request to the built-in server and its reply
DQIX_RAMDUMP=dir,frame,...                     main RAM at those frames (debugging)
DQIX_MAC=00:09:bf:12:34:56                     the console's MAC (replaying a report's mac line)
DQIX_WIFILOG=1 / DQIX_NETLOG=1 / DQIX_NETHEX=1 log the wireless chip / network / every frame in hex
DQIX_DUMP_AT=100,200                           with DQIX_DUMP=dir,1: only those frames
DQIX_MODE=enhanced / DQIX_NO_SETTINGS=1        window build: force a mode / ignore settings.ini
```

## Layout

```
recomp/build.py        ARM9 recompiler: modules, functions, calls, overlays, jump tables
recomp/arm7.py         ARM7 recompiler: discovers functions itself, reuses build.py
recomp/insn.py         ARMv5TE ARM + Thumb instruction decoder that emits C
runtime/cpu.h          CPU state + helpers the generated code uses
runtime/nds.h          runtime state shared by the files below
runtime/mem.c, mem7.c  memory maps (ARM9, ARM7)
runtime/io.c           timing, per-CPU IRQs/timers/DMA, ARM9 registers, divider/sqrt
runtime/io7.c          ARM7 registers: SPI (firmware, touch, power), RTC, sound regs
runtime/card.c         cartridge ROM reads and the save chip
runtime/ipc.c          IPC sync + FIFOs between the CPUs
runtime/cpu_bios.c     CPU modes, interrupt entry, CP15, BIOS (both CPUs), logging
runtime/threads.c      game threads on coroutines (both CPUs) and ARM7 scheduling
runtime/interp.c       ARM/Thumb interpreter for code that isn't recompiled
runtime/coro.h         coroutines: ucontext (Linux) / Fibers (Windows)
runtime/dispatch.c     indirect calls, overlay detection, budgets, call trace
runtime/main.c         ROM loading, firmware boot state, entry
runtime/gpu2d.c        both 2D engines, per scanline
runtime/gpu3d.c        3D geometry engine + software rasterizer
runtime/sound.c        sound channels and mixer
runtime/wifi.c         the wireless chip and the access point DQIX-PC (after melonDS)
runtime/net.c          the access point's network (libslirp)
runtime/wfc.c          the built-in Nintendo Wi-Fi Connection server (DNS, TCP, login, downloads)
wfc/                   its files, per game code (not in git; see wfc/README.txt)
tools/dqvc_build.py    builds wfc/YDQE/dqvc.txt (DQVC's weekly stock) from saved pages
third_party/libslirp   user-mode network stack (BSD licence, as vendored by melonDS)
runtime/input.c        DS buttons / touch from the frontend or a script
runtime/frontend_sdl.c window, input, audio, pacing, settings (SDL2)
runtime/menu_win32.c   the menu bar (Windows)
runtime/gl_render.c    OpenGL: Enhanced 3D, composition, the DS Lite screen + console
runtime/frontend_none.c headless frontend (tests)
runtime/report.c       F10 / crash reports (inputs, saves, screenshot, log)
patches.txt            instruction overrides applied when recompiling
build-pc.bat           one-click Windows build (installs MSYS2 first time)
build-windows.sh       the build itself (runs inside MSYS2)
hle.txt / hle7.txt     functions the runtime provides instead (thread switch)
interpret.txt          functions run in the interpreter (encrypted in the ROM)
build.mk               makefile (make -f build.mk)
tools/where.sh         run, then show where the game is
tests/                 decoder check vs LLVM; behaviour tests for recompiler + interpreter
recomp.bat             regenerate the ARM9 C on Windows (developer build)
Install Dragon Quest IX.bat  the setup window (tools/setup-wizard.ps1): ROM version, folder,
                       shortcuts, Apps entry (tools/uninstall.ps1), then BUILD.bat
BUILD.bat              the one-click build: your ROM -> the game folder
tools/build-game.sh    what BUILD.bat runs (inside MSYS2; also works on Linux)
tools/extract_rom.py   takes the code out of a DS ROM (same output as dsd rom extract)
config/usa/arm9        function names and relocations from the dqix-decomp project
rom/, extras/          where your ROM and the optional extras go
```

Generated C is derived from the game: never share it. The project itself contains no game code.

## How it works

* ARM9 function boundaries, modes and overlay targets come from the decomp's
  `symbols.txt` / `relocs.txt`. The ARM7 has no symbol map: `arm7.py` finds
  functions from the entry point, calls, code pointers in literal pools and
  data tables, and prologues in uncovered code.
* Each function becomes `void fn_<module>_<addr>(Cpu* c)`; registers live in
  `c->r[]`. Branches become `goto`, calls C calls, returns `return`.
* Calls into overlays that share an address check which overlay is loaded
  (its code bytes are compared against the overlay file).
* Jump tables (branch tables, address tables, Thumb offset tables, computed
  jumps into unrolled loops) become `switch` statements over local labels.
* Literal-pool loads are folded into constants.
* Indirect calls go through `dispatch_call()`; an address with no recompiled
  function runs in the interpreter.
* `mov lr, pc` / `add lr, pc, #0` followed by a jump is a call. A tail call
  that set a different return address continues there when the callee returns.
* The SDK's thread switch is replaced by native coroutines, one per game
  thread, on both CPUs. The ARM7 runs as another coroutine: whenever the
  ARM9 waits or time moves on it runs until it waits, then hands back.
* Time follows the DS clock. The recompiler adds each straight run of
  instructions' cycle cost; every 512 ARM9 cycles (2,048 ARM7) the runtime
  turns them into bus time, runs the other CPU and delivers interrupts.
* `hooks.txt` lists functions the runtime wraps: the recompiled function is
  emitted as `<name>_orig` and `hook_<Name>(c, orig)` (runtime) is called
  instead. USA addresses; `tools/region.py` moves them for the EU ROM.
* HD text (`runtime/hdtext.c`): the game blits each letter (1 bit per pixel,
  12 rows) into a window's 4-bit tile buffer in RAM (`GlyphBlit`, hooked).
  Every pixel of a letter's box gets a tag (letter instance, x, y, ink);
  tags follow DMA, CpuSet/CpuFastSet and `Memcpy` (hooked) into VRAM and are
  dropped on any other write. The 2D engines read the tags of text
  background pixels: the front-most tagged pixels are left out of the
  picture and their letters listed with screen position and ink colour.
  `gl_render.c` draws each letter as a quad from an atlas (8 px per DS
  pixel, rendered with stb_truetype, scaled to the DS 'H' and baseline),
  clipped to the letter boxes the game shows. Game logic never sees any of
  it (the per-frame CRC with HD text on matches the one with it off in
  Classic mode).
* Textures (`gl_render.c`): each decoded texture is keyed by FNV-1a 64 of
  width, height and RGBA (see-through texels as 0). `textures/**/<key>.png`
  replaces it (loaded on first use, mipmapped, at most 3000 in the GPU);
  dumping writes `textures/dump/<key>.png`. Upscaled textures run xBR level
  1 at 4x (2x above 128x128 texels, none above 256x256) on the CPU when a
  texture is decoded. Smooth 2D runs the same xBR rule in the compose
  shader on the 2D layers (not where 3D shows).

## Later

* Tune the DS Lite screen filter against photos of a real DS Lite running
  DQIX (same frames rendered here): colours per backlight level, black glow,
  pixel grid from a macro shot, motion blur from slow-motion video; plus the
  same shots of the port on the monitor with identical camera settings.
* The downloadable quests could arrive one week at a time, as Nintendo
  released them (the quest file format is decoded; the release order is
  needed).
* Save states (a state would only fit the build that made it).
* Enhanced extras: widescreen, top-screen map.

## Known limits

* Only copies of this PC version can play together: a real DS can't join
  (its wireless needs answers within microseconds).
* Anti-piracy and a few functions the decomp marks as data run in the
  interpreter (correct, but slower).
* 528 `bx r12` long-call stubs go through the dispatcher (later optimisation).
