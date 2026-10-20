"""DQIX static recompiler: turns the game's ARM9 code into C.

Usage:
    python recomp/build.py --extract <extract/usa> --config <config/usa/arm9> --out <dir>

Inputs come from the dqix-decomp project:
  * extract/usa      - code images extracted by `dsd rom extract` (from your own ROM)
  * config/usa/arm9  - symbols.txt / relocs.txt with every function's address,
                       size and ARM/Thumb mode, and which overlay each call targets
Output (generated from the game's code, keep private):
  * <out>/funcs.h, <out>/<module>_NN.c, <out>/dispatch_table.c, <out>/report.txt
"""
import argparse
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field

import yaml

sys.path.insert(0, os.path.dirname(__file__))
from insn import (BRANCH, CALL, CALL_REG, EXC_RET, JUMP_REG, JUMPTABLE, RET, SEQ, STOP,  # noqa: E402
                  Decoder, h)

SYM_RE = re.compile(r"^(\S+) kind:(function|label)\((arm|thumb)(?:,size=0x([0-9a-f]+))?[^)]*\) addr:0x([0-9a-f]+)")
REL_RE = re.compile(r"^from:0x([0-9a-f]+) kind:(\S+) to:0x([0-9a-f]+) module:(\S+)")




def insn_cycles(i, thumb):
    """Estimated ARM9 cycles for one instruction (for emulated time; the runtime
    scales it to bus cycles). Data-processing 1; loads 2; stores 1; load/store
    multiple 1 + 1 per register; multiplies 3; taking a branch / call / return
    refills the pipeline: +2."""
    mn, w = i.mn, i.word
    if thumb:
        regs = bin(w & 0xFF).count("1") + ((w >> 8) & 1 if mn.startswith(("push", "pop")) else 0)
    else:
        regs = bin(w & 0xFFFF).count("1")
    if mn.startswith(("ldm", "pop")):
        c = 1 + regs
    elif mn.startswith(("stm", "push")):
        c = 1 + regs
    elif mn.startswith("ldr"):
        c = 2
    elif mn.startswith(("mul", "mla", "umul", "umla", "smul", "smla")):
        c = 3
    else:
        c = 1
    if i.flow in (BRANCH, CALL, CALL_REG, JUMP_REG, RET, EXC_RET, JUMPTABLE):
        c += 2
    return c


class write_if_changed:
    """Like open(path, "w"), but leaves the file (and its timestamp) alone when
    the new content is identical, so make only recompiles what changed."""
    def __init__(self, path):
        self.path = path
        self.parts = []
    def write(self, text):
        self.parts.append(text)
    def __enter__(self):
        return self
    def __exit__(self, *exc):
        if exc[0] is not None:
            return False
        new = "".join(self.parts)
        try:
            with open(self.path) as f:
                if f.read() == new:
                    return False
        except OSError:
            pass
        with open(self.path, "w") as f:
            f.write(new)
        return False

@dataclass
class Func:
    module: "Module"
    name: str
    addr: int
    size: int
    thumb: bool

    @property
    def cname(self):
        return f"fn_{self.module.name}_{self.addr:08x}"


@dataclass
class Module:
    name: str
    ovl_id: int  # -1 for non-overlays
    base: int
    data: bytes
    funcs: dict = field(default_factory=dict)    # addr -> Func
    labels: set = field(default_factory=set)
    sym_addrs: list = field(default_factory=list)  # every symbol address, sorted
    relocs: dict = field(default_factory=dict)   # from -> module spec

    @property
    def end(self):
        return self.base + len(self.data)

    def contains(self, a):
        return self.base <= a < self.end


