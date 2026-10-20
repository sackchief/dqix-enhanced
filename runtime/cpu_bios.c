/* CPU modes, interrupts, CP15 and the DS BIOS (high-level, both CPUs). */
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "hdtext.h"
#include "bios_tables.h"
#define LZ_COST_DEFAULT 0
static void bios_cost(Core* k, uint32_t units, uint32_t per);
static uint32_t lz_cost(void);
#include "dispatch.h"

Cpu g_arm9, g_arm7;
static const Bus g_bus9 = { mem_read8, mem_read16, mem_read32, mem_write8, mem_write16, mem_write32, dispatch_call };
static const Bus g_bus7 = { mem7_read8, mem7_read16, mem7_read32, mem7_write8, mem7_write16, mem7_write32, dispatch7_call };
Core g_c9 = { 0, "ARM9", &g_arm9, &g_bus9, 0, 0, 0, 0, {{0}}, {{0}}, g_io, NULL, 0 };
Core g_c7 = { 1, "ARM7", &g_arm7, &g_bus7, 0, 0, 0, 0, {{0}}, {{0}}, g_io7, NULL, 0 };

uint32_t core_irq_handler_slot(Core* k) { return k->id == 0 ? g_dtcm_base + 0x3FFC : 0x0380FFFC; }
uint32_t core_irq_check_slot(Core* k)   { return k->id == 0 ? g_dtcm_base + 0x3FF8 : 0x0380FFF8; }

/* ---------------------------------------------------------------- modes */
int cpu_mode_index(uint32_t mode) {
    switch (mode & 0x1F) {
    case 0x11: return 1;
    case 0x12: return 2;
    case 0x13: return 3;
    case 0x17: return 4;
    case 0x1B: return 5;
    default: return 0;  /* user / system */
    }
}

void cpu_set_mode(Cpu* c, uint32_t mode) {
    int from = cpu_mode_index(c->cpsr_ctl), to = cpu_mode_index(mode);
    if (from != to) {
        c->bank_r13[from] = c->r[13]; c->bank_r14[from] = c->r[14];
        c->r[13] = c->bank_r13[to];   c->r[14] = c->bank_r14[to];
    }
    c->cpsr_ctl = (c->cpsr_ctl & ~0x1Fu) | (mode & 0x1F);
}

void cpu_write_cpsr_ctl(Cpu* c, uint32_t v, uint32_t mask) {
    uint32_t nv = (c->cpsr_ctl & ~mask) | (v & mask);
    if ((nv & 0x1F) != (c->cpsr_ctl & 0x1F)) cpu_set_mode(c, nv & 0x1F);
    int enabling = (c->cpsr_ctl & 0x80) && !(nv & 0x80);
    c->cpsr_ctl = nv;
    if (enabling) irq_service(c);
}

uint32_t cpu_read_spsr(Cpu* c) { return c->spsr[cpu_mode_index(c->cpsr_ctl)]; }
void cpu_write_spsr(Cpu* c, uint32_t v, uint32_t m) {
    uint32_t* s = &c->spsr[cpu_mode_index(c->cpsr_ctl)];
    *s = (*s & ~m) | (v & m);
}
void cpu_exception_return(Cpu* c, uint32_t addr) {
    (void)addr;
    uint32_t s = cpu_read_spsr(c);
    c->n = s >> 31; c->z = (s >> 30) & 1; c->cf = (s >> 29) & 1; c->v = (s >> 28) & 1; c->q = (s >> 27) & 1;
    cpu_write_cpsr_ctl(c, s, 0x07FFFFFF);
}

/* Call a game function with up to 4 arguments; returns r0, other registers preserved. */
static uint32_t call_game(Core* k, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    Cpu* c = k->cpu;
    uint32_t save[16]; memcpy(save, c->r, sizeof save);
    c->r[0] = a0; c->r[1] = a1; c->r[2] = a2; c->r[3] = a3;
    c->r[14] = 0xFFFF0000u;
    k->bus->call(c, fn, 0xFFFF0000u);
    uint32_t r = c->r[0];
    memcpy(c->r, save, sizeof save);
    return r;
}

