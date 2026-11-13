/* Playing with friends far away: lobby, lockstep inputs over TCP, the hidden
 * consoles (see session.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "nds.h"
#include "air.h"
#include "session.h"
#include "version.h"
#include "upnp.h"
#include "portmap.h"
#include "relay.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <time.h>
typedef SOCKET sock_t;
#define BADSOCK INVALID_SOCKET
#define closesock closesocket
static CRITICAL_SECTION g_mx;
static void mx_init(void) { InitializeCriticalSection(&g_mx); }
static void lock(void) { EnterCriticalSection(&g_mx); }
static void unlock(void) { LeaveCriticalSection(&g_mx); }
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static int sock_err_again(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
static int sock_errno(void) { return WSAGetLastError(); }
static void make_dir(const char* p) { _mkdir(p); }
#include <io.h>
/* the names in a folder (files or folders) */
static int list_dir(const char* dir, int want_dirs, char names[][64], int max) {
    char pat[600]; snprintf(pat, sizeof pat, "%s\\*", dir);
    struct _finddata_t fd; intptr_t h = _findfirst(pat, &fd); int n = 0;
    if (h == -1) return 0;
    do {
        if (fd.name[0] == '.' || !(fd.attrib & _A_SUBDIR) != !want_dirs || strlen(fd.name) >= 64) continue;
        if (n < max) snprintf(names[n++], 64, "%s", fd.name);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
    return n;
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include "self_exe.h"
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define BADSOCK (-1)
#define closesock close
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static void mx_init(void) {}
static void lock(void) { pthread_mutex_lock(&g_mx); }
static void unlock(void) { pthread_mutex_unlock(&g_mx); }
static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static int sock_err_again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }
static int sock_errno(void) { return errno; }
static void make_dir(const char* p) { mkdir(p, 0755); }
#include <dirent.h>
static int list_dir(const char* dir, int want_dirs, char names[][64], int max) {
    DIR* d = opendir(dir); int n = 0;
    if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= 64) continue;
        char p[700]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        if (!S_ISDIR(st.st_mode) != !want_dirs) continue;
        if (n < max) snprintf(names[n++], 64, "%s", e->d_name);
    }
    closedir(d);
    return n;
}
#endif

void (*session_lobby_hook)(void);

static double now_ms(void);
static void flog(const char* fmt, ...);
static uint64_t build_id(void);
/* message types */
enum { M_HELLO = 1, M_WELCOME, M_LOBBY, M_START, M_INPUT, M_HASH, M_LEAVE, M_PING, M_PONG, M_REFUSE };
#define MAGIC "DQIXMP01"
#define HASH_EVERY 120

typedef struct {
    sock_t s;
    uint8_t* in; size_t in_len, in_cap;
    uint8_t* out; size_t out_len, out_cap;
    int index;                      /* console number (host side: the guest's) */
    int hello;                      /* host: its HELLO arrived */
    double ping_sent;
    double heard;                   /* when something last arrived from it */
    /* tests: DQIX_NETDELAY=ms holds what we send that long (an internet's latency on one PC) */
    uint64_t queued, sent, held_upto; /* bytes ever put in / sent / allowed out */
    struct { double t; uint64_t end; } mark[1024]; int mhead, mtail;
} Peer;

typedef struct {
    char name[32];
    uint8_t* save; uint32_t save_len;
    uint8_t* fw; uint32_t fw_len;
} Console;

static int g_hidden, g_running, g_requested, g_host, g_me, g_n = 1, g_delay = 4;
static int g_state;                 /* SessionInfo.state */
static int g_start_req, g_cancel_req, g_auto_players;
static Console g_con[SESSION_MAX];
static int g_ping[SESSION_MAX], g_gone[SESSION_MAX];
static char g_addr[128], g_msg[256], g_name[32] = "Player", g_hint[1000];
static int g_port = SESSION_PORT, g_upnp_done;
static int g_reach;                     /* host: 1 direct, 2 a code (the relay), 3 the home network only */
static int g_desync, g_local;
static Peer g_peer[SESSION_MAX];    /* host: guests 1..3 (by slot); guest: [0] = the host */
static sock_t g_listen = BADSOCK;
static int64_t g_rtc;
static char g_rom_path[1024], g_extract_path[1024], g_shm[64];
static uint64_t g_sent_frames;      /* my inputs sent so far */
static uint64_t g_hash_next = HASH_EVERY;   /* my next fingerprint to send */
typedef struct { int k; uint64_t frame, hash; } PendingHash;
static PendingHash g_pending[64];
static int g_npending;

int session_hidden(void) { return g_hidden; }
int session_running(void) { return g_running; }
int session_console(void) { return g_me; }
int session_players(void) { return g_n; }
int session_requested(void) { return g_requested || g_hidden; }

/* session\\front.log: what happened in the session on this PC, for when something goes wrong */
static FILE* g_flog;
static uint64_t g_build[SESSION_MAX];   /* the other PCs' build fingerprints, where known (the log) */
static double g_flog_t0;
static void flog(const char* fmt, ...) {
    if (!g_flog) return;
    char line[1200];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    fprintf(g_flog, "[%9.3f] %s\n", (now_ms() - g_flog_t0) / 1000.0, line);
    fflush(g_flog);
}

static void flog_line(const char* line) { flog("%s", line); }
/* open session\\front.log as soon as hosting or joining begins, so a session that never
 * gets going can be looked into too (it is written line by line: it can be read while it happens) */
static void flog_open(const char* mode) {
    if (g_flog) return;
    make_dir("session");
    if (!(g_flog = fopen("session/front.log", "w"))) return;
    g_flog_t0 = now_ms();
    time_t t = time(NULL);
    char when[64]; strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&t));
    flog("Dragon Quest IX %s, build %016llx, %s, %s", DQIX_VERSION, (unsigned long long)build_id(), when, mode);
    relay_set_log(flog_line);
}
static const char* addr_text(const struct sockaddr* a, char* out, size_t n) {
    char h[INET6_ADDRSTRLEN + 1] = "?", sv[16] = "?";
    getnameinfo(a, a->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in), h, sizeof h, sv, sizeof sv,
                NI_NUMERICHOST | NI_NUMERICSERV);
    snprintf(out, n, a->sa_family == AF_INET6 ? "[%s]:%s" : "%s:%s", h, sv);
    return out;
}

static void news(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char m[256]; vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    lock(); snprintf(g_msg, sizeof g_msg, "%s", m); unlock();
    char line[300]; snprintf(line, sizeof line, "session: %s\n", m);
    LOG_WARN("%s", line);
    report_log_line(line);
    flog("%s", m);
}

/* a fingerprint of this build (the program and the game library next to it):
 * PCs with different builds may not compute exactly the same things */
static int self_dir(char* out, size_t n);
static uint64_t fnv_file(const char* path, uint64_t h) {
    FILE* f = fopen(path, "rb");
    if (!f) return h;
    static uint8_t buf[1 << 16]; size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) for (size_t i = 0; i < got; i++) h = (h ^ buf[i]) * 0x100000001b3ull;
    fclose(f);
    return h;
}
static uint64_t build_id(void) {
    static uint64_t id;
    if (id) return id;
    uint64_t h = 0xcbf29ce484222325ull;
    char dir[1024] = "", path[1200];
#ifdef _WIN32
    char exe[MAX_PATH]; DWORD l = GetModuleFileNameA(NULL, exe, MAX_PATH); if (l && l < MAX_PATH) h = fnv_file(exe, h);
    if (self_dir(dir, sizeof dir)) { snprintf(path, sizeof path, "%s\\dqixcore.dll", dir); h = fnv_file(path, h); }
#else
    { char exe[1024]; if (rt_self_exe(exe, sizeof exe) > 0) h = fnv_file(exe, h); }
    if (self_dir(dir, sizeof dir)) { snprintf(path, sizeof path, "%s/dqixcore.so", dir); h = fnv_file(path, h); }
#endif
    return id = h | 1;
}

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
#endif
}

/* ---------------------------------------------------------------- packing */
uint32_t session_pack(uint16_t keys, int touch, int x, int y) {
    if (x < 0) x = 0;
    if (x > 255) x = 255;
    if (y < 0) y = 0;
    if (y > 255) y = 255;
    return (uint32_t)(keys & 0x1FFF) | (touch ? 1u << 13 : 0) | (touch ? (uint32_t)x << 14 | (uint32_t)y << 22 : 0);
}
void session_unpack(uint32_t v, uint16_t* keys, int* touch, int* x, int* y) {
    *keys = (uint16_t)(v & 0x1FFF);
    *touch = (v >> 13) & 1;
    if (*touch) { *x = (int)((v >> 14) & 0xFF); *y = (int)((v >> 22) & 0xFF); }
}