def load_symbols(mod, path):
    addrs = set()
    with open(path) as f:
        for line in f:
            ma = re.search(r" addr:0x([0-9a-f]+)", line)
            if ma:
                addrs.add(int(ma.group(1), 16))
            m = SYM_RE.match(line.strip())
            if not m:
                continue
            name, kind, mode, size, addr = m.groups()
            addr = int(addr, 16)
            if kind == "function":
                mod.funcs[addr] = Func(mod, name, addr, int(size, 16), mode == "thumb")
            else:
                mod.labels.add(addr)
    mod.sym_addrs = sorted(addrs)
    # Code the symbol map labels as data: static initialisers (called through
    # an overlay's constructor table) and a few mislabeled func_ entries.
    with open(path) as f:
        for line in f:
            m = re.match(r"^((?:func|__sinit)_\S+) kind:data\S* addr:0x([0-9a-f]+)", line.strip())
            if m:
                addr = int(m.group(2), 16)
                if addr not in mod.funcs:
                    nxt = min([a for a in mod.sym_addrs if a > addr] + [mod.base + len(mod.data)])
                    mod.funcs[addr] = Func(mod, m.group(1), addr, nxt - addr, False)


def load_relocs(mod, path):
    if not os.path.exists(path):
        return
    with open(path) as f:
        for line in f:
            m = REL_RE.match(line.strip())
            if m and ("call" in m.group(2) or "branch" in m.group(2)):
                mod.relocs[int(m.group(1), 16)] = m.group(4)


def load_modules(extract, config):
    mods = []
    rd = lambda p: open(os.path.join(extract, p), "rb").read()
    y = lambda p: yaml.safe_load(open(os.path.join(extract, p)))
    main = Module("main", -1, y("arm9/arm9.yaml")["base_address"], rd("arm9/arm9.bin"))
    load_symbols(main, os.path.join(config, "symbols.txt"))
    load_relocs(main, os.path.join(config, "relocs.txt"))
    mods.append(main)
    for name in ("itcm", "dtcm"):
        m = Module(name, -1, y(f"arm9/{name}.yaml")["base_address"], rd(f"arm9/{name}.bin"))
        load_symbols(m, os.path.join(config, name, "symbols.txt"))
        load_relocs(m, os.path.join(config, name, "relocs.txt"))
        mods.append(m)
    for ov in y("arm9_overlays/overlays.yaml")["overlays"]:
        oid = ov["id"]
        m = Module(f"ov{oid:03d}", oid, ov["base_address"], rd(f"arm9_overlays/{ov['file_name']}"))
        load_symbols(m, os.path.join(config, "overlays", m.name, "symbols.txt"))
        load_relocs(m, os.path.join(config, "overlays", m.name, "relocs.txt"))
        mods.append(m)
    return mods


