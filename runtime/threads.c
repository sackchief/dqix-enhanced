/* Game (Nitro SDK) threads on native coroutines, for both CPUs.
 *
 * The SDK switches threads by saving every register into the thread's
 * OSContext and jumping into the middle of another thread's code. Recompiled
 * code can't jump into the middle of a C function, so these SDK routines are
 * replaced (see hle.txt / hle7.txt):
 *   OS_SaveContext   write the context, remember the thread
 *   OS_LoadContext   switch to that thread's native coroutine
 *   OS_IrqHandler_ThreadSwitch  interrupt-time switch, performed after the
 *                    interrupt handler has returned
 * The rest of the SDK scheduler (priorities, queues, sleep) runs recompiled.
 *
 * OSContext: +0 cpsr, +4..+0x3C r0-r14, +0x40 resume pc+4, +0x44 SVC sp,
 * (ARM9 only) +0x48 divider/sqrt state.
 *
 * The ARM7 runs as a second CPU: arm7_run() switches from whichever ARM9
 * thread is running to the current ARM7 thread; arm7_yield() switches back.
 */
#include <string.h>
#include "nds.h"
#include "coro.h"

#define RESUME_IRQ   0xFFFF0004u   /* our marker: switched out at an interrupt */
#define THREAD_STACK (2u << 20)
#define MAX_THREADS  64

typedef struct Thread {
    uint32_t ctx;          /* OSContext address (== OSThread address) */
    Coro co;
    Cpu cpu;               /* register state while switched out */
    uint32_t start_pc;
    int alive;
    uint32_t hist[32];     /* recent function entries on this thread (debugging) */
    unsigned nhist;
} Thread;

typedef struct ThreadSys {
    Core* core;
    uint32_t cp_save, cp_restore;      /* ARM9 divider/sqrt context helpers (0 on ARM7) */
    uint32_t resume_saved;             /* pc+4 that OS_SaveContext stores */
    uint32_t threadinfo;               /* OSi_ThreadInfo: +0 isNeedRescheduling, +4 current, +8 list, +0xC switch callback */
    uint32_t irq_queue;                /* OSThreadQueue {head, tail} of threads woken by interrupts */
    uint32_t off_state, off_list_next, off_q1, off_q2, off_link;
    uint32_t irq_switch_from, irq_switch_to;
    Thread threads[MAX_THREADS];
    Thread* cur;
} ThreadSys;

static ThreadSys g_ts9 = { &g_c9, 0x020CD594, 0x020CD5D4, 0x020C7D7C, 0x02111304, 0x027E0060,
                           0x64, 0x68, 0x78, 0x7C, 0x80, 0, 0, {{0}}, NULL };
static ThreadSys g_ts7 = { &g_c7, 0, 0, 0x037FCA8C, 0x03808FD0, 0x03808E34,
                           0x48, 0x4C, 0x5C, 0x60, 0x64, 0, 0, {{0}}, NULL };
unsigned g_thread_switches;
static Coro* g_arm9_resume;     /* the ARM9 coroutine to return to from the ARM7 */
static int g_in_arm7;

static Thread* find(ThreadSys* ts, uint32_t ctx) {
    for (int i = 0; i < MAX_THREADS; i++) if (ts->threads[i].alive && ts->threads[i].ctx == ctx) return &ts->threads[i];
    return NULL;
}
static Thread* alloc_thread(ThreadSys* ts, uint32_t ctx) {
    for (int i = 0; i < MAX_THREADS; i++) if (!ts->threads[i].alive) {
        Thread* t = &ts->threads[i];
        memset(t, 0, sizeof *t);
        t->ctx = ctx; t->alive = 1;
        return t;
    }
    rt_fatal("%s: too many game threads\n", ts->core->name);
}