/* ---------------------------------------------------------------- messages */
static void put_bytes(Peer* p, const void* d, size_t n) {
    if (p->s == BADSOCK) return;
    if (p->out_len + n > p->out_cap) { p->out_cap = (p->out_len + n) * 2 + 4096; p->out = realloc(p->out, p->out_cap); }
    memcpy(p->out + p->out_len, d, n); p->out_len += n;
}
typedef struct { uint8_t* d; size_t n, cap; } Buf;
static void b_put(Buf* b, const void* d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 256; b->d = realloc(b->d, b->cap); }
    memcpy(b->d + b->n, d, n); b->n += n;
}
static void b_u8(Buf* b, uint8_t v) { b_put(b, &v, 1); }
static void b_u32(Buf* b, uint32_t v) { uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; b_put(b, t, 4); }
static void b_u64(Buf* b, uint64_t v) { b_u32(b, (uint32_t)v); b_u32(b, (uint32_t)(v >> 32)); }
static void b_str(Buf* b, const char* s, size_t n) { char t[128] = { 0 }; snprintf(t, sizeof t, "%s", s); b_put(b, t, n); }
static double g_netdelay = -1;
static void send_msg(Peer* p, int type, const Buf* b) {
    uint8_t h[5] = { 0 };
    uint32_t n = b ? (uint32_t)b->n : 0;
    h[0] = (uint8_t)n; h[1] = (uint8_t)(n >> 8); h[2] = (uint8_t)(n >> 16); h[3] = (uint8_t)(n >> 24); h[4] = (uint8_t)type;
    put_bytes(p, h, 5);
    if (n) put_bytes(p, b->d, n);
    p->queued += 5 + n;
    if (g_netdelay < 0) g_netdelay = getenv("DQIX_NETDELAY") ? atof(getenv("DQIX_NETDELAY")) : 0;
    if (g_netdelay > 0) {
        int next = (p->mtail + 1) % 1024;
        if (next == p->mhead) p->mhead = (p->mhead + 1) % 1024;              /* full: the oldest goes now */
        p->mark[p->mtail].t = now_ms() + g_netdelay; p->mark[p->mtail].end = p->queued; p->mtail = next;
    }
}
typedef struct { const uint8_t* d; size_t n, pos; int bad; } Rd;
static int64_t g_utc_off;               /* the host's time zone (rtc_utc_offset) */
static Rd g_wfc_rd;                     /* the start message's Wi-Fi files, for do_start */
static uint8_t* g_wfc_buf;              /* host: its own start message (freed after) */
static const uint8_t* r_get(Rd* r, size_t n) { if (r->pos + n > r->n) { r->bad = 1; return NULL; } const uint8_t* p = r->d + r->pos; r->pos += n; return p; }
static uint8_t r_u8(Rd* r) { const uint8_t* p = r_get(r, 1); return p ? p[0] : 0; }
static uint32_t r_u32(Rd* r) { const uint8_t* p = r_get(r, 4); return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0; }
static uint64_t r_u64(Rd* r) { uint64_t lo = r_u32(r); return lo | (uint64_t)r_u32(r) << 32; }
static void r_str(Rd* r, char* out, size_t n) { const uint8_t* p = r_get(r, n); if (p) { memcpy(out, p, n); out[n - 1] = 0; } else out[0] = 0; }
static uint8_t* r_blob(Rd* r, uint32_t* len) {
    uint32_t n = r_u32(r);
    const uint8_t* p = r_get(r, n);
    if (!p) return NULL;
    uint8_t* m = malloc(n ? n : 1); memcpy(m, p, n); *len = n; return m;
}

static void peer_close(Peer* p) {
    if (p->s != BADSOCK) closesock(p->s);
    p->s = BADSOCK;
    p->in_len = p->out_len = 0;
}

/* ---------------------------------------------------------------- sockets */
static void sock_nonblock(sock_t s) {
#ifdef _WIN32
    u_long one = 1; ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int one1 = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one1, sizeof one1);
}

static int net_up(void) {
#ifdef _WIN32
    static int done; if (done) return 1;
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 0;
    done = 1;
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    return 1;
}

/* flush what's queued; read what's there. -1 = the connection is gone */
static int peer_io(Peer* p) {
    if (p->s == BADSOCK) return -1;
    size_t may = p->out_len;
    if (g_netdelay > 0) {                                         /* tests: only what has waited long enough */
        double t = now_ms(); uint64_t upto = p->sent;
        while (p->mhead != p->mtail && p->mark[p->mhead].t <= t) { upto = p->mark[p->mhead].end; p->mhead = (p->mhead + 1) % 1024; }
        p->held_upto = upto > p->held_upto ? upto : p->held_upto;
        may = (size_t)(p->held_upto - p->sent);
        if (may > p->out_len) may = p->out_len;
    }
    while (may) {
        int n = (int)send(p->s, (const char*)p->out, (int)(may > 65536 ? 65536 : may), 0);
        if (n < 0) { if (sock_err_again()) break; return -1; }
        memmove(p->out, p->out + n, p->out_len - (size_t)n); p->out_len -= (size_t)n;
        may -= (size_t)n; p->sent += (uint64_t)n;
    }
    for (;;) {
        if (p->in_len + 65536 > p->in_cap) { p->in_cap = p->in_len + 65536 * 2; p->in = realloc(p->in, p->in_cap); }
        int n = (int)recv(p->s, (char*)p->in + p->in_len, 65536, 0);
        if (n == 0) return -1;
        if (n < 0) { if (sock_err_again()) break; return -1; }
        p->in_len += (size_t)n;
        p->heard = now_ms();
    }
    return 0;
}
/* the next whole message, or 0 */
static int peer_msg(Peer* p, int* type, Rd* r) {
    if (p->in_len < 5) return 0;
    uint32_t n = (uint32_t)p->in[0] | (uint32_t)p->in[1] << 8 | (uint32_t)p->in[2] << 16 | (uint32_t)p->in[3] << 24;
    if (p->in_len < 5 + (size_t)n) return 0;
    *type = p->in[4];
    r->d = p->in + 5; r->n = n; r->pos = 0; r->bad = 0;
    return 1;
}
static void peer_consume(Peer* p, const Rd* r) {
    size_t used = 5 + r->n;
    memmove(p->in, p->in + used, p->in_len - used); p->in_len -= used;
}

/* ---------------------------------------------------------------- the console data */
static void my_console(Console* c) {
    snprintf(c->name, sizeof c->name, "%s", g_name);
    uint32_t n;
    const uint8_t* s = backup_image(&n);
    c->save = malloc(n); memcpy(c->save, s, n); c->save_len = n;
    const uint8_t* f = firmware_image(&n);
    c->fw = malloc(n); memcpy(c->fw, f, n); c->fw_len = n;
}
static void put_console(Buf* b, const Console* c) {
    b_str(b, c->name, 32);
    b_u32(b, c->save_len); b_put(b, c->save, c->save_len);
    b_u32(b, c->fw_len); b_put(b, c->fw, c->fw_len);
}
static void get_console(Rd* r, Console* c) {
    r_str(r, c->name, 32);
    c->save = r_blob(r, &c->save_len);
    c->fw = r_blob(r, &c->fw_len);
}

/* ---------------------------------------------------------------- the lobby (host) */
static void host_send_lobby(void) {
    Buf b = { 0 };
    b_u8(&b, (uint8_t)g_n);
    for (int k = 0; k < g_n; k++) { b_str(&b, g_con[k].name, 32); b_u32(&b, (uint32_t)g_ping[k]); }
    for (int i = 1; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK && g_peer[i].hello) send_msg(&g_peer[i], M_LOBBY, &b);
    free(b.d);
}

static void host_accept(void) {
    if (g_listen == BADSOCK) return;
    for (;;) {
        struct sockaddr_storage from; socklen_t fl = sizeof from;
        sock_t s = accept(g_listen, (struct sockaddr*)&from, &fl);
        if (s == BADSOCK) return;
        { char t[80]; flog("someone connected from %s", addr_text((struct sockaddr*)&from, t, sizeof t)); }
        sock_nonblock(s);
        int slot = -1;
        for (int i = 1; i < SESSION_MAX; i++) if (g_peer[i].s == BADSOCK) { slot = i; break; }
        if (slot < 0 || g_state != 2) {
            Peer tmp; memset(&tmp, 0, sizeof tmp); tmp.s = s; Buf b = { 0 }; b_str(&b, slot < 0 ? "The session is full (4 players)." : "The session has already started.", 96);
            send_msg(&tmp, M_REFUSE, &b); peer_io(&tmp); free(b.d); free(tmp.out); free(tmp.in); closesock(s);
            continue;
        }
        memset(&g_peer[slot], 0, sizeof g_peer[slot]);
        g_peer[slot].s = s; g_peer[slot].index = -1;
    }
}

static void host_hello(Peer* p, Rd* r) {
    char magic[9] = { 0 }; const uint8_t* m = r_get(r, 8); if (m) memcpy(magic, m, 8);
    char ver[32]; r_str(r, ver, 32);
    Console c = { 0 };
    get_console(r, &c);
    uint64_t theirs = r->pos + 8 <= r->n ? r_u64(r) : 0;
    flog("they say hello: %s, version %s, build %016llx%s", r->bad ? "(unreadable)" : c.name, ver, (unsigned long long)theirs,
         strcmp(magic, MAGIC) ? " (not a DQIX session program)" : "");
    Buf b = { 0 };
    if (r->bad || strcmp(magic, MAGIC)) { b_str(&b, "That isn't a Dragon Quest IX PC session.", 96); send_msg(p, M_REFUSE, &b); free(b.d); peer_io(p); peer_close(p); return; }
    if (strcmp(ver, DQIX_VERSION)) {
        char t[96]; snprintf(t, sizeof t, "Different versions: the host has %s, you have %s. Both need the same.", DQIX_VERSION, ver);
        b_str(&b, t, 96); send_msg(p, M_REFUSE, &b); free(b.d); peer_io(p); peer_close(p);
        news("%s tried to join with a different version (%s)", c.name, ver);
        return;
    }
    lock();
    int k = g_n++;
    g_con[k] = c; p->index = k; p->hello = 1; g_ping[k] = 0;
    unlock();
    b_u8(&b, (uint8_t)k); b_u64(&b, build_id());
    send_msg(p, M_WELCOME, &b); free(b.d);
    news("%s joined (player %d)", c.name, k + 1);
    g_build[k] = theirs;
    host_send_lobby();
}

