#!/bin/sh
# Dragon Quest IX for PC on Linux (and the Steam Deck): turns your own DQIX ROM
# into the PC game, in the "game" folder next to this file.
#
#   ./BUILD.sh <your ROM>              build (a .nds, or a .zip with one inside)
#   ./BUILD.sh                         the first .nds in the "rom" folder, or the last ROM used
#   ./BUILD.sh --prepare <your ROM>    for RecompLauncher (recomplauncher.json "prepare"):
#                                      the same, and if it fails, its last line says why
#
# Needs gcc, make, python3 and SDL2's development files
# (Debian/Ubuntu: sudo apt install build-essential python3 libsdl2-dev;
#  Fedora: sudo dnf install gcc make python3 SDL2-devel;
#  Arch/SteamOS: sudo pacman -S base-devel python sdl2).
cd "$(dirname "$0")" || exit 1

fail() { echo; echo "BUILD FAILED: $*"; exit 1; }

PREPARE=
if [ "$1" = "--prepare" ]; then PREPARE=1; shift; fi
ROM="$1"
if [ -z "$ROM" ]; then
    for f in rom/*.nds rom/*.zip; do [ -f "$f" ] && { ROM="$f"; break; }; done
fi
if [ -z "$ROM" ] && [ -f rom/last-rom.txt ]; then ROM=$(cat rom/last-rom.txt); fi
[ -n "$ROM" ] || fail "no ROM given. Run ./BUILD.sh <your Dragon Quest IX .nds>, or put it in the \"rom\" folder."
[ -f "$ROM" ] || fail "the ROM wasn't found (\"$ROM\"). Choose your own Dragon Quest IX .nds file, or a .zip with it inside."

# the tools the build needs, each named if missing
missing=
for t in gcc make python3 sdl2-config; do command -v $t >/dev/null 2>&1 || missing="$missing $t"; done
if [ -n "$missing" ]; then
    case "$missing" in *sdl2-config*) missing="$missing (SDL2's development files)";; esac
    fail "missing build tools:$missing. Install gcc, make, python3 and SDL2's development files (see the top of BUILD.sh), then try again."
fi

# a ROM in a zip: take the .nds out into the "rom" folder
mkdir -p rom
case "$ROM" in
*.zip|*.ZIP)
    NDS=$(python3 - "$ROM" rom <<'PY'
import sys, zipfile, os
try:
    z = zipfile.ZipFile(sys.argv[1])
    names = [n for n in z.namelist() if n.lower().endswith('.nds')]
    if names:
        out = os.path.join(sys.argv[2], os.path.basename(names[0]))
        with open(out, 'wb') as f: f.write(z.read(names[0]))
        print(out)
except Exception:
    pass
PY
)
    [ -n "$NDS" ] || fail "no Dragon Quest IX ROM was found in the zip. Choose the .nds file itself, or a .zip with the .nds in it."
    ROM="$NDS" ;;
esac
case "$ROM" in /*) ;; *) ROM="$(pwd)/$ROM" ;; esac
echo "$ROM" > rom/last-rom.txt
echo "ROM: $ROM"

if sh tools/build-game.sh "$ROM"; then
    echo
    echo "Finished. Play with game/play.sh (or game/dqix)."
    exit 0
fi
if [ -n "$PREPARE" ] && [ -f build/last-error.txt ]; then fail "while $(cat build/last-error.txt)"; fi
fail "the text above says why."
