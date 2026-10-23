"""End-to-end semantic test of the recompiler.

cases.c -> clang (ARMv5TE, ARM and Thumb) -> machine code -> our recompiler -> C
cases.c -> native compiler
Both run on the same random inputs; every result must match.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(ROOT, "recomp"))
from build import Func, Module, Recompiler  # noqa: E402

BASE = 0x02000000
LDS = """
SECTIONS {
  . = 0x02000000;
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
}
"""

HARNESS_HEAD = r"""
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "dispatch.h"
#include "funcs.h"

#define RAM_SIZE (8u << 20)
static uint8_t ram[RAM_SIZE];
uint32_t g_cyc;  /* generated recompiler code increments its cycle counter */
static uint8_t* P(uint32_t a) {
    uint32_t o = a - 0x02000000u;
    if (o >= RAM_SIZE) { fprintf(stderr, "bad address %08x\n", a); exit(3); }
    return &ram[o];
}
uint8_t  mem_read8(uint32_t a) { return *P(a); }
uint16_t mem_read16(uint32_t a) { uint16_t v; memcpy(&v, P(a & ~1u), 2); return v; }
uint32_t mem_read32(uint32_t a) { uint32_t v; memcpy(&v, P(a & ~3u), 4); return v; }
void mem_write8(uint32_t a, uint8_t v) { *P(a) = v; }
void mem_write16(uint32_t a, uint16_t v) { memcpy(P(a & ~1u), &v, 2); }
void mem_write32(uint32_t a, uint32_t v) { memcpy(P(a & ~3u), &v, 4); }
void cpu_write_cpsr_ctl(Cpu* c, uint32_t v, uint32_t m) { c->cpsr_ctl = (c->cpsr_ctl & ~m) | (v & m); }
void cpu_write_spsr(Cpu* c, uint32_t v, uint32_t m) { (void)c; (void)v; (void)m; }
uint32_t cpu_read_spsr(Cpu* c) { (void)c; return 0; }
void cp15_write(Cpu* c, uint32_t a, uint32_t b, uint32_t d, uint32_t e, uint32_t v) { (void)c;(void)a;(void)b;(void)d;(void)e;(void)v; }
uint32_t cp15_read(Cpu* c, uint32_t a, uint32_t b, uint32_t d, uint32_t e) { (void)c;(void)a;(void)b;(void)d;(void)e; return 0; }
void bios_swi(Cpu* c, uint32_t n) { (void)c; fprintf(stderr, "swi %u\n", n); exit(4); }
void cpu_breakpoint(Cpu* c, uint32_t a, uint32_t i) { (void)c; fprintf(stderr, "bkpt %08x %u\n", a, i); exit(4); }
void cpu_unhandled(Cpu* c, uint32_t a, uint32_t w) { (void)c; fprintf(stderr, "unhandled %08x %08x\n", a, w); exit(4); }
void cpu_exception_return(Cpu* c, uint32_t a) { (void)c; (void)a; exit(4); }
int overlay_is_loaded(int id) { (void)id; return 0; }
uint32_t g_call_budget = 1u << 30, g_loop_budget = 1u << 30;
void recomp_budget_expired(Cpu* c) { (void)c; g_call_budget = 1u << 30; }
void recomp_loop_expired(Cpu* c) { (void)c; g_loop_budget = 1u << 30; }
void dispatch_call(Cpu* c, uint32_t t, uint32_t from) {
    for (unsigned i = 0; i < g_recomp_func_count; i++)
        if ((g_recomp_funcs[i].addr & ~1u) == (t & ~1u)) { g_recomp_funcs[i].fn(c); return; }
    fprintf(stderr, "dispatch miss %08x from %08x\n", t, from); exit(5);
}

"""
HARNESS_TESTS = r"""typedef uint32_t (*native_fn)(uint32_t, uint32_t, uint32_t, uint32_t);
@DECLS@
static const struct { const char* name; native_fn native; void (*recomp)(Cpu*); } tests[] = {
@TESTS@
};

"""
HARNESS_RNG = r"""static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    static const uint32_t edge[] = { 0, 1, 2, 31, 32, 33, 0x7fffffff, 0x80000000u, 0xffffffffu, 0xfffffffeu, 0x8000, 0xffff, 0x10000 };
    uint32_t k = (uint32_t)(rng >> 40) % 8;
    if (k == 0) return edge[(rng >> 20) % (sizeof edge / sizeof edge[0])];
    if (k == 1) return (uint32_t)(rng >> 5) & 0xff;
    return (uint32_t)(rng >> 16);
}