/* ---------------------------------------------------------------- starting */
static int compute_delay(void) {
    const char* e = getenv("DQIX_DELAY");
    if (e) return atoi(e) < 1 ? 1 : atoi(e) > 30 ? 30 : atoi(e);
    int worst = 0;
    for (int k = 0; k < g_n; k++) if (g_ping[k] > worst) worst = g_ping[k];
    /* a press reaches the furthest PC in about one round trip (via the host), plus a margin */
    int d = (int)((worst + 25) / 16.7) + 2;
    return d < 3 ? 3 : d > 20 ? 20 : d;
}

static void write_file(const char* path, const uint8_t* d, uint32_t n) {
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}

/* the hidden consoles, to notice one that stops by itself (a crash) */
#ifdef _WIN32
static HANDLE g_child[SESSION_MAX];
#else
static pid_t g_child[SESSION_MAX];
#endif
static int g_child_dead[SESSION_MAX];

#ifdef _WIN32
static HANDLE g_job;
/* our environment, one "NAME=value" per entry */
static char** env_list(void) {
    static char** list;
    if (list) return list;
    char* b = GetEnvironmentStringsA();
    int n = 0; for (char* q = b; *q; q += strlen(q) + 1) n++;
    list = calloc((size_t)n + 1, sizeof(char*));
    int i = 0; for (char* q = b; *q; q += strlen(q) + 1) list[i++] = q;
    return list;
}
#else
extern char** environ;
static char** env_list(void) { return environ; }
#endif

/* another player's console on this PC: the same program, no window */
static void spawn_hidden(int k) {
    char save[300], fw[300], log[300];
    snprintf(save, sizeof save, "session/console%d.sav", k + 1);
    snprintf(fw, sizeof fw, "session/console%d.fw", k + 1);
    snprintf(log, sizeof log, "session/console%d.log", k + 1);
    write_file(save, g_con[k].save, g_con[k].save_len);
    write_file(fw, g_con[k].fw, g_con[k].fw_len);
    /* environment: ours without DQIX_* (tests' scripts, dumps), plus the session's */
    char extra[9][200];
    snprintf(extra[0], 200, "DQIX_HIDDEN=1");
    snprintf(extra[1], 200, "DQIX_SESSION_SHM=%s", g_shm);
    snprintf(extra[2], 200, "DQIX_CONSOLE=%d", k);
    snprintf(extra[3], 200, "DQIX_COUNT=%d", g_n);
    snprintf(extra[4], 200, "DQIX_RTC=%lld", (long long)g_rtc);
    snprintf(extra[5], 200, "DQIX_FW_FILE=%s", fw);
    snprintf(extra[6], 200, "DQIX_UTC_OFFSET=%lld", (long long)g_utc_off);
    snprintf(extra[7], 200, "DQIX_NOWINDOW=1");
    snprintf(extra[8], 200, "DQIX_NO_SETTINGS=1");
    char** cur = env_list();
    int ne = 0; for (char** e = cur; *e; e++) ne++;
    char** env = calloc((size_t)ne + 10, sizeof(char*));
    int m = 0;
    for (char** e = cur; *e; e++) if (strncmp(*e, "DQIX_", 5) && **e != '=') env[m++] = *e;
    for (int i = 0; i < 9; i++) env[m++] = extra[i];
    env[m] = NULL;
#ifdef _WIN32
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char cmd[4096]; snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" \"%s\" 0 1 \"%s\"", exe, g_rom_path, g_extract_path, save);
    size_t total = 1; for (int i = 0; i < m; i++) total += strlen(env[i]) + 1;
    char* block = malloc(total); char* q = block;
    for (int i = 0; i < m; i++) { size_t l = strlen(env[i]) + 1; memcpy(q, env[i], l); q += l; }
    *q = 0;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE hlog = CreateFileA(log, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = hlog; si.hStdError = hlog; si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    if (!g_job) {
        g_job = CreateJobObjectA(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, block, NULL, &si, &pi)) {
        AssignProcessToJobObject(g_job, pi.hProcess);
        SetPriorityClass(pi.hProcess, ABOVE_NORMAL_PRIORITY_CLASS);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        g_child[k] = pi.hProcess;
    } else news("could not start player %d's console (error %lu)", k + 1, GetLastError());
    if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
    free(block);
#else
    pid_t pid = fork();
    if (pid == 0) {
        FILE* f = freopen(log, "w", stderr); (void)f;
        f = freopen(log, "a", stdout); (void)f;
        char self[1024]; rt_self_exe(self, sizeof self);
        char* args[] = { self, g_rom_path, g_extract_path, (char*)"0", (char*)"1", save, NULL };
        execve(self, args, env);
        _exit(127);
    }
    g_child[k] = pid;
#endif
    free(env);
}

/* ---------------------------------------------------------------- the others' consoles on one thread
 * With the game as a library next to the program (dqixcore.dll / .so), the
 * other players' DSes run here, in this process, on one thread, each from
 * its own copy of the library (its own memory), taking turns whenever one
 * has to wait (hosting.c). A console only waits for microseconds at a time
 * while the DSes talk, so turns between threads or processes would cost
 * more than the work; turns between fibers cost almost nothing. Without the
 * library the consoles are separate processes (spawn_hidden). */