void threads_init(void) {
    if (rom_is_eur()) {                            /* these three are in the main code, which moved */
        g_ts9.cp_save += 0x10; g_ts9.cp_restore += 0x10; g_ts9.resume_saved += 0x10;
    }
    g_c9.threads = &g_ts9;
    g_c7.threads = &g_ts7;
    g_ts9.cur = alloc_thread(&g_ts9, 0);           /* the host's main stack runs the ARM9 */
    coro_init_main(&g_ts9.cur->co);
}

/* Priority of the ARM9 thread running now (OSThread +0x70; 0 = highest, 32 = idle). */
int threads_current_priority9(void) {
    Thread* t = g_ts9.cur;
    if (!t || !t->ctx) return 16;
    return (int)g_c9.bus->r32(t->ctx + g_ts9.off_state + 0xC);
}

/* Record a function entry for the thread running on this CPU. */
void threads_note_call(int arm7, uint32_t addr) {
    Thread* t = arm7 ? g_ts7.cur : g_ts9.cur;
    if (t) t->hist[t->nhist++ % 32] = addr;
}

const char* recomp_func_name(uint32_t addr);
const char* recomp7_func_name(uint32_t addr);
static void dump_sys(FILE* f, ThreadSys* ts) {
    const Bus* b = ts->core->bus;
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread* t = &ts->threads[i];
        if (!t->alive) continue;
        uint32_t state = t->ctx ? b->r32(t->ctx + ts->off_state) : 0;
        uint32_t prio = t->ctx ? b->r32(t->ctx + ts->off_state + 0xC) : 0;
        fprintf(f, "%s thread %08x (start %08x) state %u prio %u%s, last calls (newest last):\n", ts->core->name, t->ctx, t->start_pc,
                state, prio, t == ts->cur ? " [running]" : "");
        unsigned n = t->nhist < 12 ? t->nhist : 12;
        for (unsigned k = n; k > 0; k--) {
            uint32_t a = t->hist[(t->nhist - k) % 32];
            fprintf(f, "    %08x %s\n", a, ts == &g_ts7 ? recomp7_func_name(a) : recomp_func_name(a));
        }
    }
}
void ipc_dump(FILE* f);
void threads_dump(FILE* f) { ipc_dump(f); fprintf(f, "arm9 IE %08x IF %08x IME %x / arm7 IE %08x IF %08x IME %x\n", g_c9.ie, g_c9.iflags, g_c9.ime, g_c7.ie, g_c7.iflags, g_c7.ime); dump_sys(f, &g_ts9); if (getenv("DQIX_THREADS7")) dump_sys(f, &g_ts7); }

void* threads_current_coro(Core* k) { return &k->threads->cur->co; }
int arm7_is_running(void) { return g_in_arm7; }

static void ensure_ctx(ThreadSys* ts, uint32_t ctx) { if (ts->cur && !ts->cur->ctx) ts->cur->ctx = ctx; }

static void call_fn(Core* k, uint32_t fn, uint32_t r0) {
    if (!fn) return;
    Cpu* c = k->cpu;
    uint32_t save[16]; memcpy(save, c->r, sizeof save);
    c->r[0] = r0;
    k->bus->call(c, fn, 0);
    memcpy(c->r, save, sizeof save);
}

static void write_context(ThreadSys* ts, uint32_t ctx, uint32_t resume) {
    Cpu* c = ts->core->cpu;
    const Bus* b = ts->core->bus;
    b->w32(ctx + 0, CPSR_GET(c));
    for (int i = 0; i < 15; i++) b->w32(ctx + 4 + 4 * i, c->r[i]);
    b->w32(ctx + 4, 1);                   /* resumes with r0 = 1 */
    b->w32(ctx + 0x40, resume);
    b->w32(ctx + 0x44, (c->cpsr_ctl & 0x1F) == 0x13 ? c->r[13] : c->bank_r13[3]);
}

static ThreadSys* g_starting;     /* which CPU's thread is starting (set right before the switch) */