/* ---------------------------------------------------------------- IRQs */
void irq_service(Cpu* c) {
    if (!c) return;
    Core* k = core_of(c);
    if (!k->ime || (c->cpsr_ctl & 0x80) || !(k->ie & k->iflags)) return;
    if ((k->id == 1) != (arm7_is_running() != 0)) return;   /* only on the CPU that is running */
    if (k->irq_depth > 8) return;
    k->irq_depth++;
    const Bus* b = k->bus;
    Cpu save = *c;
    uint32_t old_cpsr = CPSR_GET(c);
    cpu_set_mode(c, 0x12);                            /* IRQ mode, IRQs masked */
    c->cpsr_ctl |= 0x80;
    c->spsr[2] = old_cpsr;
    /* BIOS IRQ vector: push r0-r3, r12, lr; call the handler stored by the game */
    uint32_t sp = c->r[13] - 24;
    b->w32(sp, save.r[0]); b->w32(sp + 4, save.r[1]); b->w32(sp + 8, save.r[2]);
    b->w32(sp + 12, save.r[3]); b->w32(sp + 16, save.r[12]); b->w32(sp + 20, save.r[14]);
    c->r[13] = sp;
    c->r[0] = k->id == 0 ? g_dtcm_base + DTCM_SIZE : 0x04000000;
    c->r[14] = 0xFFFF0290u;
    b->call(c, b->r32(core_irq_handler_slot(k)), 0xFFFF0018u);
    /* return: restore the interrupted state (IRQ stack is balanced again) */
    c->r[13] += 24;
    cpu_set_mode(c, save.cpsr_ctl & 0x1F);
    uint32_t b13[6], b14[6], sp6[6];
    memcpy(b13, c->bank_r13, sizeof b13); memcpy(b14, c->bank_r14, sizeof b14); memcpy(sp6, c->spsr, sizeof sp6);
    *c = save;
    memcpy(c->bank_r13, b13, sizeof b13); memcpy(c->bank_r14, b14, sizeof b14); memcpy(c->spsr, sp6, sizeof sp6);
    k->irq_depth--;
    threads_after_irq(c);
}

/* ---------------------------------------------------------------- waiting */
/* Advance until something happens. The ARM9 drives time; the ARM7 hands
 * control back to the ARM9 and is resumed once time has moved on. */
static void wait_step(Core* k) {
    if (k->id == 0) time_advance_lines(1);
    else arm7_yield();
}

void bios_halt(Core* k) {
    for (int i = 0; i < 263 * 60 && !(k->ie & k->iflags); i++) wait_step(k);
    uint32_t ime = k->ime;
    k->ime = 1;
    irq_service(k->cpu);
    k->ime = ime;
}

void bios_intr_wait(Core* k, int discard, uint32_t mask) {
    const Bus* b = k->bus;
    uint32_t flags_addr = core_irq_check_slot(k);
    if (discard) b->w32(flags_addr, b->r32(flags_addr) & ~mask);
    uint32_t saved_ime = k->ime;
    k->ime = 1;   /* the BIOS enables IME while waiting */
    for (int i = 0; ; i++) {
        irq_service(k->cpu);
        uint32_t f = b->r32(flags_addr);
        if (f & mask) { b->w32(flags_addr, f & ~mask); break; }
        if (i == 263 * 120) LOG_WARN("%s IntrWait(mask %08x): nothing for 2 seconds (IE %08x IF %08x)\n", k->name, mask, k->ie, k->iflags);
        wait_step(k);
    }
    k->ime = saved_ime;
}

/* ---------------------------------------------------------------- CP15 (ARM9 only) */
static uint32_t g_cp15_ctl = 0x00052078, g_cp15_dtcm = 0x0080000A, g_cp15_itcm = 0x0000000C;

void cp15_write(Cpu* c, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint32_t v) {
    (void)op1; (void)c;
    if (crn == 1 && crm == 0 && op2 == 0) {
        g_cp15_ctl = v;
        g_dtcm_enabled = (v >> 16) & 1;
        g_itcm_enabled = (v >> 18) & 1;
    } else if (crn == 9 && crm == 1 && op2 == 0) {
        g_cp15_dtcm = v; g_dtcm_base = v & 0xFFFFF000u;
        LOG_INFO("DTCM at %08x\n", g_dtcm_base);
    } else if (crn == 9 && crm == 1 && op2 == 1) {
        g_cp15_itcm = v;
    } else if (crn == 7 && ((crm == 0 && op2 == 4) || (crm == 8 && op2 == 2))) {
        bios_halt(&g_c9);
    }
}