#ifdef _WIN32
typedef HMODULE LibHandle;
#define LIB_EXT ".dll"
static LibHandle lib_open(const char* p) { return LoadLibraryA(p); }
static void* lib_sym(LibHandle h, const char* n) { return (void*)GetProcAddress(h, n); }
#else
#include <dlfcn.h>
#include <ucontext.h>    /* macOS: build.mk defines _XOPEN_SOURCE for it */
typedef void* LibHandle;
#define LIB_EXT ".so"
static LibHandle lib_open(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void* lib_sym(LibHandle h, const char* n) { return dlsym(h, n); }
#endif

typedef struct {
    int k, dead, started;
    LibHandle lib;
    int (*core_main)(int, char**);
    CoreHost host;
    char* env[12];
    char envbuf[14][300];
    char* argv[8];
    char argbuf[3][300];
#ifdef _WIN32
    void* fiber; void* resume;
#else
    ucontext_t ctx; void* stack;
#endif
} Hosted;

static Hosted g_hosted[SESSION_MAX];
static int g_nhosted, g_hosted_cur;
static uint8_t* g_shared_rom; static size_t g_shared_rom_size;
#ifdef _WIN32
static void* g_sched_fiber;
#else
static ucontext_t g_sched_ctx;
#endif

static void hosted_yield(void) {
    Hosted* h = &g_hosted[g_hosted_cur];
#ifdef _WIN32
    h->resume = GetCurrentFiber();
    SwitchToFiber(g_sched_fiber);
#else
    swapcontext(&h->ctx, &g_sched_ctx);
#endif
}
static void hosted_exit(int code) {
    Hosted* h = &g_hosted[g_hosted_cur];
    h->dead = 1;
    LOG_WARN("session: player %d's console stopped (%d)\n", h->k + 1, code);
#ifdef _WIN32
    SwitchToFiber(g_sched_fiber);
#else
    setcontext(&g_sched_ctx);
#endif
    for (;;) {}
}
#ifdef _WIN32
static void WINAPI hosted_entry(void* p) {
#else
static void hosted_entry(void) {
    Hosted* p = &g_hosted[g_hosted_cur];
#endif
    Hosted* h = (Hosted*)p;
    int r = h->core_main(6, h->argv);
    hosted_exit(r);
}

static int self_dir(char* out, size_t n) {
#ifdef _WIN32
    char exe[MAX_PATH]; DWORD l = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (!l || l >= MAX_PATH) return 0;
#else
    char exe[1024]; if (rt_self_exe(exe, sizeof exe) <= 0) return 0;
#endif
    char* sl = strrchr(exe, '/');
#ifdef _WIN32
    char* bs = strrchr(exe, '\\'); if (bs > sl) sl = bs;
#endif
    if (!sl) return 0;
    *sl = 0;
    snprintf(out, n, "%s", exe);
    return 1;
}

static int copy_file(const char* from, const char* to) {
    FILE* a = fopen(from, "rb"); if (!a) return 0;
    FILE* b = fopen(to, "wb"); if (!b) { fclose(a); return 0; }
    static char buf[1 << 16]; size_t n; int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) if (fwrite(buf, 1, n, b) != n) { ok = 0; break; }
    fclose(a); if (fclose(b) != 0) ok = 0;
    return ok;
}

/* set up player k's console from its own copy of the library; 0 = not possible here */
static int host_console(int k) {
    char dir[1024], lib[1200], copy[300];
    if (getenv("DQIX_PROCESSES") || !self_dir(dir, sizeof dir)) return 0;
    snprintf(lib, sizeof lib, "%s/dqixcore" LIB_EXT, dir);
    FILE* t = fopen(lib, "rb"); if (!t) return 0; fclose(t);
    snprintf(copy, sizeof copy, "session/core%d" LIB_EXT, k + 1);
    if (!copy_file(lib, copy)) return 0;
#ifdef _WIN32
    char full[MAX_PATH]; GetFullPathNameA(copy, MAX_PATH, full, NULL);
#else
    char full[4096]; if (!realpath(copy, full)) return 0;
#endif
    Hosted* h = &g_hosted[g_nhosted];
    memset(h, 0, sizeof *h);
    h->k = k;
    h->lib = lib_open(full);
    if (!h->lib) { LOG_WARN("session: can't load %s\n", full); return 0; }
    void (*setup)(const CoreHost*) = (void (*)(const CoreHost*))lib_sym(h->lib, "dqix_core_setup");
    h->core_main = (int (*)(int, char**))lib_sym(h->lib, "dqix_core_main");
    if (!setup || !h->core_main) return 0;
    char save[300], fw[300];
    snprintf(save, sizeof save, "session/console%d.sav", k + 1);
    snprintf(fw, sizeof fw, "session/console%d.fw", k + 1);
    write_file(save, g_con[k].save, g_con[k].save_len);
    write_file(fw, g_con[k].fw, g_con[k].fw_len);
    int e = 0;
    snprintf(h->envbuf[e++], 300, "DQIX_HIDDEN=1");
    snprintf(h->envbuf[e++], 300, "DQIX_SESSION_SHM=%s", g_shm);
    snprintf(h->envbuf[e++], 300, "DQIX_CONSOLE=%d", k);
    snprintf(h->envbuf[e++], 300, "DQIX_COUNT=%d", g_n);
    snprintf(h->envbuf[e++], 300, "DQIX_RTC=%lld", (long long)g_rtc);
    snprintf(h->envbuf[e++], 300, "DQIX_FW_FILE=%s", fw);
    snprintf(h->envbuf[e++], 300, "DQIX_NO_SETTINGS=1");
    snprintf(h->envbuf[e++], 300, "DQIX_UTC_OFFSET=%lld", (long long)g_utc_off);
    snprintf(h->envbuf[e++], 300, "DQIX_NOWINDOW=1");
    if (getenv("DQIX_HIDDEN_DUMP") && getenv("DQIX_DUMP_AT")) {            /* tests: the hidden screens too */
        snprintf(h->envbuf[e++], 300, "DQIX_DUMP=%s%d,1", getenv("DQIX_HIDDEN_DUMP"), k + 1);
        snprintf(h->envbuf[e++], 300, "DQIX_DUMP_AT=%.280s", getenv("DQIX_DUMP_AT"));
    }
    for (int i = 0; i < e; i++) h->env[i] = h->envbuf[i];
    h->env[e] = NULL;
    h->host.env = (const char* const*)h->env;
    h->host.yield = hosted_yield;
    h->host.exit_console = hosted_exit;
    h->host.rom = g_shared_rom; h->host.rom_size = g_shared_rom_size;
    snprintf(h->argbuf[0], 300, "%s", save);
    h->argv[0] = (char*)"dqix"; h->argv[1] = g_rom_path; h->argv[2] = g_extract_path;
    h->argv[3] = (char*)"0"; h->argv[4] = (char*)"1"; h->argv[5] = h->argbuf[0]; h->argv[6] = NULL;
    setup(&h->host);
    g_nhosted++;
    return 1;
}

/* the thread the others' consoles take turns on */
#ifdef _WIN32
static DWORD WINAPI hosted_thread(LPVOID u)
#else
static void* hosted_thread(void* u)
#endif
{
    (void)u;
#ifdef _WIN32
    g_sched_fiber = ConvertThreadToFiber(NULL);
    for (int i = 0; i < g_nhosted; i++)       /* the game's own code wants a deep stack (64 MB, reserved, not used) */
        g_hosted[i].fiber = CreateFiberEx(64 * 1024, 64 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, hosted_entry, &g_hosted[i]);
#else
    for (int i = 0; i < g_nhosted; i++) {
        Hosted* h = &g_hosted[i];
        getcontext(&h->ctx);
        h->stack = malloc(64u * 1024 * 1024);
        h->ctx.uc_stack.ss_sp = h->stack; h->ctx.uc_stack.ss_size = 64u * 1024 * 1024; h->ctx.uc_link = NULL;
        makecontext(&h->ctx, hosted_entry, 0);
    }
#endif
    unsigned idle = 0; double idle_since = 0;
    for (;;) {
        uint64_t before = air_progress();
        int alive = 0;
        for (int i = 0; i < g_nhosted; i++) {
            Hosted* h = &g_hosted[i];
            if (h->dead) continue;
            alive++;
            g_hosted_cur = i;
#ifdef _WIN32
            SwitchToFiber(h->resume ? h->resume : h->fiber);
#else
            swapcontext(&g_sched_ctx, &h->ctx);
#endif
        }
        if (!alive) break;
        /* nothing moved: everyone waits for this PC's player or the network */
        if (air_progress() != before) { idle = 0; continue; }
        if (++idle < 64) continue;
        if (idle == 64) idle_since = now_ms();
        if (now_ms() - idle_since < 2.0) {
#ifdef _WIN32
            SwitchToThread();
#else
            sched_yield();
#endif
        } else sleep_ms(1);
    }
    return 0;
}

static int start_hosted(void) {
    if (!g_nhosted) return 0;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 1 << 20, hosted_thread, NULL, 0, NULL);
    if (!t) return 0;
    SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
#else
    pthread_t t; if (pthread_create(&t, NULL, hosted_thread, NULL) != 0) return 0;
#endif
    return 1;
}

#ifndef _WIN32
static void kill_children(void) { signal(SIGTERM, SIG_IGN); kill(0, SIGTERM); }
#endif

static void get_wfc(Rd* r);
static void put_wfc(Buf* b);
static void do_start(void) {
    flog_open("starting");
    {
        flog("I am player %d of %d (%s), input delay %d", g_me + 1, g_n, g_host ? "hosting" : "joined", g_delay);
        for (int k = 0; k < g_n; k++)
            flog("player %d: %s, ping %d ms%s", k + 1, g_con[k].name, g_ping[k],
                 k == g_me || !g_build[k] ? "" : g_build[k] == build_id() ? ", same build" : ", a different build (compiled separately: usually fine)");
    }
    snprintf(g_shm, sizeof g_shm, "%08x%04x", (unsigned)(g_rtc ^ (int64_t)(now_ms() * 1000)), (unsigned)rand() & 0xFFFF);
    if (!g_host) { /* the host chose the name */ }
    rtc_set_base(g_rtc);
    rtc_set_utc_offset(g_utc_off);
    if (!air_init(g_shm, g_me, g_n)) { news("could not set up the shared memory"); g_state = 5; return; }
    for (int k = 0; k < g_n; k++) air_set_name(k, g_con[k].name);
    get_wfc(&g_wfc_rd);
    free(g_wfc_buf); g_wfc_buf = NULL; memset(&g_wfc_rd, 0, sizeof g_wfc_rd);
    for (int k = 0; k < g_n; k++) for (int f = 0; f < g_delay; f++) air_input_put(k, (uint64_t)f, 0);
#ifndef _WIN32
    setpgid(0, 0); atexit(kill_children);
#endif
    /* the other players' consoles: on one thread here if the game is also a library next to us, else processes */
    { extern uint8_t* g_rom; extern size_t g_rom_size; g_shared_rom = g_rom; g_shared_rom_size = g_rom_size; }
    int hosted = 1;
    for (int k = 0; k < g_n && hosted; k++) if (k != g_me && !host_console(k)) hosted = 0;
    if (hosted && start_hosted()) LOG_WARN("session: the other players' DSes run on one thread (dqixcore)\n");
    else { g_nhosted = 0; for (int k = 0; k < g_n; k++) if (k != g_me) spawn_hidden(k); }
    g_running = 1;
    lock(); g_state = 3; unlock();
    news("started: %d players, input delay %d frames (%d ms)", g_n, g_delay, (int)(g_delay * 16.7));
}

static void host_start(void) {
    g_delay = compute_delay();
    Buf b = { 0 };
    g_utc_off = rtc_utc_offset();
    b_u8(&b, (uint8_t)g_n); b_u8(&b, (uint8_t)g_delay); b_u64(&b, (uint64_t)g_rtc); b_u64(&b, (uint64_t)g_utc_off);
    char shm[64]; snprintf(shm, sizeof shm, "%s", ""); (void)shm;
    for (int k = 0; k < g_n; k++) put_console(&b, &g_con[k]);
    size_t wfc_at = b.n;
    put_wfc(&b);
    for (int i = 1; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK && g_peer[i].hello) send_msg(&g_peer[i], M_START, &b);
    { Rd own = { b.d + wfc_at, b.n - wfc_at, 0, 0 }; g_wfc_rd = own; g_wfc_buf = b.d; }   /* the same files here (after the log opens) */
    /* push it out before the game starts */
    double t0 = now_ms();
    for (;;) {
        int pending = 0;
        for (int i = 1; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK) { if (peer_io(&g_peer[i]) < 0) peer_close(&g_peer[i]); else if (g_peer[i].out_len) pending = 1; }
        if (!pending || now_ms() - t0 > 20000) break;
        sleep_ms(2);
    }
    do_start();
}