"""
HARNESS_MAIN = r"""int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb");
    fread(ram, 1, RAM_SIZE, f); fclose(f);
    int iters = argc > 2 ? atoi(argv[2]) : 20000;
    long checked = 0, failed = 0;
    for (unsigned t = 0; t < sizeof tests / sizeof tests[0]; t++) {
        int bad = 0;
        for (int n = 0; n < iters; n++) {
            uint32_t a = rnd(), b = rnd(), cc = rnd(), d = rnd();
            Cpu c; memset(&c, 0, sizeof c);
            c.r[0] = a; c.r[1] = b; c.r[2] = cc; c.r[3] = d;
            c.r[13] = 0x02780000u; c.r[14] = 0xFFFFFFF0u;
            tests[t].recomp(&c);
            uint32_t want = tests[t].native(a, b, cc, d);
            checked++;
            if (c.r[0] != want || c.r[13] != 0x02780000u) {
                failed++;
                if (bad++ < 3) printf("FAIL %s(%08x,%08x,%08x,%08x): recomp %08x native %08x sp %08x\n",
                                      tests[t].name, a, b, cc, d, c.r[0], want, c.r[13]);
            }
        }
        printf("%-20s %s\n", tests[t].name, bad ? "FAIL" : "ok");
    }
    printf("%ld checks, %ld failures\n", checked, failed);
    return failed != 0;
}
"""
HARNESS = HARNESS_HEAD + HARNESS_TESTS + HARNESS_RNG + HARNESS_MAIN



INTERP_SHIM = r"""
const RecompFunc g_recomp_funcs[1];
const unsigned g_recomp_func_count = 0;
Cpu g_arm9, g_arm7;
static const Bus g_bus = { mem_read8, mem_read16, mem_read32, mem_write8, mem_write16, mem_write32, 0 };
Core g_c9 = { 0, "ARM9", &g_arm9, &g_bus };
Core g_c7 = { 1, "ARM7", &g_arm7, &g_bus };
int g_log_level = 0;
void rt_log(int l, const char* f, ...) { (void)l; (void)f; }
_Noreturn void rt_fatal(const char* f, ...) { fprintf(stderr, "fatal: %s\n", f); exit(6); }
const RecompFunc* dispatch_find(Core* k, uint32_t a) { (void)k; (void)a; return NULL; }
void bios7_swi(Cpu* c, uint32_t n) { bios_swi(c, n); }
"""
INTERP_MAIN = r"""
void interp_call(Core* k, uint32_t target);
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb");
    fread(ram, 1, RAM_SIZE, f); fclose(f);
    int iters = argc > 2 ? atoi(argv[2]) : 5000;
    long checked = 0, failed = 0;
    for (unsigned t = 0; t < sizeof tests / sizeof tests[0]; t++) {
        int bad = 0;
        for (int n = 0; n < iters; n++) {
            uint32_t a = rnd(), b = rnd(), cc = rnd(), d = rnd();
            memset(&g_arm9, 0, sizeof g_arm9);
            g_arm9.r[0] = a; g_arm9.r[1] = b; g_arm9.r[2] = cc; g_arm9.r[3] = d;
            g_arm9.r[13] = 0x02780000u; g_arm9.r[14] = 0xFFFFFFF0u;
            interp_call(&g_c9, tests[t].addr);
            uint32_t want = tests[t].native(a, b, cc, d);
            checked++;
            if (g_arm9.r[0] != want || g_arm9.r[13] != 0x02780000u) {
                failed++;
                if (bad++ < 3) printf("FAIL %s(%08x,%08x,%08x,%08x): interp %08x native %08x\n", tests[t].name, a, b, cc, d, g_arm9.r[0], want);
            }
        }
        printf("%-20s %s\n", tests[t].name, bad ? "FAIL" : "ok");
    }
    printf("%ld checks, %ld failures\n", checked, failed);
    return failed != 0;
}
"""


def sh(*cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        print(" ".join(cmd), r.stdout, r.stderr, sep="\n")
        raise SystemExit(1)
    return r.stdout


def run_mode(mode, iters):
    cases = os.path.join(HERE, "cases.c")
    d = tempfile.mkdtemp(prefix=f"sem_{mode}_")
    flags = ["--target=armv5te-none-eabi", "-O2", "-ffreestanding", "-fno-unwind-tables",
             "-mfloat-abi=soft", "-fno-asynchronous-unwind-tables"]
    if mode == "thumb":
        flags.append("-mthumb")
    sh("clang", *flags, "-c", cases, "-o", f"{d}/cases.o")
    open(f"{d}/link.ld", "w").write(LDS)
    sh("ld.lld", "-T", f"{d}/link.ld", f"{d}/cases.o", "-o", f"{d}/cases.elf")
    sh("llvm-objcopy", "-O", "binary", f"{d}/cases.elf", f"{d}/image.bin")
    image = open(f"{d}/image.bin", "rb").read()

    mod = Module("main", -1, BASE, image)
    names = {}
    # readelf keeps bit 0 of Thumb function addresses (nm strips it)
    for line in sh("llvm-readelf", "-s", "-W", f"{d}/cases.elf").splitlines():
        p = line.split()
        if len(p) >= 8 and p[3] == "FUNC" and p[6] != "UND":
            value, size = int(p[1], 16), int(p[2])
            thumb = bool(value & 1)
            addr = value & ~1
            mod.funcs[addr] = Func(mod, p[7], addr, size, thumb)
            names[p[7]] = addr
    mod.sym_addrs = sorted(mod.funcs)
    rc = Recompiler([mod])
    gen = f"{d}/gen"
    os.makedirs(gen)
    code = []
    queue = list(rc.all_funcs())
    while queue:
        code += [rc.emit_func(f) for f in queue]
        queue, rc.pending = rc.pending, []
    funcs = list(rc.all_funcs())
    with open(f"{gen}/funcs.h", "w") as f:
        f.write('#pragma once\n#include "cpu.h"\n' + "".join(f"void {x.cname}(Cpu* c);\n" for x in funcs))
    with open(f"{gen}/code.c", "w") as f:
        f.write('#include "cpu.h"\n#include "funcs.h"\n\n' + "\n\n".join(code))
    with open(f"{gen}/table.c", "w") as f:
        f.write('#include "funcs.h"\n#include "dispatch.h"\nconst RecompFunc g_recomp_funcs[] = {\n')
        f.write("".join(f"  {{-1, {x.addr | x.thumb}u, {x.size}, {x.cname}, \"{x.name}\"}},\n" for x in funcs))
        f.write("};\nconst unsigned g_recomp_func_count = sizeof g_recomp_funcs / sizeof g_recomp_funcs[0];\n")
    tests = sorted(n for n in names if n.startswith("t_") and n != "t_many_args_callee")
    decls = "\n".join(f"uint32_t {t}(uint32_t, uint32_t, uint32_t, uint32_t);" for t in tests)
    rows = "\n".join(f'  {{ "{t}", {t}, fn_main_{names[t]:08x} }},' for t in tests)
    open(f"{gen}/harness.c", "w").write(HARNESS.replace("@DECLS@", decls).replace("@TESTS@", rows))
    rt = os.path.join(ROOT, "runtime")
    sh("gcc", "-O1", "-w", "-o", f"{d}/test", f"-I{rt}", f"-I{gen}", f"{gen}/harness.c", f"{gen}/code.c",
       f"{gen}/table.c", cases)
    r = subprocess.run([f"{d}/test", f"{d}/image.bin", str(iters)], capture_output=True, text=True)
    # Same tests through the interpreter (runtime/interp.c), all code interpreted
    rows_i = "\n".join(f'  {{ "{t}", {t}, {names[t] | (1 if mode == "thumb" else 0)}u }},' for t in tests)
    head = (HARNESS_HEAD.replace('#include "funcs.h"', '#include "nds.h"')
            .replace("void dispatch_call(Cpu* c, uint32_t t, uint32_t from) {", "void dispatch_call_unused(Cpu* c, uint32_t t, uint32_t from) {"))
    hi = (head + INTERP_SHIM + "typedef uint32_t (*native_fn)(uint32_t, uint32_t, uint32_t, uint32_t);\n" + decls
          + "\nstatic const struct { const char* name; native_fn native; uint32_t addr; } tests[] = {\n" + rows_i + "\n};\n"
          + HARNESS_RNG + INTERP_MAIN)
    open(f"{gen}/harness_interp.c", "w").write(hi)
    sh("gcc", "-O1", "-w", "-o", f"{d}/test_interp", f"-I{rt}", f"-I{gen}", f"{gen}/harness_interp.c",
       os.path.join(rt, "interp.c"), cases, "-lm")
    r2 = subprocess.run([f"{d}/test_interp", f"{d}/image.bin", str(iters // 4)], capture_output=True, text=True)
    print(f"--- {mode} through the interpreter")
    print(r2.stdout + r2.stderr)
    if r2.returncode:
        return False
    print(f"=== {mode}: {len(funcs)} functions recompiled ({rc.stats['insns']} instructions)")
    print(r.stdout + r.stderr)
    return r.returncode == 0


if __name__ == "__main__":
    iters = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
    ok = all([run_mode("arm", iters), run_mode("thumb", iters)])
    sys.exit(0 if ok else 1)
