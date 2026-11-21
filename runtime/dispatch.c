/* Function lookup for indirect calls (both CPUs), overlay detection, the call
 * budget, and tracing. */
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "dispatch.h"

extern const RecompFunc g_recomp7_funcs[];
extern const unsigned g_recomp7_func_count;

#define HBITS 16
typedef struct { int32_t head[1 << HBITS]; int32_t* next; const RecompFunc* funcs; unsigned count; } Table;
static Table g_t9, g_t7;

static inline uint32_t hkey(uint32_t a) { return ((a >> 1) * 2654435761u) >> (32 - HBITS); }

static void table_init(Table* t, const RecompFunc* f, unsigned n) {
    memset(t->head, -1, sizeof t->head);
    t->next = malloc(sizeof(int32_t) * (n ? n : 1));
    t->funcs = f; t->count = n;
    for (unsigned i = 0; i < n; i++) {
        uint32_t k = hkey(f[i].addr & ~1u);
        t->next[i] = t->head[k];
        t->head[k] = (int32_t)i;
    }
}

void dispatch_init(void) {
    table_init(&g_t9, g_recomp_funcs, g_recomp_func_count);
    table_init(&g_t7, g_recomp7_funcs, g_recomp7_func_count);
}

/* ---------------------------------------------------------------- overlays */
typedef struct { uint32_t base, size, code_start, code_end; uint8_t* img; uint32_t gen; int loaded; } Ovl;
static Ovl g_ovl[64];
static int g_ovl_count;

void overlays_init(const char* extract) {
    char path[1024];
    snprintf(path, sizeof path, "%s/arm9_overlays/overlays.yaml", extract);
    FILE* f = fopen(path, "r");
    if (!f) rt_fatal("cannot open %s\n", path);
    char line[256];
    int id = -1;
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned v;
        if (sscanf(line, "- id: %u", &v) == 1) id = (int)v;
        else if (id >= 0 && id < 64 && sscanf(line, " base_address: %u", &v) == 1) g_ovl[id].base = v;
        else if (id >= 0 && id < 64 && sscanf(line, " code_size: %u", &v) == 1) g_ovl[id].size = v;
        if (id + 1 > g_ovl_count) g_ovl_count = id + 1;
    }
    fclose(f);
    for (int i = 0; i < g_ovl_count; i++) {
        snprintf(path, sizeof path, "%s/arm9_overlays/ov%03d.bin", extract, i);
        FILE* b = fopen(path, "rb");
        if (!b) rt_fatal("cannot open %s\n", path);
        g_ovl[i].img = malloc(g_ovl[i].size + 1);
        if (fread(g_ovl[i].img, 1, g_ovl[i].size, b) != g_ovl[i].size) rt_fatal("short read %s\n", path);
        fclose(b);
        g_ovl[i].gen = ~0u;
        if (g_ovl[i].base < lo) lo = g_ovl[i].base;
        if (g_ovl[i].base + g_ovl[i].size > hi) hi = g_ovl[i].base + g_ovl[i].size;
    }
    /* compare only code: data in an overlay changes once it runs */
    for (unsigned i = 0; i < g_t9.count; i++) {
        const RecompFunc* fn = &g_t9.funcs[i];
        if (fn->ovl_id >= 0 && fn->ovl_id < g_ovl_count && fn->size) {   /* size 0 = interpreted (code changes at run time) */
            Ovl* o = &g_ovl[fn->ovl_id];
            uint32_t a = fn->addr & ~1u, e = a + fn->size;
            if (e > o->code_end) o->code_end = e;
            if (!o->code_start || a < o->code_start) o->code_start = a;
        }
    }
    g_overlay_area_start = lo;
    g_overlay_area_end = hi;
    LOG_INFO("%d overlays, area %08x-%08x\n", g_ovl_count, lo, hi);
}

