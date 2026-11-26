/* The air between DS consoles (see air.h): shared memory between the
 * processes of one PC, one ring of frames per sender (single writer, so no
 * locks), and each console's current tick. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air.h"
#include "nds.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

#define RING 128
#define INRING 8192                               /* frames of inputs kept (2 min 17 s) */
#define HASHN 64

typedef struct {
    volatile uint64_t count;                      /* frames written */
    volatile uint64_t read;                       /* the console's own position (writers don't overtake it by INRING) */
    uint8_t pad[48];
    uint32_t v[INRING];
} AirInput;

typedef struct {
    volatile uint64_t tick;       /* next tick this console processes */
    volatile uint32_t gone;       /* stopped for good */
    volatile uint32_t attached;
    uint8_t pad[48];
} AirConsole;

typedef struct {
    uint32_t magic, count;
    uint8_t pad0[56];
    AirConsole con[AIR_MAX];
    volatile uint64_t wr[AIR_MAX];                /* frames written by each sender */
    uint8_t pad1[64 - 8 * AIR_MAX % 64];
    volatile uint64_t rd[AIR_MAX][AIR_MAX];       /* rd[receiver][sender]: frames it has taken */
    AirFrame ring[AIR_MAX][RING];
    AirInput in[AIR_MAX];
    volatile uint64_t hash_frame[AIR_MAX][HASHN];  /* frame + 1 (0 = empty) */
    volatile uint64_t hash[AIR_MAX][HASHN];
} AirShared;

#define AIR_MAGIC 0x52494151u                    /* "QAIR" */

static AirShared* g_air;
static int g_me = -1, g_n;
void (*air_idle_hook)(void);

/* frames taken from the rings with rx_tick <= now, not yet delivered */
#define LOCAL 64
static AirFrame g_q[LOCAL];
static uint64_t g_qseq[LOCAL];
static int g_qn;
static uint64_t g_pulled_seq;

static uint64_t ld(volatile uint64_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void st(volatile uint64_t* p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
#endif
}
/* waiting for another console: spin briefly (it is usually microseconds
 * away), then let others run, and after a couple of milliseconds sleep in
 * short naps so a console waiting on a friend's buttons doesn't burn a core. */
static double g_wait_start;
static inline void cpu_relax(void) {                            /* a spin-wait hint to the CPU */
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}
static void relax(unsigned* spins) {
    if (rt_yield()) return;                                     /* one of several consoles on a thread: let the others run */
    ++*spins;
    if (*spins < 256) { cpu_relax(); return; }
    if (*spins == 256) g_wait_start = now_ms();
    if (now_ms() - g_wait_start < 2.0) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
        return;
    }
#ifdef _WIN32
    Sleep(1);
#else
    static long nap = -1; if (nap < 0) { const char* e = getenv("DQIX_NAP_US"); nap = e ? atol(e) * 1000 : 250000; }   /* tests: Windows naps ~1-2 ms */
    struct timespec t = { 0, nap }; nanosleep(&t, NULL);
#endif
}
/* what this console is waiting for right now (for the window and the session log) */
static struct { int kind, k; uint64_t want, have; double since; } g_wait;
enum { W_NONE, W_TICK, W_RING, W_INPUT, W_INRING };
static char g_names[AIR_MAX][32];
void air_set_name(int k, const char* name) { if (k >= 0 && k < AIR_MAX) snprintf(g_names[k], sizeof g_names[k], "%s", name); }
static const char* who(int k) {
    static char b[48];
    if (k >= 0 && k < AIR_MAX && g_names[k][0]) snprintf(b, sizeof b, "%s's", g_names[k]); else snprintf(b, sizeof b, "player %d's", k + 1);
    return b;
}
static void wait_begin(int kind, int k, uint64_t want) { g_wait.kind = kind; g_wait.k = k; g_wait.want = want; g_wait.since = 0; }
static void wait_end(void) { g_wait.kind = W_NONE; }
double air_waiting_ms(void) { return g_wait.kind && g_wait.since > 0 ? now_ms() - g_wait.since : 0; }
int air_wait_describe(char* buf, size_t n, int* console) {
    if (!g_wait.kind || !g_air) return 0;
    int k = g_wait.k;
    *console = k;
    switch (g_wait.kind) {
    case W_TICK: snprintf(buf, n, "waiting for %s DS to catch up (it is at tick %llu, this one needs %llu)", who(k),
                          (unsigned long long)ld(&g_air->con[k].tick), (unsigned long long)g_wait.want); break;
    case W_RING: snprintf(buf, n, "waiting for %s DS to take its wireless frames", who(k)); break;
    case W_INPUT: snprintf(buf, n, "waiting for %s buttons for frame %llu (have up to %llu)", who(k),
                           (unsigned long long)g_wait.want, (unsigned long long)ld(&g_air->in[k].count)); break;
    case W_INRING: snprintf(buf, n, "waiting for %s DS to use up its buttons", who(k)); break;
    }
    return 1;
}
/* every console's progress, one line (session log) */
void air_status(char* buf, size_t n) {
    buf[0] = 0;
    if (!g_air) return;
    size_t o = 0;
    for (int k = 0; k < g_n && o < n; k++)
        o += (size_t)snprintf(buf + o, n - o, "%sP%d tick %llu in %llu/%llu%s", k ? ", " : "", k + 1,
                              (unsigned long long)ld(&g_air->con[k].tick), (unsigned long long)ld(&g_air->in[k].read),
                              (unsigned long long)ld(&g_air->in[k].count), g_air->con[k].gone ? " gone" : "");
}