static void thread_entry(void) {
    ThreadSys* ts = g_starting;
    Thread* t = ts->cur;
    Cpu* c = ts->core->cpu;
    uint32_t lr = c->r[14];
    LOG_INFO("%s thread %08x starts at %08x\n", ts->core->name, t->ctx, t->start_pc);
    ts->core->bus->call(c, t->start_pc, 0);
    /* the thread function returned: continue at its link register (OS_ExitThread) */
    ts->core->bus->call(c, lr, t->start_pc);
    rt_fatal("%s thread %08x returned from its exit routine\n", ts->core->name, t->ctx);
}

static void switch_to(ThreadSys* ts, uint32_t ctx) {
    Core* k = ts->core;
    const Bus* b = k->bus;
    Thread* cur = ts->cur;
    Thread* t = find(ts, ctx);
    uint32_t resume = b->r32(ctx + 0x40);
    if (t == cur) { LOG_WARN("%s thread %08x switching to itself\n", k->name, ctx); return; }
    int resumable = t && (resume == ts->resume_saved || resume == RESUME_IRQ);
    cur->cpu = *k->cpu;
    g_thread_switches++;
    if (resumable) {
        ts->cur = t;
        *k->cpu = t->cpu;
    } else {
        /* first run of this context (or a context re-initialised for reuse) */
        if (t) t->alive = 0;
        t = alloc_thread(ts, ctx);
        Cpu n = *k->cpu;
        uint32_t cpsr = b->r32(ctx);
        for (int i = 0; i < 15; i++) n.r[i] = b->r32(ctx + 4 + 4 * i);
        n.r[15] = 0;
        n.n = cpsr >> 31; n.z = (cpsr >> 30) & 1; n.cf = (cpsr >> 29) & 1; n.v = (cpsr >> 28) & 1; n.q = (cpsr >> 27) & 1;
        n.cpsr_ctl = cpsr & 0x07FFFFFF & ~0x20u;
        n.bank_r13[3] = b->r32(ctx + 0x44);
        t->start_pc = (resume - 4) | ((cpsr >> 5) & 1);
        coro_create(&t->co, thread_entry, THREAD_STACK);
        ts->cur = t;
        *k->cpu = n;
        g_starting = ts;
    }
    LOG_DEBUG("%s switch %08x -> %08x%s\n", k->name, cur->ctx, t->ctx, resumable ? "" : " (new)");
    coro_switch(&cur->co, &t->co);
    /* back on this thread: whoever switched here loaded our registers */
}

/* ---------------------------------------------------------------- replaced SDK functions */
static void save_context(ThreadSys* ts) {
    Cpu* c = ts->core->cpu;
    uint32_t ctx = c->r[0];
    ensure_ctx(ts, ctx);
    call_fn(ts->core, ts->cp_save, ctx + 0x48);
    write_context(ts, ctx, ts->resume_saved);
    c->r[0] = 0;
}

static void load_context(ThreadSys* ts) {
    Cpu* c = ts->core->cpu;
    uint32_t ctx = c->r[0];
    ensure_ctx(ts, ts->core->bus->r32(ts->threadinfo + 4));
    call_fn(ts->core, ts->cp_restore, ctx + 0x48);
    switch_to(ts, ctx);
    ts->core->cpu->r[0] = 1;
}

static void irq_thread_switch(ThreadSys* ts) {
    Core* k = ts->core;
    const Bus* b = k->bus;
    uint32_t q = ts->irq_queue;
    uint32_t t = b->r32(q);
    if (t) {
        while (t) {
            b->w32(t + ts->off_state, 1);            /* ready */
            b->w32(t + ts->off_q1, 0);
            b->w32(t + ts->off_q2, 0);
            uint32_t next = b->r32(t + ts->off_link);
            b->w32(t + ts->off_link, 0);
            t = next;
        }
        b->w32(q, 0); b->w32(q + 4, 0);
        b->w16(ts->threadinfo, 1);
    }
    if (!b->r16(ts->threadinfo)) return;
    b->w16(ts->threadinfo, 0);
    uint32_t cur = b->r32(ts->threadinfo + 4);
    uint32_t next = b->r32(ts->threadinfo + 8);
    while (next && (b->r32(next + ts->off_state) & 0xFFFF) != 1) next = b->r32(next + ts->off_list_next);
    if (!next || next == cur) return;
    uint32_t cb = b->r32(ts->threadinfo + 0xC);
    if (cb) {
        Cpu* c = k->cpu;
        uint32_t save[16]; memcpy(save, c->r, sizeof save);
        c->r[0] = cur; c->r[1] = next;
        b->call(c, cb, 0);
        memcpy(c->r, save, sizeof save);
    }
    b->w32(ts->threadinfo + 4, next);
    ts->irq_switch_from = cur;
    ts->irq_switch_to = next;
}

