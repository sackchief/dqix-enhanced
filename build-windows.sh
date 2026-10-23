#!/bin/sh
# Build the PC version on Windows. Run by build-pc.bat inside MSYS2 (UCRT64).
# Everything stays in your dqix-decomp folder; the result is build/pc/dqix.exe.
set -e
cd "$(dirname "$0")"
D=..
OUT=$D/build/pc
mkdir -p "$OUT"
echo "== Recompiling the game's code to C (about a minute)"
python recomp/build.py --extract $D/extract/usa --config $D/config/usa/arm9 --out $D/build/recomp --hle hle.txt --interpret interpret.txt --patch patches.txt --tags tags.txt --wrap wrap.txt
python recomp/arm7.py --extract $D/extract/usa --out $D/build/recomp7 --hle hle7.txt
echo "== Compiling (the first time takes a while; later builds only redo what changed)"
# dqixcore.dll: the game again as a library, for the other players' DSes in a session (Wireless menu)
make -j"$(nproc)" -f build.mk OS=Windows_NT GEN=$D/build/recomp GEN7=$D/build/recomp7 OBJ=$D/build/obj FRONTEND=sdl OUT="$OUT/dqix.exe" CORE="$OUT/dqixcore.dll" \
     CFLAGS="-O1 -w" DEFS=-DRECOMP_TRACE SDL_CFLAGS="$(sdl2-config --cflags)" SDL_LIBS="-lSDL2" \
     LDFLAGS="-static-libgcc -Wl,--stack,67108864" "$OUT/dqix.exe" core
cp /ucrt64/bin/SDL2.dll "$OUT/"
mkdir -p "$OUT/skins"
cp skins/*.png skins/*.txt "$OUT/skins/" 2>/dev/null || true     # console photos (View > Console colour)
mkdir -p "$OUT/fonts" && cp fonts/* "$OUT/fonts/"                 # HD text (fonts/README.txt)
for g in wfc/*/; do                                              # the built-in Wi-Fi server's files (Wireless menu)
    [ -f "$g/_list.txt" ] || continue
    mkdir -p "$OUT/$g" && cp "$g"* "$OUT/$g"
done
cat > "$OUT/play.bat" <<'BAT'
@echo off
rem Dragon Quest IX (PC). Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,
rem Enter = START, Right Shift = SELECT, mouse = touch screen, Tab = fast forward.
rem Every setting is in the menu bar at the top of the window.
cd /d "%~dp0"
dqix.exe ..\..\extract\baserom_dqix_usa.nds ..\..\extract\usa 0 1 dqix.sav
if errorlevel 1 pause
BAT
echo
echo "== Done. Double-click $OUT/play.bat to play."