/* The radio was off up to this tick: frames that reached this console by then
 * went unheard. While the radio is off this console doesn't wait for the
 * others, so some of those frames may only be written later - they must
 * still go unheard, or what is heard would depend on how fast each PC runs
 * (the PCs of a session would drift apart). */
static uint64_t g_deaf_until;

/* While waiting, take such unheard frames out of the rings: a console far
 * behind may be sending them, and with its ring to us full it would wait
 * for us while we wait for it (seen with 4 PCs in co-op: a console whose
 * radio came back on 1.4 s ahead of the others). Which frames go is decided
 * by their ticks alone, so it is the same on every PC. */
static void drain_deaf(void) {
    if (!g_air) return;
    for (int k = 0; k < g_n; k++) {
        if (k == g_me) continue;
        uint64_t r = g_air->rd[g_me][k];
        while (r < ld(&g_air->wr[k]) && g_air->ring[k][r % RING].rx_tick <= g_deaf_until) r++;
        if (r != g_air->rd[g_me][k]) st(&g_air->rd[g_me][k], r);
    }
}

/* wait helper: calls the idle hook every 15 ms or so */
static void waiting(double* last) {
    drain_deaf();
    double t = now_ms();
    if (g_wait.kind && g_wait.since == 0) g_wait.since = t;
    if (*last == 0) *last = t;
    else if (t - *last > 15) { *last = t; if (air_idle_hook) air_idle_hook(); }
}

/* this console is switched off: nobody waits for it any more */
static void air_leave(void) { if (g_air && g_me >= 0) g_air->con[g_me].gone = 1; }
void air_leave_now(void) { air_leave(); }
/* how far all the consoles have got, together (a host sees whether anything moved) */
uint64_t air_progress(void) {
    if (!g_air) return 0;
    uint64_t p = 0;
    for (int k = 0; k < g_n; k++) p += ld(&g_air->con[k].tick) + ld(&g_air->in[k].count) * 1000003u + ld(&g_air->in[k].read) * 7919u + g_air->con[k].gone;
    return p;
}

int air_init(const char* name, int index, int count) {
    if (count < 1 || count > AIR_MAX || index < 0 || index >= count) return 0;
    size_t size = sizeof(AirShared);
    char nm[160];
#ifdef _WIN32
    snprintf(nm, sizeof nm, "Local\\dqix-air-%s", name);
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)size, nm);
    if (!h) { LOG_WARN("air: cannot create shared memory %s\n", nm); return 0; }
    g_air = (AirShared*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
#else
    snprintf(nm, sizeof nm, "/dqix-air-%s", name);
    int fd = shm_open(nm, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { LOG_WARN("air: cannot open shared memory %s\n", nm); return 0; }
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return 0; }
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    g_air = p == MAP_FAILED ? NULL : (AirShared*)p;
#endif
    if (!g_air) return 0;
    if (g_air->magic != AIR_MAGIC) { g_air->count = (uint32_t)count; g_air->magic = AIR_MAGIC; }
    g_me = index; g_n = count;
    g_air->con[g_me].attached = 1;
#ifdef _WIN32
    { HMODULE w = LoadLibraryA("winmm.dll");                    /* 1 ms sleeps instead of 15.6 ms */
      typedef unsigned (__stdcall *TBP)(unsigned); TBP tbp = w ? (TBP)(void*)GetProcAddress(w, "timeBeginPeriod") : NULL;
      if (tbp) tbp(1); }
#endif
    atexit(air_leave);
    LOG_INFO("air: console %d of %d (%s)\n", index + 1, count, name);
    return 1;
}