/* An overlay counts as loaded when its code is present in RAM. */
int overlay_is_loaded(int id) {
    if (id < 0 || id >= g_ovl_count) return 0;
    Ovl* o = &g_ovl[id];
    if (o->gen == g_overlay_gen) return o->loaded;
    o->gen = g_overlay_gen;
    /* compare code only (some overlays start with data their initialisers fill in) */
    uint32_t start = o->code_start ? o->code_start - o->base : 0;
    uint32_t end = o->code_end > o->base ? o->code_end - o->base : o->size;
    uint32_t code = end - start;
    uint32_t n = code < 256 ? code : 256;
    const uint8_t* ram = &g_main_ram[o->base & (MAIN_RAM_SIZE - 1)];
    int was = o->loaded;
    o->loaded = memcmp(ram + start, o->img + start, n) == 0;
    if (o->loaded && code > 1024) {
        uint32_t mid = (start + code / 2) & ~3u;
        o->loaded = memcmp(ram + mid, o->img + mid, 64) == 0;
    }
    if (o->loaded != was) {
        LOG_INFO("overlay %d %s\n", id, o->loaded ? "loaded" : "unloaded");
        if (!o->loaded) {
            for (uint32_t i = start; i < end; i++)
                if (ram[i] != o->img[i]) { LOG_INFO("  first difference at %08x: %02x, expected %02x (code %08x-%08x)\n",
                                                    o->base + i, ram[i], o->img[i], o->base + start, o->base + end); break; }
        }
    }
    return o->loaded;
}

/* Which overlays are loaded now (bit per overlay id; ids up to 63), for the frontend
 * (the Alchemimicon's screens, a test log). Only looks: it never touches the cache
 * the game's calls use (overlay_is_loaded), so asking at any moment - a mouse move -
 * can't change what the game runs (players far apart must stay identical). */
uint64_t overlays_loaded_mask(void) {
    uint64_t m = 0;
    for (int i = 0; i < g_ovl_count && i < 64; i++) {
        const Ovl* o = &g_ovl[i];
        uint32_t start = o->code_start ? o->code_start - o->base : 0;
        uint32_t end = o->code_end > o->base ? o->code_end - o->base : o->size;
        uint32_t code = end - start, n = code < 256 ? code : 256;
        const uint8_t* ram = &g_main_ram[o->base & (MAIN_RAM_SIZE - 1)];
        int loaded = memcmp(ram + start, o->img + start, n) == 0;
        if (loaded && code > 1024) { uint32_t mid = (start + code / 2) & ~3u; loaded = memcmp(ram + mid, o->img + mid, 64) == 0; }
        if (loaded) m |= 1ull << i;
    }
    return m;
}

/* ---------------------------------------------------------------- emulated time
 * Generated code adds each instruction run's estimated ARM9 cycles to g_cyc
 * (g_cyc7 for the ARM7). At function entries and loop iterations, once
 * CYC_SYNC has built up:
 *   ARM9: the cycles become emulated time. DQIX_CPI is bus cycles per 64
 *         estimated ARM9 cycles; the default was calibrated so the game's
 *         frame-by-frame progress matches a reference DS emulator.
 *   ARM7: hand over to the ARM9 (the ARM7 runs as a coroutine per scanline). */
#define CPI_DEFAULT 31
uint32_t g_cyc, g_cyc7;
static int g_cpi = -1;
static uint32_t g_cyc_frac;

static void arm9_sync(void) {
    if (g_cpi < 0) { const char* e = getenv("DQIX_CPI"); g_cpi = e ? atoi(e) : CPI_DEFAULT; }
    uint32_t total = g_cyc * (uint32_t)g_cpi + g_cyc_frac;
    g_cyc = 0;
    g_cyc_frac = total & 63;
    if (arm7_is_running()) return;
    time_spend(total >> 6);
    irq_service(&g_arm9);
}

/* Turn the ARM9's pending cycles into emulated time now (before comparing
 * against a hardware timeline, e.g. a card transfer, so the work done while
 * waiting is not counted twice). */
void cpu_time_sync(void) { if (g_cyc) arm9_sync(); }

void recomp_budget_expired(Cpu* c) {
    if (c == &g_arm7) { g_cyc7 = 0; arm7_yield(); }
    else arm9_sync();
}
void recomp_loop_expired(Cpu* c) { recomp_budget_expired(c); }

/* ---------------------------------------------------------------- tracing */
#define TRACE_N 256
static uint32_t g_trace[TRACE_N];
static uint8_t g_trace_cpu[TRACE_N];
static unsigned g_trace_pos;
unsigned long long g_calls_traced;