class Recompiler:
    def __init__(self, mods):
        self.mods = mods
        self.by_name = {m.name: m for m in mods}
        self.stats = defaultdict(int)
        self.tags = {}             # (module, call address) -> tag: see --tags
        self.problems = []
        self.pending = []

    # ---------------------------------------------------------- resolution
    def funcs_at(self, target, from_mod, from_addr, thumb_hint=False):
        """Return list of Funcs that a call from (from_mod, from_addr) to target may reach."""
        spec = from_mod.relocs.get(from_addr)
        cands = []
        if spec:
            m = re.match(r"(\w+?)(?:\(([\d,]+)\))?$", spec)
            kind, ids = m.group(1), m.group(2)
            if kind in ("main", "itcm", "dtcm"):
                cands = [self.by_name[kind]]
            elif kind in ("overlay", "overlays") and ids:
                cands = [self.by_name[f"ov{int(x):03d}"] for x in ids.split(",")]
        if not cands:
            if from_mod.ovl_id >= 0 and from_mod.contains(target):
                cands = [from_mod]
            else:
                cands = [m for m in self.mods if m.ovl_id < 0 and m.contains(target)]
                if not cands:
                    cands = [m for m in self.mods if m.ovl_id >= 0 and target in m.funcs]
        found = [m.funcs[target] for m in cands if target in m.funcs]
        if not found:
            # Jump into the middle of a function (shared tail code): make a
            # secondary entry point that starts there.
            for m in cands:
                host = self.containing(m, target)
                if host:
                    f = Func(m, f"{host.name}+{target - host.addr:#x}", target,
                             host.addr + host.size - target, host.thumb)
                    m.funcs[target] = f
                    self.pending.append(f)
                    self.stats["secondary_entries"] += 1
                    found.append(f)
            if not found and len(cands) == 1 and cands[0].contains(target):
                # Called code the symbol map labels as data: a function that
                # runs until the next symbol.
                m = cands[0]
                nxt = min([a for a in m.sym_addrs if a > target] + [m.end])
                f = Func(m, f"func_{m.name}_{target:08x}", target, nxt - target, bool(thumb_hint))
                m.funcs[target] = f
                self.pending.append(f)
                self.stats["data_promoted_to_function"] += 1
                found.append(f)
        return found

    def containing(self, m, a):
        for fa in m.funcs:
            f = m.funcs[fa]
            if f.addr < a < f.addr + f.size:
                return f
        return None

    def call_stmt(self, mod, from_addr, target, thumb):
        fs = self.funcs_at(target, mod, from_addr, thumb)
        if len(fs) == 1:
            if fs[0].thumb != thumb:
                self.stats["mode_mismatch"] += 1
            return f"{fs[0].cname}(c);"
        if len(fs) > 1:
            self.stats["multi_overlay_calls"] += 1
            parts = [f"if (overlay_is_loaded({f.module.ovl_id})) {f.cname}(c);" for f in fs]
            parts.append(f"dispatch_call(c, {h(target | thumb)}, {h(from_addr)});")
            return "{ " + " else ".join(parts) + " }"
        self.stats["unresolved_calls"] += 1
        self.problems.append(f"{mod.name} {from_addr:08x}: call to {target:08x} has no known function")
        return f"dispatch_call(c, {h(target | thumb)}, {h(from_addr)});"

    # ---------------------------------------------------------- functions
    def discover(self, func, dec):
        start, end = func.addr, func.addr + func.size
        thumb = func.thumb
        insns = {}
        data = set()
        computed = []
        labels = {start}
        extra_labels = {a for a in func.module.labels if start <= a < end}
        labels |= extra_labels
        work = [start] + sorted(extra_labels)
        while work:
            a = work.pop()
            prev = None
            while start <= a < end and a not in insns and a not in data:
                i = dec.thumb(a) if thumb else dec.arm(a)
                insns[a] = i
                after_lr_pc = prev is not None and prev.sets_lr_pc
                prev = i
                for ea, sz in i.pool_refs:
                    for k in range(sz):
                        data.add(ea + k)
                uncond = i.cond == ""
                if i.flow == BRANCH:
                    if start <= i.target < end:
                        labels.add(i.target)
                        work.append(i.target)
                    if uncond:
                        break
                elif i.flow == JUMPTABLE and not thumb and self.table_size(insns, a, i) is not None:
                    # size from the bounds check `cmp rI, #N` before `addls pc, pc, rI, lsl #2`;
                    # entries are single instructions (usually b, sometimes a return)
                    n = self.table_size(insns, a, i)
                    i.table = [a + 8 + 4 * j for j in range(n)]
                    for t in i.table:
                        if start <= t < end:
                            labels.add(t)
                            work.append(t)
                    if uncond:
                        break
                elif i.flow == JUMPTABLE:
                    k = 0
                    while True:
                        ea = a + 8 + 4 * k
                        if not (start <= ea < end):
                            break
                        e = dec.arm(ea)
                        if e.flow != BRANCH or e.cond != "":
                            break
                        labels.add(ea)
                        work.append(ea)
                        k += 1
                    i.table = [a + 8 + 4 * j for j in range(k)]
                    if k == 0:
                        # computed jump into straight-line code (e.g. an unrolled
                        # division loop): every following instruction is a target
                        i.computed = True
                        computed.append(a)
                        work.append(a + 8)
                        if uncond:
                            break
                    elif uncond:
                        break
                elif i.table_words or i.adr >= 0:
                    # A table of code addresses: `ldr pc, [pc, rX, lsl #2]`, or
                    # `adr rT, table` followed by a load through rT.
                    tbl = a + 8 if i.table_words else i.adr
                    entries = []
                    ea = tbl
                    while start <= ea < end and ea % 4 == 0 and ea not in insns:
                        t = dec.word(ea)
                        tt = t & ~1 if thumb else t
                        if not (start <= tt < end) or (bool(t & 1) != thumb and t & 3):
                            break
                        entries.append(tt)
                        ea += 4
                    if entries:
                        self.stats["address_tables"] += 1
                        for k in range(tbl, ea):
                            data.add(k)
                        for t in entries:
                            labels.add(t)
                            work.append(t)
                    if i.table_words and uncond:
                        break
                elif i.flow == JUMP_REG and thumb and i.mn == "add" and (i.word & 0xFF87) == 0x4487:
                    # Thumb switch: cmp rI, #N ; ... ldrb/ldrh rX, [..] ; (lsls/asrs) ; add pc, rX
                    # Table of N+1 byte/halfword offsets right after the add.
                    tbl = self.thumb_offset_table(insns, a, dec)
                    if tbl:
                        n, esize, scale, signed = tbl
                        self.stats["thumb_offset_tables"] += 1
                        tstart = a + 2
                        for k in range(tstart, tstart + n * esize):
                            data.add(k)
                        for k in range(n):
                            ea = tstart + k * esize
                            v = dec.byte(ea) if esize == 1 else dec.half(ea)
                            if signed:
                                v -= (1 << (8 * esize)) if v >> (8 * esize - 1) else 0
                            t = a + 4 + v * scale
                            if start <= t < end:
                                labels.add(t)
                                work.append(t)
                    break
                elif i.flow == JUMP_REG and after_lr_pc:
                    pass  # `mov lr, pc; bx rX` is a call: execution continues after it
                elif i.flow in (RET, JUMP_REG, EXC_RET, STOP) and uncond:
                    break
                a += i.size
        # drop anything decoded from literal pools
        for a in [a for a in insns if a in data]:
            del insns[a]
        for a in computed:
            t = a + 8
            targets = []
            while t in insns:
                targets.append(t)
                t += insns[t].size
            labels.update(targets)
            insns[a].table = targets
        return insns, labels

    @staticmethod
    def table_size(insns, a, i):
        """Entries of an ARM `add<cc> pc, pc, rI, lsl #2` table: the largest bound
        compared against rI just before it (`cmp rI, #N` + bhi/bgt, or addls)."""
        if not i.cond:
            return None
        idx = i.word & 15
        best = None
        for k in range(1, 7):
            p = insns.get(a - 4 * k)
            if p is None:
                break
            w = p.word
            if (w & 0x0FF00000) == 0x03500000 and ((w >> 16) & 15) == idx:   # cmp rI, #imm
                rot = ((w >> 8) & 15) * 2
                imm = w & 0xFF
                imm = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
                if imm < 4096 and (best is None or imm > best):
                    best = imm
                continue
            if p.flow == "seq" and ((w >> 12) & 15) == idx and p.mn not in ("cmp", "cmn", "tst", "teq", "str", "strb", "strh"):
                break   # index register written: earlier compares don't apply
        return best + 1 if best is not None else None

    @staticmethod
    def thumb_offset_table(insns, a, dec):
        """Recognise the instructions before a Thumb `add pc, rX` switch.
        Returns (entries, entry_size, scale, signed) or None."""
        n = esize = None
        scale, signed = 1, False
        for k in range(1, 9):
            p = insns.get(a - 2 * k)
            if p is None:
                break
            w = p.word & 0xFFFF
            if esize is None:
                if (w >> 11) == 0x0F:        # ldrb rd, [rb, #imm]
                    esize = 1
                elif (w >> 11) == 0x11:      # ldrh rd, [rb, #imm]
                    esize = 2
                elif (w >> 11) == 0 and (w >> 6) & 31:   # lsls #s (after the load)
                    scale <<= (w >> 6) & 31
                elif (w >> 11) == 2:          # asrs: sign extension of the loaded value
                    signed = True
            elif (w >> 11) == 5:              # cmp rI, #N
                n = (w & 0xFF) + 1
                break
        if n is None or esize is None:
            return None
        # `lsls #16 ; asrs #16` sign-extends a halfword: not a scale
        if esize == 2 and scale == 1 << 16:
            scale, signed = 1, True
        return n, esize, scale, signed

    def emit_func(self, func):
        mod = func.module
        dec = Decoder(mod.base, mod.data)
        insns, labels = self.discover(func, dec)
        order = sorted(insns)
        has_jump_reg = any(insns[a].flow == JUMP_REG for a in order)
        # Only a function that explicitly loads lr with an address (not a call,
        # not a register pop) can make a tail-callee "return" somewhere else.
        lr_loaded = any(insns[a].flow == SEQ and "c->r[14] = " in (insns[a].body or "")
                        and not insns[a].mn.startswith(("ldm", "pop", "stm", "push"))
                        and "c->r[13]" not in (insns[a].body or "")
                        for a in order)
        out = [f"/* {func.name} ({'thumb' if func.thumb else 'arm'}, {func.size:#x} bytes) */",
               f"void {func.cname}(Cpu* c) {{",
               f"    RECOMP_ENTER({h(func.addr | func.thumb)});",
               "    const uint32_t entry_lr = c->r[14]; (void)entry_lr;"]
        start, end = func.addr, func.addr + func.size
        tbit = 1 if func.thumb else 0

        def branch(target, from_addr):
            if start <= target < end and target in insns:
                if target <= from_addr:
                    return f"RECOMP_LOOP(); goto L_{target:08x};"
                return f"goto L_{target:08x};"
            if start <= target < end:
                self.stats["branch_into_data"] += 1
            # Tail call. If this function changed lr before jumping (e.g. the
            # SDK IRQ dispatcher sets lr = OS_IrqHandler_ThreadSwitch), the
            # callee "returns" to that address: continue there.
            if not lr_loaded:
                return "{ " + self.call_stmt(mod, from_addr, target, func.thumb) + " return; }"
            return ("{ uint32_t tl = c->r[14]; " + self.call_stmt(mod, from_addr, target, func.thumb)
                    + f" if (tl != entry_lr) dispatch_call(c, tl, {h(from_addr)}); return; }}")

        def local_switch(expr, from_addr):
            cases = " ".join(f"case {h(t)}: goto L_{t:08x};" for t in sorted(labels) if t in insns)
            if not lr_loaded:
                return f"switch (({expr}) & ~1u) {{ {cases} default: {{ dispatch_call(c, {expr}, {h(from_addr)}); return; }} }}"
            return (f"switch (({expr}) & ~1u) {{ {cases} default: {{ uint32_t tl = c->r[14]; dispatch_call(c, {expr}, {h(from_addr)}); "
                    f"if (tl != entry_lr) dispatch_call(c, tl, {h(from_addr)}); return; }} }}")

        # cycle cost of each straight-line run (from a label or the entry to the next label)
        run_cost = {}
        cur = None
        for a in order:
            if cur is None or a in labels:
                cur = a
                run_cost[cur] = 0
            run_cost[cur] += insn_cycles(insns[a], func.thumb)
        if order and order[0] not in labels:
            out.append(f"    RECOMP_CYC({run_cost[order[0]]});")
        prev_sets_lr = False
        for idx, a in enumerate(order):
            i = insns[a]
            self.stats["insns"] += 1
            if a in labels:
                out.append(f"L_{a:08x}:; RECOMP_CYC({run_cost[a]});")
            ret_addr = (a + i.size) | tbit
            if i.flow == SEQ:
                s = i.body or ";"
                if i.mn == "udf" or "cpu_unhandled" in s:
                    self.stats["unhandled_insns"] += 1
            elif i.flow == BRANCH:
                s = branch(i.target, a)
            elif i.flow == CALL:
                s = f"c->r[14] = {h(ret_addr)}; " + self.call_stmt(mod, a, i.target, i.target_thumb)
                tag = self.tags.get((mod.name, a))
                if tag:                # what this call draws carries the tag (runtime: g_gx_tag)
                    s = f"{{ int gt = gx_tag_begin({tag}); {s} gx_tag_end(gt); }}"
            elif i.flow == RET:
                s = (i.body + " " if i.body else "") + "return;"
            elif i.flow == EXC_RET:
                s = i.body + " return;"
            elif i.flow == STOP:
                s = i.body
                self.stats["unhandled_insns"] += 1
            elif i.flow == CALL_REG:
                self.stats["indirect_calls"] += 1
                s = f"{{ {i.body} c->r[14] = {h(ret_addr)}; dispatch_call(c, tgt, {h(a)}); }}"
            elif i.flow == JUMP_REG:
                if prev_sets_lr:
                    self.stats["indirect_calls"] += 1
                    s = f"{{ {i.body} dispatch_call(c, tgt, {h(a)}); }}"
                else:
                    self.stats["indirect_jumps"] += 1
                    s = f"{{ {i.body} {local_switch('tgt', a)} }}"
            elif i.flow == JUMPTABLE:
                self.stats["jump_tables"] += 1
                table = getattr(i, "table", [])
                cases = " ".join(f"case {(t - a - 8) // 4}: {branch(t, a)}" for t in table)
                s = (f"switch ({i.index}) {{ {cases} default: cpu_unhandled(c, {h(a)}, {h(i.word)}); return; }}")
            else:
                raise AssertionError(i.flow)
            if i.cond:
                s = f"if (COND_{i.cond}) {{ {s} }}"
            out.append(f"    /* {a:08x} {i.mn} */ {s}")
            # Does execution continue at a + size?
            if i.cond:
                falls = True
            elif i.flow in (SEQ, CALL, CALL_REG):
                falls = True
            elif i.flow == JUMP_REG:
                falls = prev_sets_lr  # `mov lr, pc; bx rX` is a call
            else:
                falls = False
            prev_sets_lr = i.sets_lr_pc
            if falls:
                nxt = a + i.size
                if idx + 1 >= len(order) or order[idx + 1] != nxt:
                    out.append(f"    {branch(nxt, a)}")
        out.append("    return;")
        out.append("}")
        self.stats["functions"] += 1
        return "\n".join(out)

    def all_funcs(self):
        for m in self.mods:
            for a in sorted(m.funcs):
                yield m.funcs[a]