/* ---------------------------------------------------------------- running */
static void console_left(int k, uint64_t last) {
    if (k < 0 || k >= g_n || g_gone[k]) return;
    g_gone[k] = 1;
    air_input_put(k, last, AIR_INPUT_END);
    news("%s left (their DS is switched off)", g_con[k].name);
}

static void check_children(void) {
    static double last;
    if (now_ms() - last < 500) return;
    last = now_ms();
    for (int k = 0; k < g_n; k++) {
        if (k == g_me || !g_child[k] || g_child_dead[k]) continue;
#ifdef _WIN32
        DWORD code = 0;
        int dead = GetExitCodeProcess(g_child[k], &code) && code != STILL_ACTIVE;
#else
        int st = 0, dead = waitpid(g_child[k], &st, WNOHANG) == g_child[k];
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
#endif
        if (!dead) continue;
        g_child_dead[k] = 1;
        air_set_gone(k);                                           /* nobody waits for it any more */
        if (!g_gone[k]) news("%s's DS stopped by itself (exit code %d, see session\\console%d.log)", g_con[k].name, (int)code, k + 1);
    }
}

static void check_hashes(void) {
    for (int i = 0; i < g_npending;) {
        uint64_t mine;
        if (air_hash_get(g_pending[i].k, g_pending[i].frame, &mine)) {
            static int matched; if (mine == g_pending[i].hash && ++matched % 10 == 1) LOG_INFO("session: in sync with %s at frame %llu (%d checks)\n", g_con[g_pending[i].k].name, (unsigned long long)g_pending[i].frame, matched);
            flog("sync with %s at frame %llu: %s", g_con[g_pending[i].k].name, (unsigned long long)g_pending[i].frame, mine == g_pending[i].hash ? "same" : "DIFFERENT");
            if (getenv("DQIX_SYNCLOG")) fprintf(stderr, "sync %d %llu %s\n", g_pending[i].k, (unsigned long long)g_pending[i].frame, mine == g_pending[i].hash ? "ok" : "MISMATCH");
            if (mine != g_pending[i].hash && !g_desync) {
                g_desync = 1;
                news("OUT OF SYNC with %s at frame %llu: the PCs no longer agree. Leave and start a new session.",
                     g_con[g_pending[i].k].name, (unsigned long long)g_pending[i].frame);
            }
            g_pending[i] = g_pending[--g_npending];
        } else i++;
    }
}

static void got_input(int from_peer, Rd* r) {
    int k = r_u8(r);
    uint64_t first = r_u32(r);
    uint32_t n = r_u32(r);
    if (r->bad || k < 0 || k >= g_n || k == g_me) return;
    for (uint32_t i = 0; i < n; i++) { uint32_t v = r_u32(r); if (r->bad) return; air_input_put(k, first + i, v); }
    if (g_host) {                                                  /* pass it on to the others */
        Buf b = { 0 }; b_put(&b, r->d, r->n);
        for (int i = 1; i < SESSION_MAX; i++) if (i != from_peer && g_peer[i].s != BADSOCK && g_peer[i].hello) send_msg(&g_peer[i], M_INPUT, &b);
        free(b.d);
    }
}

static void got_hash(int from_peer, Rd* r) {
    int k = r_u8(r); uint64_t frame = r_u64(r), h = r_u64(r);
    if (r->bad || k < 0 || k >= g_n) return;
    if (k != g_me && g_npending < 64) { g_pending[g_npending].k = k; g_pending[g_npending].frame = frame; g_pending[g_npending].hash = h; g_npending++; }
    if (g_host) {
        Buf b = { 0 }; b_put(&b, r->d, r->n);
        for (int i = 1; i < SESSION_MAX; i++) if (i != from_peer && g_peer[i].s != BADSOCK && g_peer[i].hello) send_msg(&g_peer[i], M_HASH, &b);
        free(b.d);
    }
}

/* my new inputs and fingerprints go out */
static void send_mine(void) {
    uint64_t have = air_input_count(g_me);
    if (have > g_sent_frames) {
        Buf b = { 0 };
        uint32_t n = (uint32_t)(have - g_sent_frames);
        if (n > 4096) n = 4096;
        b_u8(&b, (uint8_t)g_me); b_u32(&b, (uint32_t)g_sent_frames); b_u32(&b, n);
        for (uint32_t i = 0; i < n; i++) { uint32_t v = 0; air_input_peek(g_me, g_sent_frames + i, &v); b_u32(&b, v); }
        for (int i = 0; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK && (g_host ? i > 0 && g_peer[i].hello : i == 0)) send_msg(&g_peer[i], M_INPUT, &b);
        free(b.d);
        g_sent_frames += n;
    }
    uint64_t h;
    while (air_hash_get(g_me, g_hash_next, &h)) {
        Buf b = { 0 }; b_u8(&b, (uint8_t)g_me); b_u64(&b, g_hash_next); b_u64(&b, h);
        for (int i = 0; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK && (g_host ? i > 0 && g_peer[i].hello : i == 0)) send_msg(&g_peer[i], M_HASH, &b);
        free(b.d);
        g_hash_next += HASH_EVERY;
    }
}

/* every few seconds, and whenever this console has been stuck waiting, how far everyone is */
static void net_status(char* out, size_t n) {
    size_t o = 0; out[0] = 0;
    double t = now_ms();
    for (int i = 0; i < SESSION_MAX && o < n; i++) {
        Peer* p = &g_peer[i];
        if (p->s == BADSOCK || p->index < 0) continue;
        o += (size_t)snprintf(out + o, n - o, "%s%s heard %.1f s ago, %u bytes to send", o ? ", " : "",
                              g_host ? g_con[p->index].name : "host", p->heard ? (t - p->heard) / 1000.0 : -1.0, (unsigned)p->out_len);
    }
}
static void watch(void) {
    static double last, stuck_logged;
    double t = now_ms(), w = air_waiting_ms();
    int stuck = w > 1500;
    if (!stuck) stuck_logged = 0;
    if (!(stuck && (!stuck_logged || t - stuck_logged > 5000)) && t - last < 10000) return;
    last = t;
    char a[400], nets[400], d[300] = ""; int k;
    air_status(a, sizeof a); net_status(nets, sizeof nets);
    if (stuck) { air_wait_describe(d, sizeof d, &k); stuck_logged = t; flog("STUCK %.1f s: %s", w / 1000.0, d); }
    flog("consoles: %s", a);
    flog("network: %s", nets[0] ? nets : "nobody");
}

/* ---------------------------------------------------------------- the network thread */
static void host_reachability(void);
static void handle(int i, int type, Rd* r) {
    Peer* p = &g_peer[i];
    switch (type) {
    case M_HELLO: if (g_host && !p->hello) host_hello(p, r); break;
    case M_WELCOME: {
        g_me = r_u8(r);
        uint64_t theirs = r->pos + 8 <= r->n ? r_u64(r) : 0;
        lock(); g_state = 2; unlock();
        g_build[0] = theirs;
        news("connected: you are player %d. Waiting for the host to start.", g_me + 1);
        break;
    }
    case M_LOBBY: {
        int n = r_u8(r);
        lock();
        if (n >= 1 && n <= SESSION_MAX) { g_n = n; for (int k = 0; k < n; k++) { r_str(r, g_con[k].name, 32); g_ping[k] = (int)r_u32(r); } }
        unlock();
        break;
    }
    case M_START: {
        int n = r_u8(r), d = r_u8(r); int64_t rtc = (int64_t)r_u64(r); g_utc_off = (int64_t)r_u64(r);
        if (n < 1 || n > SESSION_MAX) break;
        lock();
        g_n = n; g_delay = d; g_rtc = rtc;
        for (int k = 0; k < n; k++) get_console(r, &g_con[k]);
        unlock();
        if (r->bad) { news("the start message was damaged"); g_state = 5; break; }
        g_wfc_rd = *r;
        do_start();
        break;
    }
    case M_INPUT: if (g_running) got_input(i, r); break;
    case M_HASH: if (g_running) got_hash(i, r); break;
    case M_LEAVE: { int k = r_u8(r); uint64_t last = r_u64(r); if (g_running) console_left(k, last); break; }
    case M_PING: { Buf b = { 0 }; b_put(&b, r->d, r->n); send_msg(p, M_PONG, &b); free(b.d); break; }
    case M_PONG: { double t = now_ms(); if (p->index > 0 && p->index < SESSION_MAX) { lock(); g_ping[p->index] = (int)(t - p->ping_sent); unlock(); } break; }
    case M_REFUSE: { char why[96]; r_str(r, why, 96); news("%s", why); lock(); g_state = 5; unlock(); peer_close(p); break; }
    }
}