int air_active(void) { return g_air != NULL; }
int air_index(void) { return g_me; }
int air_count(void) { return g_n; }
void air_set_gone(int index) { if (g_air && index >= 0 && index < g_n) g_air->con[index].gone = 1; }

void air_reach(uint64_t tick, int listening) {
    if (!g_air) return;
    st(&g_air->con[g_me].tick, tick);
    if (!listening || tick <= AIR_LOOKAHEAD) return;
    uint64_t need = tick - AIR_LOOKAHEAD;
    for (int k = 0; k < g_n; k++) {
        if (k == g_me) continue;
        unsigned spins = 0; double last = 0;
        if (g_air->con[k].gone || ld(&g_air->con[k].tick) >= need) continue;
        wait_begin(W_TICK, k, need);
        while (!g_air->con[k].gone && ld(&g_air->con[k].tick) < need) { relax(&spins); waiting(&last); }
        wait_end();
    }
}

void air_send(uint64_t rx_tick, int kind, int aid, uint32_t cmd_id, const uint8_t* data, int len) {
    if (!g_air || len <= 0) return;
    if (len > AIR_FRAME_MAX) len = AIR_FRAME_MAX;
    uint64_t w = g_air->wr[g_me];
    /* ring full: wait until every receiver that is still around has taken the oldest */
    for (int k = 0; k < g_n; k++) {
        if (k == g_me) continue;
        unsigned spins = 0; double last = 0;
        wait_begin(W_RING, k, w);
        while (!g_air->con[k].gone && w - ld(&g_air->rd[k][g_me]) >= RING) { relax(&spins); waiting(&last); }
        wait_end();
    }
    AirFrame* f = &g_air->ring[g_me][w % RING];
    f->rx_tick = rx_tick; f->cmd_id = cmd_id; f->kind = (uint8_t)kind; f->aid = (uint8_t)aid; f->from = (uint8_t)g_me;
    f->len = (uint16_t)len;
    memcpy(f->data, data, (size_t)len);
    st(&g_air->wr[g_me], w + 1);
}


/* move every frame that starts by `now` from the rings into the local queue, in (tick, sender) order */
static void pull(uint64_t now) {
    for (;;) {
        int best = -1; uint64_t bt = 0;
        for (int k = 0; k < g_n; k++) {
            if (k == g_me) continue;
            uint64_t r = g_air->rd[g_me][k];
            if (r >= ld(&g_air->wr[k])) continue;
            const AirFrame* f = &g_air->ring[k][r % RING];
            if (f->rx_tick <= g_deaf_until) { st(&g_air->rd[g_me][k], r + 1); k--; continue; }   /* sent while we were deaf */
            if (f->rx_tick > now) continue;
            if (best < 0 || f->rx_tick < bt) { best = k; bt = f->rx_tick; }
        }
        if (best < 0) return;
        uint64_t r = g_air->rd[g_me][best];
        const AirFrame* f = &g_air->ring[best][r % RING];
        if (f->kind == AIR_REPLY && (int)(f->cmd_id >> 28) != g_me) { st(&g_air->rd[g_me][best], r + 1); continue; }   /* a reply to another host */
        if (g_qn == LOCAL) { memmove(&g_q[0], &g_q[1], sizeof(AirFrame) * (LOCAL - 1)); memmove(&g_qseq[0], &g_qseq[1], 8 * (LOCAL - 1)); g_qn--; }
        g_q[g_qn] = g_air->ring[best][r % RING];
        g_qseq[g_qn++] = g_pulled_seq++;
        st(&g_air->rd[g_me][best], r + 1);
    }
}