PRELUDE = '#include "cpu.h"\n#include "funcs.h"\n\n'


def apply_patches(mods, path):
    """Override instruction words in the code we translate. The ROM itself is
    untouched (the runtime still compares overlays against the original)."""
    by_name = {m.name: m for m in mods}
    for line in open(path):
        line = line.split("#")[0].split()
        if len(line) != 3:
            continue
        m = by_name[line[0]]
        addr, word = int(line[1], 16), int(line[2], 16)
        off = addr - m.base
        old = int.from_bytes(m.data[off:off + 4], "little")
        data = bytearray(m.data)
        data[off:off + 4] = word.to_bytes(4, "little")
        m.data = bytes(data)
        print(f"patch {m.name} {addr:08x}: {old:08x} -> {word:08x}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extract", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--per-file", type=int, default=400)
    ap.add_argument("--hle", help="file listing '<module> <addr> [name]' functions the runtime provides instead (as hle_<name>, or under the generated name)")
    ap.add_argument("--interpret", help="file listing '<module> <addr>' functions to run in the interpreter (code that changes at run time)")
    ap.add_argument("--patch", help="file listing '<module> <addr> <new 32-bit ARM word>' instruction overrides (e.g. removing anti-piracy traps)")
    ap.add_argument("--hooks", help="file listing '<module> <addr> <name>' functions the runtime watches: they are kept, renamed <name>_orig, and calls go to hook_<name>(c, orig), which runs them")
    ap.add_argument("--tags", help="file listing '<module> <call addr> <tag>': the 3D drawn during these calls carries the tag (e.g. the stylus arrow, hidden while walking with a stick); the game's code is unchanged")
    args = ap.parse_args()

    mods = load_modules(args.extract, args.config)
    if args.patch:
        apply_patches(mods, args.patch)
    rc = Recompiler(mods)
    if args.tags:
        for line in open(args.tags):
            line = line.split("#")[0].split()
            if len(line) == 3:
                rc.tags[(line[0], int(line[1], 16))] = int(line[2], 0)
    interp = set()
    if args.interpret:
        for line in open(args.interpret):
            line = line.split("#")[0].split()
            if len(line) == 2:
                interp.add((line[0], int(line[1], 16)))
    hooks = {}
    if args.hooks:
        for line in open(args.hooks):
            line = line.split("#")[0].split()
            if len(line) == 3:
                hooks[(line[0], int(line[1], 16))] = line[2]
    hle = {}
    if args.hle:
        for line in open(args.hle):
            line = line.split("#")[0].split()
            if len(line) in (2, 3):
                hle[(line[0], int(line[1], 16))] = line[2] if len(line) == 3 else None
    os.makedirs(args.out, exist_ok=True)

    # Translate everything in memory first: translating can discover secondary
    # entry points, which then need translating too.
    code = {}
    queue = list(rc.all_funcs())
    while queue:
        for fn in queue:
            if (fn.module.name, fn.addr) in interp:
                code[(fn.module.name, fn.addr)] = (f"/* {fn.name}: encrypted in the ROM, decrypted at run time: interpreted */\n"
                                                   f"void interp_stub(Cpu* c, uint32_t addr);\n"
                                                   f"void {fn.cname}(Cpu* c) {{ interp_stub(c, {h(fn.addr | fn.thumb)}); }}")
                rc.stats["interpreted_functions"] += 1
                fn.interpreted = True
                continue
            if (fn.module.name, fn.addr) in hle:
                name = hle[(fn.module.name, fn.addr)]
                if name:      # the runtime's hle_<name>, whatever the address is in this ROM
                    code[(fn.module.name, fn.addr)] = (f"/* {fn.name}: provided by the runtime (HLE) */\n"
                                                       f"void hle_{name}(Cpu* c);\n"
                                                       f"void {fn.cname}(Cpu* c) {{ hle_{name}(c); }}")
                else:
                    code[(fn.module.name, fn.addr)] = f"/* {fn.cname}: provided by the runtime (HLE) */"
                rc.stats["hle_functions"] += 1
                continue
            code[(fn.module.name, fn.addr)] = rc.emit_func(fn)
            if (fn.module.name, fn.addr) in hooks:      # the original under another name, and the runtime's hook in front of it
                name = hooks[(fn.module.name, fn.addr)]
                head = f"void {fn.cname}(Cpu* c) {{"
                assert head in code[(fn.module.name, fn.addr)], fn.cname
                code[(fn.module.name, fn.addr)] = (code[(fn.module.name, fn.addr)].replace(head, f"void {fn.cname}_orig(Cpu* c) {{", 1) +
                                                   f"\n/* {fn.name}: watched by the runtime (hook) */\n"
                                                   f"void hook_{name}(Cpu* c, void (*orig)(Cpu*));\n"
                                                   f"void {fn.cname}(Cpu* c) {{ hook_{name}(c, {fn.cname}_orig); }}")
                rc.stats["hooked_functions"] = rc.stats.get("hooked_functions", 0) + 1
        queue, rc.pending = rc.pending, []

    funcs = list(rc.all_funcs())
    with write_if_changed(os.path.join(args.out, "funcs.h")) as f:
        f.write("#pragma once\n#include \"cpu.h\"\n")
        for fn in funcs:
            f.write(f"void {fn.cname}(Cpu* c);\n")

    files = []
    for m in mods:
        mf = [m.funcs[a] for a in sorted(m.funcs)]
        for k in range(0, len(mf), args.per_file):
            path = os.path.join(args.out, f"{m.name}_{k // args.per_file:02d}.c")
            with write_if_changed(path) as f:
                f.write(PRELUDE)
                for fn in mf[k:k + args.per_file]:
                    f.write(code[(m.name, fn.addr)])
                    f.write("\n\n")
            files.append(path)

    with write_if_changed(os.path.join(args.out, "dispatch_table.c")) as f:
        f.write('#include "funcs.h"\n#include "dispatch.h"\n\nconst RecompFunc g_recomp_funcs[] = {\n')
        for fn in funcs:
            size = 0 if getattr(fn, "interpreted", False) else fn.size
            f.write(f"    {{ {fn.module.ovl_id}, {h(fn.addr | fn.thumb)}, {size}, {fn.cname}, \"{fn.name}\" }},\n")
        f.write("};\nconst unsigned g_recomp_func_count = sizeof(g_recomp_funcs) / sizeof(g_recomp_funcs[0]);\n")

    with write_if_changed(os.path.join(args.out, "report.txt")) as f:
        for k in sorted(rc.stats):
            f.write(f"{k}: {rc.stats[k]}\n")
        f.write(f"\nproblems ({len(rc.problems)}):\n")
        f.write("\n".join(rc.problems[:2000]))
    for k in sorted(rc.stats):
        print(f"{k:24s} {rc.stats[k]}")
    print(f"{len(files)} C files written to {args.out}")


if __name__ == "__main__":
    main()