uint32_t cp15_read(Cpu* c, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) {
    (void)c; (void)op1;
    if (crn == 0 && crm == 0 && op2 == 0) return 0x41059461;   /* ARM946E-S */
    if (crn == 0 && crm == 0 && op2 == 1) return 0x0F0D2112;   /* cache type */
    if (crn == 1) return g_cp15_ctl;
    if (crn == 9 && op2 == 0) return g_cp15_dtcm;
    if (crn == 9 && op2 == 1) return g_cp15_itcm;
    return 0;
}

/* ---------------------------------------------------------------- BIOS functions */
static uint16_t crc16(const Bus* b, uint16_t crc, uint32_t p, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        crc ^= b->r8(p + i);
        for (int j = 0; j < 8; j++) { int carry = crc & 1; crc >>= 1; if (carry) crc ^= 0xA001; }  /* CRC-16 (what the BIOS table method computes) */
    }
    return crc;
}

typedef struct { Core* k; uint32_t src, cb; int has_cb; } Reader;
static uint8_t rd8(Reader* r) {
    if (r->has_cb) return (uint8_t)call_game(r->k, r->k->bus->r32(r->cb + 8), r->src++, 0, 0, 0);
    return r->k->bus->r8(r->src++);
}
static uint32_t rd_header(Reader* r, uint32_t dst, uint32_t param) {
    if (r->has_cb) { uint32_t h = call_game(r->k, r->k->bus->r32(r->cb), r->src, dst, param, 0); r->src += 4; return h; }
    uint32_t h = r->k->bus->r32(r->src); r->src += 4; return h;
}

static void put_byte(const Bus* b, uint32_t dst, uint32_t* out, uint8_t v, uint8_t* pending, int write16) {
    if (write16) { if (*out & 1) b->w16(dst + *out - 1, (uint16_t)(*pending | (v << 8))); else *pending = v; }
    else b->w8(dst + *out, v);
    (*out)++;
}

static void lz77(Core* k, int cb, int write16) {
    Cpu* c = k->cpu; const Bus* b = k->bus;
    Reader r = { k, c->r[0], c->r[3], cb };
    uint32_t dst = c->r[1];
    uint32_t len = rd_header(&r, dst, c->r[2]) >> 8, out = 0;
    uint8_t pending = 0;
    static uint8_t window[0x10000];
    while (out < len) {
        uint8_t flags = rd8(&r);
        for (int i = 0; i < 8 && out < len; i++, flags <<= 1) {
            if (flags & 0x80) {
                uint8_t b1 = rd8(&r), b2 = rd8(&r);
                uint32_t n = (b1 >> 4) + 3, disp = (((b1 & 15) << 8) | b2) + 1;
                for (uint32_t j = 0; j < n && out < len; j++) {
                    uint8_t v = window[(out - disp) & 0xFFFF];
                    window[out & 0xFFFF] = v;
                    put_byte(b, dst, &out, v, &pending, write16);
                }
            } else {
                uint8_t v = rd8(&r); window[out & 0xFFFF] = v;
                put_byte(b, dst, &out, v, &pending, write16);
            }
        }
    }
    if (write16 && (out & 1)) b->w16(dst + out - 1, pending);
    bios_cost(k, len, lz_cost());
    if (cb && b->r32(c->r[3] + 4)) call_game(k, b->r32(c->r[3] + 4), 0, 0, 0, 0);
}

static void rle(Core* k, int cb, int write16) {
    Cpu* c = k->cpu; const Bus* b = k->bus;
    Reader r = { k, c->r[0], c->r[3], cb };
    uint32_t dst = c->r[1];
    uint32_t len = rd_header(&r, dst, c->r[2]) >> 8, out = 0;
    uint8_t pending = 0;
    while (out < len) {
        uint8_t f = rd8(&r);
        uint32_t n = (f & 0x80) ? (f & 0x7F) + 3 : (f & 0x7F) + 1;
        uint8_t v = (f & 0x80) ? rd8(&r) : 0;
        for (uint32_t j = 0; j < n && out < len; j++)
            put_byte(b, dst, &out, (f & 0x80) ? v : rd8(&r), &pending, write16);
    }
    if (write16 && (out & 1)) b->w16(dst + out - 1, pending);
    if (cb && b->r32(c->r[3] + 4)) call_game(k, b->r32(c->r[3] + 4), 0, 0, 0, 0);
}

