#!/bin/sh
# Turn a Dragon Quest IX ROM into the PC game. Run by BUILD.bat inside MSYS2
# (UCRT64); also works on Linux with gcc, make, python3 (+ PyYAML) and SDL2.
#
#   tools/build-game.sh <rom.nds>
#
# Everything is made inside this folder:
#   build/   working files (the extracted code, the generated C, objects)
#   game/    the result: dqix.exe and everything it needs, including a copy
#            of the ROM, so the folder can be moved anywhere
set -e
cd "$(dirname "$0")/.."
ROM="$1"
[ -f "$ROM" ] || { echo "No ROM given (or it doesn't exist): $ROM"; exit 1; }

echo "== Checking the ROM"
# no checksum: any version can be tried. The game code in the header says which it is.
CODE=$(dd if="$ROM" bs=1 skip=12 count=4 2>/dev/null | tr -cd 'A-Za-z0-9') || true
case "$CODE" in
YDQE) echo "   Dragon Quest IX (USA)" ;;
YDQP) echo "   Dragon Quest IX (Europe)" ;;
YDQJ) echo "   Dragon Quest IX (Japan): only the USA and European versions are supported; trying anyway" ;;
"")   echo "   Couldn't read the ROM file: $ROM"
      echo "   Is it still being copied, or open in another program? Is it a OneDrive file that isn't downloaded"
      echo "   (right-click it, Always keep on this device)? Then run this again."
      exit 1 ;;
*)    echo "   This doesn't look like Dragon Quest IX (game code '$CODE'); trying anyway" ;;
esac

PY=python3; command -v $PY >/dev/null 2>&1 || PY=python
echo "== Extracting the game's code"
$PY tools/extract_rom.py "$ROM" build/extract
echo "== Recompiling it to C (about a minute)"
# the symbol map and address lists are the USA version's; region.py moves them for the European one
REGION=$($PY tools/region.py "$ROM" build/extract build/region)
if [ "$REGION" = eur ]; then CFG=build/region; CONFIG=build/region/arm9; else CFG=.; CONFIG=config/usa/arm9; fi
# code compiled for the other version (or before this check existed) doesn't link with this one: start over
if [ "$(cat build/obj/region.txt 2>/dev/null)" != "$REGION" ]; then
    rm -rf build/recomp build/obj/gen
    mkdir -p build/obj && echo "$REGION" > build/obj/region.txt
fi
$PY recomp/build.py --extract build/extract --config $CONFIG --out build/recomp --hle $CFG/hle.txt --interpret $CFG/interpret.txt --patch $CFG/patches.txt --tags $CFG/tags.txt --hooks $CFG/hooks.txt
$PY recomp/arm7.py --extract build/extract --out build/recomp7 --hle hle7.txt

# The program's icon: the game's own (the one the DS menu shows), made from the
# ROM like everything else, so nothing from the game is kept in this project.
# Optional: without it the exe has Windows' plain icon.
make_icon() {    # make_icon <rom> <dir> -> sets RES
    RES=
    mkdir -p "$2"
    if $PY tools/rom_icon.py "$1" "$2/dqix.ico" && command -v windres >/dev/null 2>&1; then
        cat > "$2/dqix.rc" <<'RC'
1 ICON "dqix.ico"
1 VERSIONINFO
FILEVERSION 0,0,0,0
PRODUCTVERSION 0,0,0,0
FILEOS 0x40004
FILETYPE 1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "FileDescription", "Dragon Quest IX"
      VALUE "ProductName", "DQIX Enhanced"
      VALUE "OriginalFilename", "dqix.exe"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
RC
        if (cd "$2" && windres dqix.rc -O coff -o dqix_res.o); then RES="$2/dqix_res.o"; echo "   icon: the game's own"; fi
    fi
}

echo "== Compiling (the first time takes a while; later builds only redo what changed)"
mkdir -p game
case "$(uname -s)" in
MINGW*|MSYS*)
    EXE=game/dqix.exe
    make_icon "$ROM" build/icon
    # dqixcore.dll: the game again as a library, for the other players' DSes in a session
    make -j"$(nproc)" -f build.mk OS=Windows_NT GEN=build/recomp GEN7=build/recomp7 OBJ=build/obj FRONTEND=sdl OUT="$EXE" CORE=game/dqixcore.dll \
         CFLAGS="-O1 -w" DEFS=-DRECOMP_TRACE SDL_CFLAGS="$(sdl2-config --cflags)" SDL_LIBS="-lSDL2" \
         LDFLAGS="-static-libgcc -Wl,--stack,67108864" RES="$RES" "$EXE" core
    cp /ucrt64/bin/SDL2.dll game/ ;;
*)
    EXE=game/dqix
    make -j"$(nproc)" -f build.mk GEN=build/recomp GEN7=build/recomp7 OBJ=build/obj FRONTEND=sdl OUT="$EXE" CORE=game/dqixcore.so PIC=-fPIC \
         CFLAGS="-O1 -w" DEFS=-DRECOMP_TRACE SDL_CFLAGS="${SDL_CFLAGS:-$(sdl2-config --cflags)}" SDL_LIBS="${SDL_LIBS:-$(sdl2-config --libs)}" "$EXE" core ;;
esac

echo "== Putting the game folder together"
mkdir -p game/data game/skins
rm -rf game/data/code && cp -r build/extract game/data/code     # the overlays (the game checks which is loaded)
if ! cmp -s "$ROM" game/data/dqix.nds; then cp "$ROM" game/data/dqix.nds; fi
cp skins/*.png skins/*.txt game/skins/ 2>/dev/null || true
# optional files: the built-in Wi-Fi server's downloads and DQVC schedule
for src in extras/wfc wfc; do
    for g in "$src"/*/; do
        [ -f "$g/_list.txt" ] || continue
        mkdir -p "game/wfc/$(basename "$g")" && cp "$g"* "game/wfc/$(basename "$g")/"
    done
done
# DQVC's news: the one message that survives (Nintendo's last), from the fans'
# archive of Nintendo's download server. The quests and DQVC's sales are
# built into the game; the news is Square Enix's text, so it is fetched, not kept here.
if [ "$REGION" = eur ]; then GC=YDQP; NEWS=infomes_45.bin; else GC=YDQE; NEWS=infomes_47.bin; fi
if [ ! -f game/wfc/$GC/$NEWS ]; then
    mkdir -p game/wfc/$GC
    if curl -fsSL --max-time 30 -o game/wfc/$GC/$NEWS \
         https://raw.githubusercontent.com/InternalLoss/DLS1-Files/master/$GC/$NEWS; then
        echo "   DQVC news: fetched"
    else
        rm -f game/wfc/$GC/$NEWS; echo "   DQVC news: not fetched (optional; Sellma just has no news)"
    fi
fi
cat > game/play.bat <<'BAT'
@echo off
rem Dragon Quest IX (PC). Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,
rem Enter = START, Right Shift = SELECT, mouse = touch screen, Tab = fast forward.
rem Every setting is in the menu bar at the top of the window.
cd /d "%~dp0"
dqix.exe data\dqix.nds data\code 0 1 dqix.sav
if errorlevel 1 pause
BAT
cat > game/play.sh <<'SH'
#!/bin/sh
cd "$(dirname "$0")" && exec ./dqix data/dqix.nds data/code 0 1 dqix.sav
SH
chmod +x game/play.sh
echo
case "$(uname -s)" in MINGW*|MSYS*) echo "== Done. The game is in the 'game' folder: double-click dqix.exe." ;; *) echo "== Done. The game is in the 'game' folder: run play.sh." ;; esac