/* Called by irq_service after the interrupted registers are back. */
void threads_after_irq(Cpu* c) {
    ThreadSys* ts = core_of(c)->threads;
    uint32_t next = ts->irq_switch_to;
    if (!next) return;
    ts->irq_switch_to = 0;
    uint32_t prev = ts->irq_switch_from;
    ensure_ctx(ts, prev);
    /* No interrupt may be taken until the switch is done (on the DS this all
     * happens inside the IRQ handler, with IRQs masked). The helpers below run
     * game code, where a pending IRQ would otherwise be delivered: its own
     * thread switch would start inside this one, the game's idea of the
     * current thread and the one running would part, and a thread could put
     * the idle thread to sleep: every thread waiting, a frozen game. */
    Core* k = ts->core;
    k->irq_depth += 100;                                   /* irq_service: not now */
    call_fn(ts->core, ts->cp_save, prev + 0x48);
    write_context(ts, prev, RESUME_IRQ);
    call_fn(ts->core, ts->cp_restore, next + 0x48);
    k->irq_depth -= 100;
    switch_to(ts, next);
}

/* ARM9 (by name: the address depends on the ROM's version, see hle.txt) */
void hle_OS_SaveContext(Cpu* c) { (void)c; save_context(&g_ts9); }
void hle_OS_LoadContext(Cpu* c) { (void)c; load_context(&g_ts9); }
void hle_OS_IrqHandler_ThreadSwitch(Cpu* c) { irq_thread_switch(&g_ts9); c->r[13] += 4; }   /* pops OS_IrqHandler's lr */
/* ARM7 */
void fn_a7w_037fca58(Cpu* c) { (void)c; save_context(&g_ts7); }
void fn_a7w_037fca8c(Cpu* c) { (void)c; load_context(&g_ts7); }
void fn_a7w_037fb528(Cpu* c) { irq_thread_switch(&g_ts7); c->r[13] += 4; }

/* ---------------------------------------------------------------- the ARM7 as a coroutine */
static uint32_t g_arm7_entry;

static void arm7_main(void) {
    LOG_INFO("ARM7 starts at %08x\n", g_arm7_entry);
    dispatch7_call(&g_arm7, g_arm7_entry, 0);
    rt_fatal("ARM7 entry returned\n");
}

void threads_start_arm7(uint32_t entry) {
    g_arm7_entry = entry;
    g_ts7.cur = alloc_thread(&g_ts7, 0);
    coro_create(&g_ts7.cur->co, arm7_main, THREAD_STACK);
}

void arm7_run(void) {
    if (g_in_arm7 || !g_ts7.cur) return;
    g_in_arm7 = 1;
    g_arm9_resume = &g_ts9.cur->co;
    coro_switch(g_arm9_resume, &g_ts7.cur->co);
    g_in_arm7 = 0;
}

void arm7_yield(void) {
    if (!g_in_arm7) return;
    coro_switch(&g_ts7.cur->co, g_arm9_resume);
    /* resumed: time has passed, take any interrupt that arrived meanwhile */
    irq_service(&g_arm7);
}
uint32_t threads_cur_ctx9(void) { return g_ts9.cur ? g_ts9.cur->ctx : 0; }