static void bit_unpack(Core* k) {
    Cpu* c = k->cpu; const Bus* b = k->bus;
    uint32_t src = c->r[0], dst = c->r[1], info = c->r[2];
    uint32_t srclen = b->r16(info), sw = b->r8(info + 2), dw = b->r8(info + 3), off = b->r32(info + 4);
    int zero_flag = off >> 31; off &= 0x7FFFFFFF;
    uint32_t acc = 0, accbits = 0;
    for (uint32_t i = 0; i < srclen; i++) {
        uint8_t byte = b->r8(src + i);
        for (uint32_t bit = 0; bit < 8; bit += sw) {
            uint32_t v = (byte >> bit) & ((1u << sw) - 1);
            if (v || zero_flag) v += off;
            acc |= v << accbits; accbits += dw;
            if (accbits >= 32) { b->w32(dst, acc); dst += 4; acc = 0; accbits = 0; }
        }
    }
}

/* BIOS routines run natively here, but take time on the DS: charge their
 * estimated ARM9 cycles per unit of work (calibrated against a reference DS
 * emulator; DQIX_LZCOST overrides the decompression cost for tests). */
static void bios_cost(Core* k, uint32_t units, uint32_t per) {
    if (k->id != 0) return;
    extern uint32_t g_cyc;
    g_cyc += units * per;
}
static uint32_t lz_cost(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("DQIX_LZCOST"); v = e ? atoi(e) : LZ_COST_DEFAULT; }
    return (uint32_t)v;
}

/* Calls common to both BIOSes. Returns 0 if `n` isn't one of them. */
static int bios_common(Core* k, uint32_t n) {
    Cpu* c = k->cpu; const Bus* b = k->bus;
    switch (n) {
    case 0x03:                                                      /* WaitByLoop: time passes, the other CPU runs */
        if (k->id == 1) arm7_yield();
        else { time_spend(c->r[0] * 4); arm7_run(); }
        irq_service(c);
        return 1;
    case 0x04: bios_intr_wait(k, c->r[0] != 0, c->r[1]); return 1;  /* IntrWait */
    case 0x05: bios_intr_wait(k, 1, 1); return 1;                   /* VBlankIntrWait */
    case 0x06: bios_halt(k); return 1;                              /* Halt */
    case 0x09: {                                                    /* Div */
        int32_t a = (int32_t)c->r[0], d = (int32_t)c->r[1];
        if (d == 0) { LOG_WARN("%s BIOS Div by zero\n", k->name); return 1; }
        int32_t q = (a == INT32_MIN && d == -1) ? a : a / d;
        int32_t r = (a == INT32_MIN && d == -1) ? 0 : a % d;
        c->r[0] = (uint32_t)q; c->r[1] = (uint32_t)r; c->r[3] = (uint32_t)(q < 0 ? -q : q);
        return 1;
    }
    case 0x0B: {                                                    /* CpuSet */
        uint32_t s = c->r[0], d = c->r[1], ctl = c->r[2], cnt = ctl & 0x1FFFFF;
        int fill = (ctl >> 24) & 1, w32 = (ctl >> 26) & 1;
        for (uint32_t i = 0; i < cnt; i++) {
            if (w32) { b->w32(d, b->r32(s)); d += 4; if (!fill) s += 4; }
            else { b->w16(d, b->r16(s)); d += 2; if (!fill) s += 2; }
        }
        if (g_hdtext_any && !fill && k->id == 0) hdtext_copy(c->r[1], c->r[0], cnt * (w32 ? 4u : 2u));
        bios_cost(k, cnt, 6);
        return 1;
    }
    case 0x0C: {                                                    /* CpuFastSet */
        uint32_t s = c->r[0], d = c->r[1], ctl = c->r[2];
        uint32_t cnt = ((ctl & 0x1FFFFF) + 7) & ~7u;
        int fill = (ctl >> 24) & 1;
        for (uint32_t i = 0; i < cnt; i++) { b->w32(d, b->r32(s)); d += 4; if (!fill) s += 4; }
        if (g_hdtext_any && !fill && k->id == 0) hdtext_copy(c->r[1], c->r[0], cnt * 4u);
        bios_cost(k, cnt, 2);
        return 1;
    }
    case 0x0D: {                                                    /* Sqrt */
        uint32_t v = c->r[0], r = 0;
        for (uint32_t bit = 1u << 30; bit; bit >>= 2) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; }
        c->r[0] = r;
        return 1;
    }
    case 0x0E: c->r[0] = crc16(b, (uint16_t)c->r[0], c->r[1], c->r[2]); return 1;
    case 0x0F: c->r[0] = 0; return 1;                               /* IsDebugger */
    case 0x10: bit_unpack(k); return 1;
    case 0x11: lz77(k, 0, 0); return 1;
    case 0x12: lz77(k, 1, 1); return 1;
    case 0x14: rle(k, 0, 0); return 1;
    case 0x15: rle(k, 1, 1); return 1;
    default: return 0;
    }
}

