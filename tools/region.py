#!/usr/bin/env python3
"""Adapt the recompiler's inputs to the version of the ROM being built.

  region.py <rom.nds> <extract dir> <out dir>

The symbol map (config/usa/arm9) and the address lists (hle.txt, tags.txt,
patches.txt, interpret.txt, hooks.txt) are made for the USA ROM (YDQE). The European ROM
(YDQP) is the same program with 16 bytes more in the main ARM9 code: two
functions near the start (func_0200f3a4 and func_0200fb08, 8 bytes each) pick
the game's language from the DS's settings. Everything after them in the main
code moves; the data after the code, ITCM, DTCM, every overlay and the ARM7
stay where they are. The secure area's BIOS call stubs (0x02000000-0x020007ff)
are in another order on each ROM: they are found by their SVC number.

For the USA ROM the files are used as they are. For the European ROM this
writes <out>/arm9 (the symbol map) and <out>/hle.txt, tags.txt, patches.txt,
interpret.txt, hooks.txt with every main-module address moved, after checking each call
in the map against the ROM's own instructions.

Prints the region (usa or eur); the build uses it to choose the files.
"""
import os
import re
import shutil
import struct
import sys

# USA main-module address -> European one: (first, end, shift)
EUR_SHIFTS = [
    (0x0200F3B8, 0x0200FB20, 0x08),     # rest of func_0200f3a4 up to func_0200fb08's insert
    (0x0200FB20, 0x020EEBD0, 0x10),     # everything else in the code (and its constructor table)
]

# secure area: BIOS call stubs, `swi N; bx lr` (Mod: `swi 9; mov r0, r1; bx lr`)
SVC_STUBS = {
    "SoftReset": 0x00, "WaitByLoop": 0x03, "IntrWait": 0x04, "VBlankIntrWait": 0x05,
    "Halt": 0x06, "Div": 0x09, "Mod": 0x09, "CpuSet": 0x0B, "CpuFastSet": 0x0C,
    "Sqrt": 0x0D, "GetCRC16": 0x0E, "IsDebugger": 0x0F, "BitUnPack": 0x10,
    "LZ77UnCompReadNormalWrite8bit": 0x11, "LZ77UnCompReadByCallbackWrite16bit": 0x12,
    "HuffUnCompReadByCallback": 0x13, "RLUnCompReadNormalWrite8bit": 0x14,
    "RLUnCompReadByCallbackWrite16bit": 0x15,
}
SECURE_END = 0x02000800


def game_code(rom):
    with open(rom, "rb") as f:
        return f.read(16)[12:16].decode("ascii", "replace")


def find_stubs(arm9):
    """SVC number -> stub addresses in the secure area: (plain, mod)."""
    plain, mod = {}, {}
    for o in range(0, SECURE_END - 0x02000000 - 6, 2):
        h0, h1, h2 = struct.unpack_from("<HHH", arm9, o)
        if h0 >> 8 != 0xDF:
            continue
        n = h0 & 0xFF
        if h1 == 0x4770:
            plain.setdefault(n, []).append(0x02000000 + o)
        elif h1 == 0x1C08 and h2 == 0x4770:
            mod.setdefault(n, []).append(0x02000000 + o)
    return plain, mod


class Map:
    def __init__(self, arm9):
        plain, mod = find_stubs(arm9)
        self.stubs = {}
        for name, n in SVC_STUBS.items():
            cands = (mod if name == "Mod" else plain).get(n, [])
            if len(cands) != 1:
                sys.exit(f"region: the BIOS call {name} (SVC {n:#x}) is not where it should be in this ROM")
            self.stubs[name] = cands[0]
        self.secure = {}           # USA stub address -> this ROM's, filled from the symbol map

    def main(self, a):
        if a < SECURE_END and a in self.secure:
            return self.secure[a]
        for first, end, shift in EUR_SHIFTS:
            if first <= a < end:
                return a + shift
        return a


ADDR_RE = re.compile(r"addr:0x([0-9a-f]+)")
SIZE_RE = re.compile(r"size=0x([0-9a-f]+)")
REL_RE = re.compile(r"^from:0x([0-9a-f]+) kind:(\S+) to:0x([0-9a-f]+) module:(\S+)(.*)$")


def map_symbols(src, dst, mp, is_main):
    with open(src) as f, open(dst, "w") as out:
        for line in f:
            m = ADDR_RE.search(line)
            if m and is_main:
                a = int(m.group(1), 16)
                na = mp(a)
                s = SIZE_RE.search(line)
                if s and a >= SECURE_END:
                    end = a + int(s.group(1), 16)
                    line = line[:s.start(1)] + f"{mp(end - 1) + 1 - na:x}" + line[s.end(1):]
                line = ADDR_RE.sub(f"addr:0x{na:08x}", line, count=1)
                if na != a:                                  # names made from the address follow it
                    line = line.replace(f"_{a:08x} ", f"_{na:08x} ", 1)
            out.write(line)