static const char* name_in(Table* t, uint32_t addr) {
    for (int32_t i = t->head[hkey(addr & ~1u)]; i >= 0; i = t->next[i])
        if ((t->funcs[i].addr & ~1u) == (addr & ~1u)) return t->funcs[i].name;
    return "?";
}
const char* recomp_func_name(uint32_t addr) { return name_in(&g_t9, addr); }
const char* recomp7_func_name(uint32_t addr) { return name_in(&g_t7, addr); }

/* Optional profile of function entries: DQIX_PROF="first_frame,last_frame". */
#define PROF_N 65536
static uint32_t g_prof_addr[PROF_N], g_prof_cnt[PROF_N];
static int g_prof_state = -1;                     /* -1 unknown, 0 off, 1 on */
static uint64_t g_prof_from, g_prof_to;
static void prof_add(uint32_t addr, int cpu) {
    if (g_prof_state < 0) {
        const char* e = getenv("DQIX_PROF");
        unsigned long long a, b;
        g_prof_state = e && sscanf(e, "%llu,%llu", &a, &b) == 2;
        if (g_prof_state) { g_prof_from = a; g_prof_to = b; }
    }
    if (!g_prof_state || g_frame < g_prof_from || g_frame > g_prof_to) return;
    uint32_t key = addr | (cpu == 7 ? 0x80000000u : 0);   /* addresses are < 0x80000000 */
    uint32_t h = (key * 2654435761u) >> 16;
    for (;; h = (h + 1) & (PROF_N - 1)) {
        if (g_prof_addr[h] == key) { g_prof_cnt[h]++; return; }
        if (!g_prof_addr[h]) { g_prof_addr[h] = key; g_prof_cnt[h] = 1; return; }
    }
}
void prof_report(FILE* f) {
    if (g_prof_state != 1) return;
    fprintf(f, "profile, frames %llu-%llu (function entries):\n", (unsigned long long)g_prof_from, (unsigned long long)g_prof_to);
    int top = getenv("DQIX_PROF_TOP") ? atoi(getenv("DQIX_PROF_TOP")) : 50;   /* how many to list */
    for (int k = 0; k < top; k++) {
        int best = -1;
        for (int i = 0; i < PROF_N; i++) if (g_prof_cnt[i] && (best < 0 || g_prof_cnt[i] > g_prof_cnt[best])) best = i;
        if (best < 0) break;
        uint32_t key = g_prof_addr[best], a = key & 0x7FFFFFFF;
        int cpu = key >> 31 ? 7 : 9;
        fprintf(f, "  %10u  ARM%d %08x %s\n", g_prof_cnt[best], cpu, a, cpu == 7 ? recomp7_func_name(a) : recomp_func_name(a));
        g_prof_cnt[best] = 0;
    }
}

void threads_note_call(int arm7, uint32_t addr);
static void trace_str(uint32_t addr);
void recomp_trace_enter(uint32_t addr) {
    prof_add(addr, 9);
    trace_str(addr);
    threads_note_call(0, addr);
    g_trace_cpu[g_trace_pos % TRACE_N] = 9;
    g_trace[g_trace_pos++ % TRACE_N] = addr;
    g_calls_traced++;
    /* DQIX_TRACEFN="addr,count[,from_frame]": dump the trace when addr is entered */
    static int st = -1; static uint32_t fn_addr; static int fn_left; static unsigned long long fn_from;
    if (st < 0) {
        const char* e = getenv("DQIX_TRACEFN");
        st = e && sscanf(e, "%x,%d,%llu", &fn_addr, &fn_left, &fn_from) >= 2;
    }
    if (st && addr == fn_addr && fn_left > 0 && g_frame >= fn_from) {
        fn_left--;
        fprintf(stderr, "[f%llu] entered %08x, lr %08x, sp %08x, r0-r3 %08x %08x %08x %08x\n", (unsigned long long)g_frame, addr, g_arm9.r[14], g_arm9.r[13], g_arm9.r[0], g_arm9.r[1], g_arm9.r[2], g_arm9.r[3]);
        fprintf(stderr, "  [r1..] "); for (int i = 0; i < 48; i++) fprintf(stderr, "%02x", mem_read8(g_arm9.r[1] + (uint32_t)i)); fprintf(stderr, "\n");
        trace_dump(stderr, getenv("DQIX_TRACEN") ? atoi(getenv("DQIX_TRACEN")) : 16);
    }
}
/* DQIX_TRACESTR="addr:rN,addr:sN,...": when the function at addr is entered,
 * log the text its argument points to (rN: in register N; sN: the Nth word on
 * the stack, the 5th argument is s0). Debugging: which file names the game asks
 * for, say. */