void bios_swi(Cpu* c, uint32_t n) {
    { static int lg = -1; static unsigned long long f0, f1;
      if (lg < 0) { const char* e = getenv("DQIX_SWILOG"); lg = e && sscanf(e, "%llu,%llu", &f0, &f1) == 2; }
      if (lg && g_frame >= f0 && g_frame <= f1 && n != 4 && n != 5 && n != 6) fprintf(stderr, "swi f%llu %02x r0 %08x r1 %08x r2 %08x\n", (unsigned long long)g_frame, n, c->r[0], c->r[1], c->r[2]); }
    if (!bios_common(&g_c9, n))
        rt_fatal("ARM9 BIOS call %02x not implemented (r0=%08x r1=%08x r2=%08x)\n", n, c->r[0], c->r[1], c->r[2]);
}

/* ARM7-only calls: sound helper tables, sleep. */
void bios7_swi(Cpu* c, uint32_t n) {
    Core* k = &g_c7;
    if (bios_common(k, n)) return;
    switch (n) {
    case 0x07: bios_halt(k); return;                                /* Sleep */
    case 0x08: return;                                              /* SoundBias */
    case 0x1A: c->r[0] = bios_sine_table[c->r[0] & 0x3F]; return;              /* GetSineTable */
    case 0x1B: c->r[0] = bios_pitch_table[c->r[0] % 768]; return;              /* GetPitchTable */
    case 0x1C: c->r[0] = bios_volume_table[c->r[0] % 724]; return;             /* GetVolumeTable */
    case 0x1F: bios_halt(k); return;                                /* CustomHalt */
    default:
        rt_fatal("ARM7 BIOS call %02x not implemented (r0=%08x r1=%08x r2=%08x)\n", n, c->r[0], c->r[1], c->r[2]);
    }
}

void cpu_breakpoint(Cpu* c, uint32_t a, uint32_t imm) { (void)c; LOG_WARN("bkpt #%u at %08x\n", imm, a); }
void cpu_unhandled(Cpu* c, uint32_t a, uint32_t w) {
    c->fault_addr = a;
    rt_fatal("ARM9: untranslatable instruction %08x at %08x\n", w, a);
}
void cpu7_unhandled(Cpu* c, uint32_t a, uint32_t w) {
    c->fault_addr = a;
    rt_fatal("ARM7: untranslatable instruction %08x at %08x\n", w, a);
}

/* ---------------------------------------------------------------- logging */
int g_log_level = 2;
/* At most LOG_PER_FRAME lines per frame: a burst (the game clearing memory
 * that isn't mapped, thousands of writes) would otherwise print thousands
 * of lines, and printing is slow enough (a Windows console) to stall the game. */
#define LOG_PER_FRAME 32
void rt_log(int level, const char* fmt, ...) {
    if (level > g_log_level) return;
    static uint64_t frame = ~0ull; static unsigned count, dropped; static uint64_t dropped_frame;
    if (g_frame != frame) {
        if (dropped) {
            char note[128];
            snprintf(note, sizeof note, "[f%llu] ... %u more log lines in that frame not shown\n", (unsigned long long)dropped_frame, dropped);
            fputs(note, stderr); report_log_line(note);
        }
        frame = g_frame; count = 0; dropped = 0;
    }
    if (++count > LOG_PER_FRAME) { dropped++; dropped_frame = g_frame; return; }
    char line[512];
    int n = snprintf(line, sizeof line, "[f%llu l%u %s] ", (unsigned long long)g_frame, g_vcount, arm7_is_running() ? "7" : "9");
    va_list ap; va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    fputs(line, stderr);
    report_log_line(line);
}
_Noreturn void rt_fatal(const char* fmt, ...) {
    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "FATAL [frame %llu, %s]: %s", (unsigned long long)g_frame, arm7_is_running() ? "ARM7" : "ARM9", msg);
    trace_dump(stderr, 32);
    backup_flush();
    char reason[600];
    snprintf(reason, sizeof reason, "crash: %s", msg);
    report_write(reason);
    rt_exit(1);
}