def map_relocs(src, dst, mp, is_main):
    if not os.path.exists(src):
        return []
    rel = []
    with open(src) as f, open(dst, "w") as out:
        for line in f:
            m = REL_RE.match(line.rstrip("\n"))
            if not m:
                out.write(line)
                continue
            fr, kind, to, mod, rest = int(m.group(1), 16), m.group(2), int(m.group(3), 16), m.group(4), m.group(5)
            if is_main:
                fr = mp(fr)
            if mod == "main":
                to = mp(to)
            rel.append((fr, kind, to, mod))
            out.write(f"from:0x{fr:08x} kind:{kind} to:0x{to:08x} module:{mod}{rest}\n")
    return rel


def bl_target(code, base, a, thumb):
    o = a - base
    if thumb:
        if not 0 <= o <= len(code) - 4:
            return None
        h1, h2 = struct.unpack_from("<HH", code, o)
        if h1 >> 11 != 0x1E or h2 >> 11 not in (0x1F, 0x1D):
            return None
        off = h1 & 0x7FF
        off -= 0x800 if off & 0x400 else 0
        t = a + 4 + (off << 12) + ((h2 & 0x7FF) << 1)
        return t if h2 >> 11 == 0x1F else t & ~3
    if not 0 <= o <= len(code) - 4:
        return None
    w = struct.unpack_from("<I", code, o)[0]
    if (w >> 25) & 7 != 5:
        return None
    off = w & 0xFFFFFF
    off -= 0x1000000 if off & 0x800000 else 0
    t = a + 8 + off * 4
    if w >> 28 == 0xF:
        t += ((w >> 24) & 1) * 2
    return t


def map_list(src, dst, mp):
    """hle.txt and the like: '<module> <address> ...'; only main addresses move."""
    with open(src) as f, open(dst, "w") as out:
        for line in f:
            p = line.split("#")[0].split()
            if len(p) >= 2 and p[0] == "main":
                a = int(p[1], 16)
                line = line.replace(p[1], f"{mp(a):08x}", 1)
            out.write(line)


def main():
    rom, extract, out = sys.argv[1], sys.argv[2], sys.argv[3]
    code = game_code(rom)
    if code != "YDQP":
        print("usa")
        return
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    usa = os.path.join(root, "config", "usa", "arm9")
    arm9 = open(os.path.join(extract, "arm9", "arm9.bin"), "rb").read()
    mp = Map(arm9)
    for line in open(os.path.join(usa, "symbols.txt")):
        name = line.split(" ", 1)[0]
        m = ADDR_RE.search(line)
        if name in mp.stubs and m:
            mp.secure[int(m.group(1), 16)] = mp.stubs[name]

    dst = os.path.join(out, "arm9")
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    map_symbols(os.path.join(usa, "symbols.txt"), os.path.join(dst, "symbols.txt"), mp.main, True)
    checks = [(arm9, 0x02000000, map_relocs(os.path.join(usa, "relocs.txt"), os.path.join(dst, "relocs.txt"), mp.main, True))]
    import yaml
    for sub in sorted(os.listdir(usa)):
        if not os.path.isdir(os.path.join(usa, sub)):
            continue
        dirs = [sub] if sub != "overlays" else [os.path.join(sub, o) for o in sorted(os.listdir(os.path.join(usa, sub)))]
        for d in dirs:
            os.makedirs(os.path.join(dst, d))
            map_symbols(os.path.join(usa, d, "symbols.txt"), os.path.join(dst, d, "symbols.txt"), mp.main, False)
            rel = map_relocs(os.path.join(usa, d, "relocs.txt"), os.path.join(dst, d, "relocs.txt"), mp.main, False)
            name = os.path.basename(d)
            if name.startswith("ov"):
                ovs = yaml.safe_load(open(os.path.join(extract, "arm9_overlays", "overlays.yaml")))["overlays"]
                ov = next(o for o in ovs if o["id"] == int(name[2:]))
                checks.append((open(os.path.join(extract, "arm9_overlays", ov["file_name"]), "rb").read(), ov["base_address"], rel))
            else:
                y = yaml.safe_load(open(os.path.join(extract, "arm9", f"{name}.yaml")))
                checks.append((open(os.path.join(extract, "arm9", f"{name}.bin"), "rb").read(), y["base_address"], rel))

    # every direct call into the main module must land where the map says
    n = bad = 0
    for data, base, rel in checks:
        for fr, kind, to, mod in rel:
            if mod != "main" or "call" not in kind:
                continue
            t = bl_target(data, base, fr, kind.startswith("thumb"))
            n += 1
            if t is None or (t & ~1) != (to & ~1) and (t & ~3) != (to & ~3):
                bad += 1
                if bad <= 5:
                    print(f"region: call at {fr:08x} goes to {t if t is None else hex(t)}, the map says {to:08x}", file=sys.stderr)
    if bad:
        sys.exit(f"region: {bad} of {n} calls don't match: this ROM isn't the European version this was made for")

    for name in ("hle.txt", "tags.txt", "patches.txt", "interpret.txt", "hooks.txt"):
        map_list(os.path.join(root, name), os.path.join(out, name), mp.main)
    print("eur")


if __name__ == "__main__":
    main()
