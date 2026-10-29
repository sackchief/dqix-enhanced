#!/usr/bin/env python3
"""Exercise the exact AppImage rsync filter against synthetic private-data sentinels.

No real ROMs, saves, user workspaces, or game outputs are accessed by this test.
"""
from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "packaging" / "linux" / "appimage-filter.txt"
SENTINEL = "APPIMAGE_PRIVATE_PAYLOAD_SENTINEL_2026"
ALLOWED = (
    "README.md",
    "build.mk",
    "runtime/legitimate-source.c",
    "config/fixture.txt",
    "tools/build-game.sh",
    "tools/extract_rom.py",
    "tools/region.py",
    "third_party/legitimate-helper.c",
    "fonts/labels.bin",
)
FORBIDDEN = (
    "packaging/linux/gui.py",
    "private.nds",
    "private.sav",
    "runtime/private.bin",
    "config/rom-dump.bin",
    "game/data/dqix.nds",
    "game/data/dqix.sav",
    "game/data/code/generated-from-rom.c",
    "build/recomp/main_00.c",
    "build/recomp7/arm7_00.c",
    "build/obj/game.o",
    "extract/arm9/arm9.bin",
    "rom/private.nds",
    "testbuild/generated.c",
    "test-build/generated.c",
    "Mods/custom/mod.pack",
    "mods/custom/mod.pack",
    "music/custom.wav",
    "textures/custom.bmp",
    "gen/generated-overlay.c",
    "gen7/generated-arm7.c",
    "obj/generated.o",
    "dist/private.sav",
    "wfc/YDQE/_list.txt",
    ".git/config",
    "tests/fixture.nds",
    "unknown/private.txt",
    "runtime/game/generated-from-rom.c",
    "runtime/build/generated_00.c",
    "runtime/extract/arm9.nds",
    "runtime/rom/secret.nds",
    "runtime/testbuild/generated.c",
    "third_party/Mods/custom.mod",
    "third_party/music/custom.wav",
    "third_party/textures/custom.png",
    "third_party/wfc/_list.txt",
    "third_party/gen/generated.c",
    "third_party/gen7/generated.c",
    "third_party/obj/generated.o",
)
FORBIDDEN_DIRS = {
    "game", "build", "extract", "rom", "testbuild", "test-build", "obj",
    "gen", "gen7", "mods", "music", "textures", "wfc", "release", "reports",
}
FORBIDDEN_SUFFIXES = {
    ".nds", ".sav", ".dsv", ".srm", ".sra", ".bin", ".o", ".exe", ".dll", ".so",
    ".pyc", ".ppm", ".bmp", ".wav",
}


def write_file(root: Path, relative: str, content: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main() -> int:
    if not POLICY.is_file():
        raise SystemExit(f"AppImage payload policy is missing: {POLICY}")
    if shutil.which("rsync") is None:
        raise SystemExit("rsync is required to test the actual AppImage payload policy")

    with tempfile.TemporaryDirectory(prefix="dqix-appimage-payload-test-") as temp:
        root = Path(temp)
        source, destination = root / "source", root / "payload"
        source.mkdir()
        for relative in ALLOWED:
            write_file(source, relative, "legitimate, synthetic source fixture\n")
        for relative in FORBIDDEN:
            write_file(source, relative, SENTINEL + "\n")

        command = [
            "rsync",
            "-a",
            f"--filter=merge {POLICY}",
            str(source) + "/",
            str(destination) + "/",
        ]
        subprocess.run(command, check=True, capture_output=True, text=True)

        failures: list[str] = []
        for relative in ALLOWED:
            if not (destination / relative).is_file():
                failures.append(f"allowlisted source missing: {relative}")
        for relative in FORBIDDEN:
            if (destination / relative).exists():
                failures.append(f"forbidden sentinel path copied: {relative}")

        if destination.exists():
            forbidden_dir_names = {name.casefold() for name in FORBIDDEN_DIRS}
            for path in destination.rglob("*"):
                relative = path.relative_to(destination).as_posix()
                if path.is_dir() and path.name.casefold() in forbidden_dir_names:
                    failures.append(f"forbidden directory present: {relative}/")
                elif path.is_file():
                    suffix = path.suffix.casefold()
                    approved_font_table = relative == "fonts/labels.bin"
                    if suffix in FORBIDDEN_SUFFIXES and not (suffix == ".bin" and approved_font_table):
                        failures.append(f"forbidden extension present: {relative}")
                    try:
                        if SENTINEL in path.read_text(encoding="utf-8", errors="ignore"):
                            failures.append(f"synthetic private-data sentinel leaked: {relative}")
                    except OSError:
                        failures.append(f"could not inspect payload fixture: {relative}")

        if failures:
            raise SystemExit("AppImage payload policy test failed:\n- " + "\n- ".join(failures))
        print(
            "AppImage payload policy passed: "
            f"{len(ALLOWED)} allowlisted fixtures retained; "
            f"{len(FORBIDDEN)} private-data sentinels excluded."
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