static void peer_lost(int i) {
    Peer* p = &g_peer[i];
    int k = p->index;
    flog("connection %d (player %d) closed or broke (socket error %d)", i, k + 1, sock_errno());
    peer_close(p);
    if (g_host) {
        if (k <= 0) return;
        if (g_running) {
            uint64_t last = air_input_count(k);
            console_left(k, last);
            Buf b = { 0 }; b_u8(&b, (uint8_t)k); b_u64(&b, last);
            for (int j = 1; j < SESSION_MAX; j++) if (g_peer[j].s != BADSOCK && g_peer[j].hello) send_msg(&g_peer[j], M_LEAVE, &b);
            free(b.d);
        } else {                                                   /* left the lobby: the others move up */
            lock();
            for (int j = k; j + 1 < g_n; j++) { g_con[j] = g_con[j + 1]; g_ping[j] = g_ping[j + 1]; }
            g_n--;
            for (int j = 1; j < SESSION_MAX; j++) if (g_peer[j].index > k) g_peer[j].index--;
            unlock();
            news("a player left the lobby");
            /* tell everyone their new numbers */
            for (int j = 1; j < SESSION_MAX; j++) if (g_peer[j].s != BADSOCK && g_peer[j].hello) {
                Buf b = { 0 }; b_u8(&b, (uint8_t)g_peer[j].index); b_u64(&b, build_id()); send_msg(&g_peer[j], M_WELCOME, &b); free(b.d);
            }
            host_send_lobby();
        }
    } else {
        if (g_running) {
            for (int j = 0; j < g_n; j++) if (j != g_me) console_left(j, air_input_count(j));
            news("the host has gone: the session is over (your game goes on by itself)");
            lock(); g_state = 4; unlock();
        } else { news("lost the connection to the host"); lock(); g_state = 5; unlock(); }
    }
}

#ifdef _WIN32
static DWORD WINAPI net_thread(LPVOID u)
#else
static void* net_thread(void* u)
#endif
{
    (void)u;
    double last_ping = 0;
    if (g_host) host_reachability();
    for (;;) {
        if (g_host && g_state == 2) host_accept();
        if (g_host && g_state == 2 && now_ms() - last_ping > 500) {
            last_ping = now_ms();
            for (int i = 1; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK && g_peer[i].hello) { g_peer[i].ping_sent = last_ping; Buf b = { 0 }; b_u64(&b, 0); send_msg(&g_peer[i], M_PING, &b); free(b.d); }
            host_send_lobby();
        }
        if (g_host && g_state == 2 && (g_start_req || (g_auto_players && g_n >= g_auto_players))) { g_start_req = 0; host_start(); }
        if (g_running) { send_mine(); check_hashes(); check_children(); watch(); }
        for (int i = 0; i < SESSION_MAX; i++) {
            Peer* p = &g_peer[i];
            if (p->s == BADSOCK) continue;
            if (peer_io(p) < 0) { peer_lost(i); continue; }
            int type; Rd r;
            while (p->s != BADSOCK && peer_msg(p, &type, &r)) { handle(i, type, &r); if (p->s != BADSOCK) peer_consume(p, &r); }
            if (p->s != BADSOCK && peer_io(p) < 0) peer_lost(i);
        }
        if (g_cancel_req) break;
        sleep_ms(1);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---------------------------------------------------------------- reachability (host) */
/* this PC's global IPv6 address, if it has one (no packet is sent) */
static int ipv6_global(char* out, size_t n) {
    const char* t = getenv("DQIX_TEST_V6");                     /* tests: pretend this is our global IPv6 address */
    if (t) { if (!*t) return 0; snprintf(out, n, "%s", t); return 1; }
    sock_t s = socket(AF_INET6, SOCK_DGRAM, 0);
    if (s == BADSOCK) return 0;
    struct sockaddr_in6 to; memset(&to, 0, sizeof to);
    to.sin6_family = AF_INET6; to.sin6_port = htons(53);
    inet_pton(AF_INET6, "2001:4860:4860::8888", &to.sin6_addr);
    int ok = connect(s, (struct sockaddr*)&to, sizeof to) == 0;
    struct sockaddr_in6 me; socklen_t ml = sizeof me;
    if (ok) ok = getsockname(s, (struct sockaddr*)&me, &ml) == 0;
    closesock(s);
    if (!ok) return 0;
    if ((me.sin6_addr.s6_addr[0] & 0xE0) != 0x20) return 0;       /* global unicast is 2000::/3 */
    inet_ntop(AF_INET6, &me.sin6_addr, out, (socklen_t)n);
    return 1;
}

/* this PC's address on the home network (no packet is sent) */
static int ipv4_local(char* out, size_t n) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == BADSOCK) return 0;
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    int ok = connect(s, (struct sockaddr*)&to, sizeof to) == 0;
    struct sockaddr_in me; socklen_t ml = sizeof me;
    if (ok) ok = getsockname(s, (struct sockaddr*)&me, &ml) == 0;
    closesock(s);
    if (!ok || me.sin_addr.s_addr == 0) return 0;
    inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)n);
    return 1;
}

/* Letting friends in: ask the router to open the port, by UPnP, then PCP, then
 * NAT-PMP (portmap.c); with IPv6, ask it to open its firewall too (UPnP IGD 2
 * or PCP). The host window then says what worked and which address to give. */
#ifdef _WIN32
static DWORD WINAPI renew_thread(LPVOID u);
#else
static void* renew_thread(void* u);
#endif
static void host_reachability(void) {
    char ext[64] = "", lan[64] = "", why[256] = "", v6[80] = "", method[16] = "";
    int ext_port = g_port;
    int tried = !getenv("DQIX_NO_UPNP");
    int r = tried ? upnp_open(g_port, ext, sizeof ext, lan, sizeof lan, why, sizeof why) : 0;
    if (r) snprintf(method, sizeof method, "UPnP");
    char why_upnp[256]; snprintf(why_upnp, sizeof why_upnp, "%s", why);
    if (r != 1 && tried) {                                        /* UPnP off, missing or refused: the other two */
        char e2[64], m2[16], w2[256];
        int r2 = portmap_open(g_port, e2, sizeof e2, &ext_port, m2, sizeof m2, w2, sizeof w2);
        if (r2 == 1 || (r2 == 2 && !r)) { r = r2; snprintf(ext, sizeof ext, "%s", e2); snprintf(method, sizeof method, "%s", m2); snprintf(why, sizeof why, "%s", w2); }
        else if (!r) {                                            /* one plain reason */
            int upnp_silent = strstr(why_upnp, "no router answered") != NULL;
            int pmp_silent = strstr(w2, "doesn't answer") || strstr(w2, "couldn't");
            if (upnp_silent && pmp_silent) snprintf(why, sizeof why, "it doesn't answer UPnP, NAT-PMP or PCP; they may be switched off in its settings");
            else if (!upnp_silent) snprintf(why, sizeof why, "%s", why_upnp);
            else snprintf(why, sizeof why, "%s", w2);
        }
    }
    if (!lan[0]) ipv4_local(lan, sizeof lan);
    if (!tried) snprintf(why, sizeof why, "not tried");
    int has6 = ipv6_global(v6, sizeof v6), open6 = 0;
    char why6[256] = "";
    if (has6 && tried) {
        open6 = upnp_pinhole(v6, g_port, why6, sizeof why6);
        if (!open6) open6 = portmap_open6(v6, g_port, why6, sizeof why6);
    }
    char v6line[300] = "";
    if (has6) snprintf(v6line, sizeof v6line, open6 ? " With IPv6, friends can also use [%s]:%d."
                                                     : " With IPv6, friends can also try [%s]:%d (your router may block it).", v6, g_port);
    int direct = (r == 1 && ext[0]) || (has6 && open6);
    char direct_addr[160] = "";
    if (r == 1 && ext[0]) snprintf(direct_addr, sizeof direct_addr, "%s:%d", ext, ext_port);
    else if (has6 && open6) snprintf(direct_addr, sizeof direct_addr, "[%s]:%d", v6, g_port);
    char home[96]; snprintf(home, sizeof home, "%s:%d", lan[0] ? lan : "this PC", g_port);
    /* the relay: a code that works for everyone, whatever their internet (relay.h) */
    char code[16] = "", shown[24] = "", rwhy[256] = "";
    int relayed = 0;
    if (relay_url()[0] && !getenv("DQIX_NO_RELAY")) {
        relay_new_code(code, sizeof code);
        relayed = relay_host_start(code, g_port, rwhy, sizeof rwhy);
        if (relayed) relay_show_code(code, shown, sizeof shown);
    } else snprintf(rwhy, sizeof rwhy, "this version has no relay");
    lock();
    if (relayed) {
        g_reach = 2;
        snprintf(g_addr, sizeof g_addr, "%s", shown);
        if (direct)
            snprintf(g_hint, sizeof g_hint, "Friends choose Join a friend and type this code: it works on any internet. "
                     "Your router also lets friends in directly (%s): the address %s works too, with a little less delay. "
                     "Friends in the same house can use %s.", method[0] ? method : "IPv6", direct_addr, home);
        else
            snprintf(g_hint, sizeof g_hint, "Friends choose Join a friend and type this code. Your router doesn't let friends "
                     "connect directly (%s), so they come in through the relay: it works on any internet, with a little "
                     "more delay. Friends in the same house can use %s.", why, home);
    } else if (direct) {
        g_reach = 1;
        snprintf(g_addr, sizeof g_addr, "%s", direct_addr);
        if (r == 1 && ext[0])
            snprintf(g_hint, sizeof g_hint, "Your router opened the port by itself (%s): give friends the address above. "
                     "Friends in the same house use %s.%s", method, home, v6line);
        else
            snprintf(g_hint, sizeof g_hint, "This address is for friends whose internet has IPv6 (your router let it through). "
                     "For the others the router didn't open a port (%s): forward TCP port %d to this PC (%s), "
                     "or all join a free virtual LAN (Tailscale, ZeroTier or Radmin VPN) and use this PC's address there.",
                     why, g_port, lan[0] ? lan : "this PC");
    } else {
        g_reach = 3;
        if (has6) snprintf(g_addr, sizeof g_addr, "[%s]:%d", v6, g_port);
        else snprintf(g_addr, sizeof g_addr, "%s", home);
        snprintf(g_hint, sizeof g_hint, "Friends far away can't join yet: your router doesn't let them in (%s), and the relay "
                 "couldn't be used (%s). %s"
                 "To play with friends elsewhere: forward TCP port %d to this PC (%s) in your router, or all join a free "
                 "virtual LAN (Tailscale, ZeroTier or Radmin VPN) and use this PC's address there.",
                 why, rwhy, has6 ? "The address above works for friends with IPv6 if your router lets it through. "
                                 : "The address above only works for friends in the same house. ",
                 g_port, lan[0] ? lan : "this PC");
    }
    {   /* in the log and bug reports: what the router did */
        static char line[1200];
        snprintf(line, sizeof line, "session: reachable at %s: %s\n", g_addr, g_hint);
        LOG_WARN("%s", line);
        report_log_line(line);
        flog("reachable at %s: %s", g_addr, g_hint);
    }
    unlock();
    g_upnp_done = 1;
#ifdef _WIN32
    CreateThread(NULL, 0, renew_thread, NULL, 0, NULL);
#else
    { pthread_t t; if (pthread_create(&t, NULL, renew_thread, NULL) == 0) pthread_detach(t); }
#endif
}