static void trace_str(uint32_t addr) {
    static int n = -1; static uint32_t fa[16]; static char kind[16]; static int idx[16];
    if (n < 0) {
        n = 0;
        const char* e = getenv("DQIX_TRACESTR");
        while (e && *e && n < 16) {
            unsigned a; char k; int i;
            if (sscanf(e, "%x:%c%d", &a, &k, &i) != 3) break;
            fa[n] = a; kind[n] = k; idx[n] = i; n++;
            e = strchr(e, ','); if (e) e++;
        }
    }
    for (int k = 0; k < n; k++) {
        if (fa[k] != addr) continue;
        uint32_t p = kind[k] == 's' ? mem_read32(g_arm9.r[13] + 4u * (uint32_t)idx[k]) : g_arm9.r[idx[k] & 15];
        char s[96]; int i = 0;
        for (; i < 95; i++) { uint8_t ch = mem_read8(p + (uint32_t)i); if (!ch) break; s[i] = (ch >= 32 && ch < 127) ? (char)ch : '?'; }
        s[i] = 0;
        fprintf(stderr, "[f%llu] %08x(\"%s\") lr %08x %s\n", (unsigned long long)g_frame, addr, s, g_arm9.r[14], recomp_func_name(g_arm9.r[14]));
    }
}

void recomp7_trace_enter(uint32_t addr) {
    prof_add(addr, 7);
    threads_note_call(1, addr);
    g_trace_cpu[g_trace_pos % TRACE_N] = 7;
    g_trace[g_trace_pos++ % TRACE_N] = addr;
    g_calls_traced++;
}

void trace_dump(FILE* f, int n) {
    if (!g_calls_traced) return;
    fprintf(f, "last %d functions entered (oldest first):\n", n);
    unsigned total = g_trace_pos < TRACE_N ? g_trace_pos : TRACE_N;
    if ((unsigned)n > total) n = (int)total;
    for (int k = n; k > 0; k--) {
        unsigned idx = (g_trace_pos - k) % TRACE_N;
        uint32_t a = g_trace[idx];
        int cpu = g_trace_cpu[idx];
        fprintf(f, "  ARM%d %08x %s\n", cpu, a, cpu == 7 ? recomp7_func_name(a) : recomp_func_name(a));
    }
}

/* ---------------------------------------------------------------- dispatch */
void interp_call(Core* k, uint32_t target);

/* The recompiled function at `addr` that may run now, or NULL. */
const RecompFunc* dispatch_find(Core* k, uint32_t addr) {
    Table* t = k->id == 0 ? &g_t9 : &g_t7;
    uint32_t key = addr & ~1u;
    for (int32_t i = t->head[hkey(key)]; i >= 0; i = t->next[i]) {
        const RecompFunc* f = &t->funcs[i];
        if ((f->addr & ~1u) != key) continue;
        if (!f->size) return NULL;                                 /* interpreted on purpose */
        if (f->ovl_id >= 0 && !overlay_is_loaded(f->ovl_id)) continue;
        return f;
    }
    return NULL;
}

static void dispatch_any(Core* k, Cpu* c, uint32_t target, uint32_t from) {
    const RecompFunc* f = dispatch_find(k, target);
    if (f) { f->fn(c); return; }
    /* not recompiled: code built or decrypted at run time */
    static uint32_t seen[2][64]; static int nseen[2];
    int known = 0;
    for (int i = 0; i < nseen[k->id]; i++) if (seen[k->id][i] == target) known = 1;
    if (!known && nseen[k->id] < 64) {
        seen[k->id][nseen[k->id]++] = target;
        LOG_INFO("%s: interpreting code at %08x (called from %08x)\n", k->name, target, from);
    }
    interp_call(k, target);
}

void dispatch_call(Cpu* c, uint32_t target, uint32_t from) { dispatch_any(&g_c9, c, target, from); }
void dispatch7_call(Cpu* c, uint32_t target, uint32_t from) { dispatch_any(&g_c7, c, target, from); }
void interp_stub(Cpu* c, uint32_t addr) { interp_call(core_of(c), addr); }