static void drop(int i) {
    memmove(&g_q[i], &g_q[i + 1], sizeof(AirFrame) * (size_t)(g_qn - i - 1));
    memmove(&g_qseq[i], &g_qseq[i + 1], 8 * (size_t)(g_qn - i - 1));
    g_qn--;
}

int air_recv(uint64_t now, AirFrame* out) {
    if (!g_air) return 0;
    pull(now);
    for (int i = 0; i < g_qn; i++) {
        if (g_q[i].kind != AIR_FRAME) continue;
        *out = g_q[i];
        drop(i);
        return 1;
    }
    return 0;
}

void air_skip(uint64_t now) {
    if (!g_air) return;
    if (now > g_deaf_until) g_deaf_until = now;
    pull(now);
    g_qn = 0;
}

int air_take_reply(uint64_t now, uint32_t cmd_id, int aid, AirFrame* out) {
    if (!g_air) return 0;
    pull(now);
    for (int i = 0; i < g_qn; i++) {
        if (g_q[i].kind != AIR_REPLY || g_q[i].cmd_id != cmd_id || g_q[i].aid != aid) continue;
        *out = g_q[i];
        drop(i);
        return 1;
    }
    /* replies to older CMDs are of no use any more */
    for (int i = 0; i < g_qn;) { if (g_q[i].kind == AIR_REPLY && g_q[i].cmd_id != cmd_id) drop(i); else i++; }
    return 0;
}

/* DQIX_AIR="name,index,count" (tests: several copies on one PC). */
void air_init_from_env(void) {
    static int done; if (done) return; done = 1;
    const char* e = getenv("DQIX_AIR");
    if (!e) return;
    char name[64]; int idx, cnt;
    if (sscanf(e, "%63[^,],%d,%d", name, &idx, &cnt) == 3) air_init(name, idx, cnt);
}

/* ---------------------------------------------------------------- inputs */
void air_input_put(int k, uint64_t frame, uint32_t v) {
    if (!g_air || k < 0 || k >= g_n) return;
    AirInput* in = &g_air->in[k];
    uint64_t c = in->count;
    if (frame < c) return;                                    /* already there */
    while (c <= frame) {
        unsigned spins = 0; double last = 0;
        wait_begin(W_INRING, k, c);
        while (c - ld(&in->read) >= INRING && !g_air->con[k].gone) { relax(&spins); waiting(&last); }
        wait_end();
        in->v[c % INRING] = c == frame ? v : 0;
        c++;
        st(&in->count, c);
    }
}
uint64_t air_input_count(int k) { return g_air && k >= 0 && k < g_n ? ld(&g_air->in[k].count) : 0; }
int air_input_peek(int k, uint64_t frame, uint32_t* v) {
    if (!g_air || k < 0 || k >= g_n) return 0;
    AirInput* in = &g_air->in[k];
    uint64_t c = ld(&in->count);
    if (frame >= c || c - frame > INRING) return 0;
    *v = in->v[frame % INRING];
    return 1;
}
uint32_t air_input_get(int k, uint64_t frame) {
    AirInput* in = &g_air->in[k];
    unsigned spins = 0; double last = 0;
    wait_begin(W_INPUT, k, frame);
    while (ld(&in->count) <= frame) { relax(&spins); waiting(&last); }
    wait_end();
    uint32_t v = in->v[frame % INRING];
    if (k == g_me) st(&in->read, frame);
    return v;
}

/* ---------------------------------------------------------------- fingerprints */
void air_hash_put(int k, uint64_t frame, uint64_t h) {
    if (!g_air || k < 0 || k >= g_n) return;
    int i = (int)((frame / 60) % HASHN);
    g_air->hash[k][i] = h;
    st(&g_air->hash_frame[k][i], frame + 1);
}
int air_hash_get(int k, uint64_t frame, uint64_t* h) {
    if (!g_air || k < 0 || k >= g_n) return 0;
    for (int i = 0; i < HASHN; i++)
        if (ld(&g_air->hash_frame[k][i]) == frame + 1) { *h = g_air->hash[k][i]; return 1; }
    return 0;
}