static void close_ports(void) { upnp_close(); portmap_close(); }

/* Keeping the router's openings from running out (they last 2 hours), on a
 * thread of their own: a router slow to answer never holds up the game. */
#ifdef _WIN32
static DWORD WINAPI renew_thread(LPVOID u)
#else
static void* renew_thread(void* u)
#endif
{
    (void)u;
    const char* t = getenv("DQIX_TEST_RENEW_MS");              /* tests: check more often than once a minute */
    int tick = t ? atoi(t) : 60000;
    for (int minute = 1; ; minute++) {
        sleep_ms(tick);
        portmap_renew();                                          /* when half their time is up */
        if (minute % 30 == 0) upnp_renew();
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---------------------------------------------------------------- setup */
static int resolve(const char* host, int port, struct sockaddr_storage* out, socklen_t* len) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return 0;
    memcpy(out, res->ai_addr, res->ai_addrlen); *len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return 1;
}

static int start_host(int port) {
    sock_t s = socket(AF_INET6, SOCK_STREAM, 0);
    int v6 = s != BADSOCK;
    if (!v6) s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == BADSOCK) return 0;
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    if (v6) { int off = 0; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof off); }
    int ok;
    if (v6) { struct sockaddr_in6 a; memset(&a, 0, sizeof a); a.sin6_family = AF_INET6; a.sin6_port = htons((uint16_t)port); a.sin6_addr = in6addr_any; ok = bind(s, (struct sockaddr*)&a, sizeof a) == 0; }
    else { struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = INADDR_ANY; ok = bind(s, (struct sockaddr*)&a, sizeof a) == 0; }
    if (!ok || listen(s, 4) != 0) { closesock(s); return 0; }
    sock_nonblock(s);
    g_listen = s;
    return 1;
}

/* connect, giving up after `ms` (a blocked port otherwise takes 20 s or more); 0 = connected */
static int connect_within(sock_t s, const struct sockaddr* a, socklen_t al, int ms) {
    sock_nonblock(s);
    if (connect(s, a, al) == 0) return 0;
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK) return -1;
#else
    if (errno != EINPROGRESS) return -1;
#endif
    fd_set w, x; FD_ZERO(&w); FD_ZERO(&x); FD_SET(s, &w); FD_SET(s, &x);
    struct timeval tv = { ms / 1000, (long)(ms % 1000) * 1000 };
    int k = select((int)s + 1, NULL, &w, &x, &tv);
    if (k == 0) {
#ifdef _WIN32
        WSASetLastError(10060);
#else
        errno = 110;
#endif
        return -1;
    }
    int err = 0; socklen_t el = sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
    if (k < 0 || err) {
#ifdef _WIN32
        WSASetLastError(err ? err : WSAGetLastError());
#else
        errno = err ? err : errno;
#endif
        return -1;
    }
    return 0;
}

/* 10.x, 172.16-31.x, 192.168.x: an address inside one home network */
static int home_address(const char* host) {
    unsigned a, b;
    if (sscanf(host, "%u.%u.", &a, &b) != 2) return 0;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

static int start_join(const char* host, int port) {
    struct sockaddr_storage a; socklen_t al;
    if (!resolve(host, port, &a, &al)) { news("can't find %s", host); return 0; }
    { char t[80]; flog("connecting to %s ...", addr_text((struct sockaddr*)&a, t, sizeof t)); }
    sock_t s = socket(a.ss_family, SOCK_STREAM, 0);
    if (s == BADSOCK) { flog("no socket (error %d)", sock_errno()); return 0; }
    double t0 = now_ms();
    if (connect_within(s, (struct sockaddr*)&a, al, 10000) != 0) {
        int e = sock_errno();
        flog("connect failed after %.1f s, socket error %d (%s)", (now_ms() - t0) / 1000.0, e,
             e == 10060 || e == 110 ? "timed out: nothing answered - the host's port is closed or a firewall drops it" :
             e == 10061 || e == 111 ? "refused: the host's PC answered, but nothing listens on that port" :
             e == 10065 || e == 10051 || e == 113 || e == 101 ? "no route to that address" : "see the number");
        closesock(s);
        if (home_address(host)) news("can't connect to %s:%d: that is an address inside your friend's home network, which "
                                     "can't be reached from elsewhere. Ask your friend for the code their game shows.", host, port);
        else news("can't connect to %s:%d (is the host waiting, and its port open?)", host, port);
        return 0;
    }
    flog("connected after %.1f s; saying hello (version %s)", (now_ms() - t0) / 1000.0, DQIX_VERSION);
    sock_nonblock(s);
    memset(&g_peer[0], 0, sizeof g_peer[0]);
    g_peer[0].s = s; g_peer[0].index = 0;
    Console me = { 0 };
    my_console(&me);
    Buf b = { 0 };
    b_put(&b, MAGIC, 8); b_str(&b, DQIX_VERSION, 32); put_console(&b, &me); b_u64(&b, build_id());
    send_msg(&g_peer[0], M_HELLO, &b);
    free(b.d);
    return 1;
}

/* In a session every console uses the built-in Wi-Fi (DQVC, the downloads)
 * and nothing else: it answers from inside the program, in the game's own
 * time, from the same files on every PC (the host's, sent at the start), so
 * every PC computes the same. Real servers on the internet stay off: their
 * answers would arrive at different moments on different PCs. */
static void session_wireless(void) {
    g_wireless.online = 1;
    g_wireless.builtin = 1;
    snprintf(g_wireless.dns, sizeof g_wireless.dns, "10.64.0.2");
    net_seal(1);
}

/* The host's built-in Wi-Fi files (wfc/<game>/...: small) go to everyone in
 * the start message; every PC, the host's included, then uses that copy in
 * session/wfc. */
#define WFC_SEND_MAX (8 << 20)
static void put_wfc(Buf* b) {
    char games[8][64], files[256][64];
    int ng = list_dir("wfc", 1, games, 8);
    size_t count_at = b->n; b_u32(b, 0);
    uint32_t count = 0; size_t total = 0;
    for (int g = 0; g < ng; g++) {
        char dir[200]; snprintf(dir, sizeof dir, "wfc/%s", games[g]);
        int nf = list_dir(dir, 0, files, 256);
        for (int i = 0; i < nf; i++) {
            char path[300]; snprintf(path, sizeof path, "%s/%s", dir, files[i]);
            FILE* f = fopen(path, "rb"); if (!f) continue;
            fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
            if (len < 0 || total + (size_t)len > WFC_SEND_MAX) { fclose(f); continue; }
            uint8_t* d = malloc((size_t)len + 1);
            if (d && fread(d, 1, (size_t)len, f) == (size_t)len) {
                char name[128]; snprintf(name, sizeof name, "%s/%s", games[g], files[i]);
                b_str(b, name, 128); b_u32(b, (uint32_t)len); b_put(b, d, (size_t)len);
                count++; total += (size_t)len;
            }
            free(d); fclose(f);
        }
    }
    b->d[count_at] = (uint8_t)count; b->d[count_at + 1] = (uint8_t)(count >> 8);
    b->d[count_at + 2] = (uint8_t)(count >> 16); b->d[count_at + 3] = (uint8_t)(count >> 24);
}
static int safe_name(const char* n) {
    if (strstr(n, "..") || strchr(n, '\\') || strchr(n, ':') || n[0] == '/') return 0;
    const char* sl = strchr(n, '/'); return sl && !strchr(sl + 1, '/');   /* game/file only */
}
static void get_wfc(Rd* r) {
    /* start clean: no files left over from an earlier session */
    char games[8][64], files[256][64];
    make_dir("session"); make_dir("session/wfc");
    int ng = list_dir("session/wfc", 1, games, 8);
    for (int g = 0; g < ng; g++) {
        char dir[200]; snprintf(dir, sizeof dir, "session/wfc/%s", games[g]);
        int nf = list_dir(dir, 0, files, 256);
        for (int i = 0; i < nf; i++) { char p[300]; snprintf(p, sizeof p, "%s/%s", dir, files[i]); remove(p); }
    }
    uint32_t count = r->pos + 4 <= r->n ? r_u32(r) : 0, written = 0;
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        char name[128]; r_str(r, name, 128);
        uint32_t len; uint8_t* d = r_blob(r, &len);
        if (!d || !safe_name(name)) { free(d); continue; }
        char dir[200]; snprintf(dir, sizeof dir, "session/wfc/%.*s", (int)(strchr(name, '/') - name), name);
        make_dir(dir);
        char path[300]; snprintf(path, sizeof path, "session/wfc/%s", name);
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(d, 1, len, f); fclose(f); written++; }
        free(d);
    }
    flog("built-in Wi-Fi: %u files from the host", (unsigned)written);
}

void session_pre_boot(int argc, char** argv) {
    if (argc > 1) snprintf(g_rom_path, sizeof g_rom_path, "%s", argv[1]);
    if (argc > 2) snprintf(g_extract_path, sizeof g_extract_path, "%s", argv[2]);
    if (getenv("DQIX_HIDDEN")) { g_hidden = 1; session_wireless(); return; }
    if (getenv("DQIX_SESSION")) { g_requested = 1; session_wireless(); }
    const char* nm = getenv("DQIX_PLAYER_NAME");
    if (nm && *nm) snprintf(g_name, sizeof g_name, "%s", nm);
}

void session_boot(void) {
    for (int i = 0; i < SESSION_MAX; i++) g_peer[i].s = BADSOCK;
    if (g_hidden) {
        const char* shm = getenv("DQIX_SESSION_SHM");
        g_me = getenv("DQIX_CONSOLE") ? atoi(getenv("DQIX_CONSOLE")) : 0;
        g_n = getenv("DQIX_COUNT") ? atoi(getenv("DQIX_COUNT")) : 1;
        if (!shm || !air_init(shm, g_me, g_n)) { LOG_WARN("session: hidden console without a session\n"); rt_exit(3); }
        g_running = 1;
        return;
    }
    const char* e = getenv("DQIX_SESSION");
    if (!e) return;
    mx_init();
    if (!net_up()) { news("networking is not available"); return; }
    char mode[16] = { 0 }, a[128] = { 0 };
    int port = SESSION_PORT, players = 0;
    if (!strncmp(e, "host", 4)) {
        sscanf(e, "host:%d:%d", &port, &players);
        g_port = port;
        g_host = 1; g_me = 0; g_n = 1;
        my_console(&g_con[0]);
        g_rtc = rtc_base();
        g_auto_players = players;
        flog_open("hosting");
        if (!start_host(port)) { news("can't wait for players on port %d (is another game hosting?)", port); g_state = 5; return; }
        snprintf(g_addr, sizeof g_addr, "(finding your address...)");
        atexit(close_ports);
        g_state = 2;
        news("hosting on port %d: waiting for players", port);
    } else if (!strncmp(e, "join:", 5) && relay_parse_code(e + 5, a, sizeof a)) {
        /* join:DQ-XXXX-XXXX: through the relay (relay.h) */
        char code[16]; snprintf(code, sizeof code, "%s", a);
        relay_show_code(code, g_addr, sizeof g_addr);
        flog_open("joining with a code");
        g_state = 1;
        news("connecting with the code %s", g_addr);
        char why[256]; int lp = 0;
        if (!relay_join_start(code, &lp, why, sizeof why)) { news("%s", why); g_state = 5; }
        else if (!start_join("127.0.0.1", lp)) g_state = 5;
        else news("connected to the relay: waiting for the host to let you in");
    } else if (!strncmp(e, "join:", 5)) {
        /* join:address[:port]; an IPv6 address goes in [brackets] */
        const char* s = e + 5;
        if (*s == '[') { const char* c = strchr(s, ']'); if (c) { snprintf(a, sizeof a, "%.*s", (int)(c - s - 1), s + 1); if (c[1] == ':') port = atoi(c + 2); } }
        else if (strchr(s, ':') != strrchr(s, ':')) snprintf(a, sizeof a, "%s", s);   /* a bare IPv6 address (several colons): the usual port */
        else { const char* c = strrchr(s, ':'); if (c) { snprintf(a, sizeof a, "%.*s", (int)(c - s), s); port = atoi(c + 1); } else snprintf(a, sizeof a, "%s", s); }
        snprintf(g_addr, sizeof g_addr, "%s:%d", a, port);
        flog_open("joining");
        g_state = 1;
        news("connecting to %s", g_addr);
        if (home_address(a)) news("%s is an address inside one home network: it only works for players in that home. "
                                  "For a friend far away, ask them for the code their game shows.", a);
        if (!start_join(a, port)) { g_state = 5; }
    } else if (!strncmp(e, "local:", 6)) {
        /* tests: N consoles on this PC only, the others' buttons from DQIX_INPUT_1..3, each its own MAC */
        int n = atoi(e + 6); if (n < 2) n = 2; if (n > SESSION_MAX) n = SESSION_MAX;
        g_host = 1; g_me = 0; g_n = n; g_local = 1;
        g_rtc = rtc_base(); g_utc_off = rtc_utc_offset();
        for (int k = 0; k < n; k++) {
            my_console(&g_con[k]);
            snprintf(g_con[k].name, sizeof g_con[k].name, "Console %d", k + 1);
            if (k) { uint8_t mac[6]; memcpy(mac, &g_con[k].fw[0x36], 6); mac[5] ^= (uint8_t)(0x40 + k); firmware_image_set_mac(g_con[k].fw, mac); }
            /* tests: another player's save / console (DQIX_SAVE_1.., DQIX_FW_1..) */
            char nm[32]; const char* path;
            snprintf(nm, sizeof nm, "DQIX_SAVE_%d", k);
            if (k && (path = getenv(nm))) { FILE* f = fopen(path, "rb"); if (f) { g_con[k].save_len = (uint32_t)fread(g_con[k].save, 1, g_con[k].save_len, f); fclose(f); } }
            snprintf(nm, sizeof nm, "DQIX_FW_%d", k);
            if (k && (path = getenv(nm))) { FILE* f = fopen(path, "rb"); if (f) { size_t got = fread(g_con[k].fw, 1, g_con[k].fw_len, f); (void)got; fclose(f); } }
        }
        g_delay = 2;
        { Buf b = { 0 }; put_wfc(&b); Rd own = { b.d, b.n, 0, 0 }; g_wfc_rd = own; g_wfc_buf = b.d; }
        do_start();
        return;
    } else { snprintf(mode, sizeof mode, "%.15s", e); news("unknown DQIX_SESSION %s", mode); return; }
    if (g_state == 5) {
        if (!session_lobby_hook) exit(4);
        while (!g_cancel_req) session_lobby_hook();                /* shows the error until the player closes it */
        exit(0);
    }
#ifdef _WIN32
    CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
#else
    pthread_t t; pthread_create(&t, NULL, net_thread, NULL);
#endif
    while (g_state != 3) {
        if (g_cancel_req) exit(0);
        if (session_lobby_hook) session_lobby_hook();
        else { if (g_state == 5) exit(4); sleep_ms(10); }
    }
}

/* ---------------------------------------------------------------- per frame */
static uint64_t ram_hash(void) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint64_t* p = (const uint64_t*)g_main_ram;
    for (size_t i = 0; i < MAIN_RAM_SIZE / 8; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint32_t session_frame_input(uint64_t frame, uint32_t local) {
    if (!g_running) return local;
    if (frame && frame % HASH_EVERY == 0) air_hash_put(g_me, frame, ram_hash());
    if (!g_hidden) air_input_put(g_me, frame + (uint64_t)g_delay, local);
    if (g_local) { uint32_t input_script_for(int, uint64_t); for (int k = 1; k < g_n; k++) air_input_put(k, frame + (uint64_t)g_delay, input_script_for(k, frame)); }
    uint32_t v = air_input_get(g_me, frame);
    if (v == AIR_INPUT_END) { LOG_WARN("session: this console was switched off at frame %llu\n", (unsigned long long)frame); backup_flush(); rt_exit(0); }
    return v;
}

/* ---------------------------------------------------------------- for the window */
void session_info(SessionInfo* o) {
    memset(o, 0, sizeof *o);
    if (!g_requested) return;
    lock();
    o->state = g_state; o->host = g_host; o->me = g_me; o->n = g_n; o->delay = g_delay; o->desync = g_desync;
    for (int k = 0; k < g_n && k < SESSION_MAX; k++) { snprintf(o->names[k], 32, "%s", g_con[k].name); o->ping_ms[k] = g_ping[k]; o->gone[k] = g_gone[k]; }
    snprintf(o->address, sizeof o->address, "%s", g_addr);
    snprintf(o->message, sizeof o->message, "%s", g_msg);
    snprintf(o->hint, sizeof o->hint, "%s", g_hint);
    o->ready = !g_host || g_upnp_done;
    o->reach = g_reach;
    unlock();
}
void session_start_now(void) { if (g_host) g_start_req = 1; }
void session_cancel(void) { g_cancel_req = 1; }
void session_set_name(const char* name) { if (name && *name) snprintf(g_name, sizeof g_name, "%s", name); }
