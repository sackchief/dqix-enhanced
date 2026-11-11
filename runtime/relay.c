/* The relay client (relay.h): a WebSocket to the relay, and the bridge
 * between it and ordinary TCP connections on this PC.
 *
 * The WebSocket: on Windows, https:// goes through WinHTTP (TLS, proxies);
 * elsewhere through a small client of our own, with TLS from relay_tls.c
 * (OpenSSL, if the build has it). http:// (local tests) goes through the same
 * client without TLS, on every system.
 * Every message is [type, id, data...] (tools/relay/worker.js). */
#ifdef _WIN32
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602        /* WinHTTP WebSockets: Windows 8 and later */
#define _CRT_RAND_S                /* rand_s: random codes */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include "relay.h"
#include "relay_url.h"
#include "relay_tls.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
typedef SOCKET sock_t;
#define BADSOCK INVALID_SOCKET
#define closesock closesocket
typedef CRITICAL_SECTION mutex_t;
static void mx_init(mutex_t* m) { InitializeCriticalSection(m); }
static void mx_lock(mutex_t* m) { EnterCriticalSection(m); }
static void mx_unlock(mutex_t* m) { LeaveCriticalSection(m); }
static double now_ms(void) { return (double)GetTickCount64(); }
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
typedef DWORD (WINAPI *thread_fn)(LPVOID);
#define THREAD_RET DWORD WINAPI
#define THREAD_END return 0
static void spawn(thread_fn f, void* a) { HANDLE h = CreateThread(NULL, 0, f, a, 0, NULL); if (h) CloseHandle(h); }
static void rand_bytes(uint8_t* b, size_t n) { for (size_t i = 0; i < n; i++) { unsigned v; rand_s(&v); b[i] = (uint8_t)v; } }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
typedef int sock_t;
#define BADSOCK (-1)
#define closesock close
typedef pthread_mutex_t mutex_t;
static void mx_init(mutex_t* m) { pthread_mutex_init(m, NULL); }
static void mx_lock(mutex_t* m) { pthread_mutex_lock(m); }
static void mx_unlock(mutex_t* m) { pthread_mutex_unlock(m); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
typedef void* (*thread_fn)(void*);
#define THREAD_RET void*
#define THREAD_END return NULL
static void spawn(thread_fn f, void* a) { pthread_t t; if (pthread_create(&t, NULL, f, a) == 0) pthread_detach(t); }
static void rand_bytes(uint8_t* b, size_t n) {
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, n, f) != n) for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rand();
    if (f) fclose(f);
}
#endif

enum { RM_OPEN = 0, RM_DATA = 1, RM_CLOSE = 2, RM_OK = 3, RM_ERROR = 4, RM_KEEP = 5,
       RM_MULTI = 6 };   /* host -> relay: every friend's bytes in one message ([id, len lo, len hi, bytes]...): the relay counts each message it gets */
#define COALESCE_MS 30         /* gather a connection's bytes this long before sending (fewer relay messages) */
#define KEEPALIVE_MS 25000

static void (*g_log)(const char* line);
void relay_set_log(void (*fn)(const char* line)) { g_log = fn; }
static void rlog(const char* fmt, ...) {
    char line[600]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (g_log) g_log(line); else fprintf(stderr, "relay: %s\n", line);
}

const char* relay_url(void) {
    const char* e = getenv("DQIX_RELAY");
    return e ? e : DQIX_RELAY_URL;
}

/* ---------------------------------------------------------------- codes */
static const char ALPHA[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
void relay_new_code(char* out, size_t n) {
    uint8_t r[8]; rand_bytes(r, 8);
    char c[9]; for (int i = 0; i < 8; i++) c[i] = ALPHA[r[i] % (sizeof ALPHA - 1)];
    c[8] = 0; snprintf(out, n, "%s", c);
}
int relay_parse_code(const char* t, char* out, size_t n) {
    char c[16]; int k = 0;
    while (*t == ' ') t++;
    if ((t[0] == 'D' || t[0] == 'd') && (t[1] == 'Q' || t[1] == 'q') && (t[2] == '-' || t[2] == ' ')) t += 3;
    for (; *t; t++) {
        char ch = *t;
        if (ch == '-' || ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (!strchr(ALPHA, ch) || ch == 0) return 0;
        if (k >= 8) return 0;
        c[k++] = ch;
    }
    if (k != 8) return 0;
    c[8] = 0; snprintf(out, n, "%s", c);
    return 1;
}
void relay_show_code(const char* code, char* out, size_t n) { snprintf(out, n, "DQ-%.4s-%.4s", code, code + 4); }

/* ---------------------------------------------------------------- the WebSocket */
typedef struct {
#ifdef _WIN32
    HINTERNET ses, con, ws;            /* WinHTTP (https) */
#endif
    sock_t s;                          /* our own client (http, or https with OpenSSL) */
    TlsConn* tls;                      /* https: the bytes go through it */
    mutex_t send_mx;
    uint8_t* rbuf; size_t rlen, rcap;  /* our client: bytes read, not yet a frame */
} Ws;

static int send_all(sock_t s, const void* d, size_t n) {
    const char* p = (const char*)d;
    while (n) { int k = send(s, p, (int)(n > 65536 ? 65536 : n), 0); if (k <= 0) return 0; p += k; n -= (size_t)k; }
    return 1;
}

/* the WebSocket's bytes: straight on the socket, or through TLS */
static int ws_wr(Ws* w, const void* d, size_t n) {
    return w->tls ? tls_write(w->tls, d, n) : send_all(w->s, d, n);
}
static int ws_rd_some(Ws* w, void* d, size_t n) {              /* 1..n bytes, or <= 0 */
    return w->tls ? tls_read(w->tls, d, n) : (int)recv(w->s, (char*)d, (int)n, 0);
}
static int ws_rd(Ws* w, void* d, size_t n) {                   /* exactly n bytes */
    char* p = (char*)d;
    while (n) { int k = ws_rd_some(w, p, n); if (k <= 0) return 0; p += k; n -= (size_t)k; }
    return 1;
}

static int parse_url(const char* url, int* secure, char* host, size_t hn, int* port, char* path, size_t pn) {
    const char* p = url;
    if (!strncmp(p, "https://", 8) || !strncmp(p, "wss://", 6)) { *secure = 1; *port = 443; p = strchr(p, '/') + 2; }
    else if (!strncmp(p, "http://", 7) || !strncmp(p, "ws://", 5)) { *secure = 0; *port = 80; p = strchr(p, '/') + 2; }
    else return 0;
    const char* slash = strchr(p, '/');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    char hp[256]; if (hl >= sizeof hp) return 0;
    memcpy(hp, p, hl); hp[hl] = 0;
    char* colon = strrchr(hp, ':');
    if (colon && !strchr(hp, ']')) { *colon = 0; *port = atoi(colon + 1); }
    snprintf(host, hn, "%s", hp);
    snprintf(path, pn, "%s", slash ? slash : "/");
    return 1;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64(const uint8_t* d, int n, char* out) {
    int k = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out[k++] = B64[v >> 18 & 63]; out[k++] = B64[v >> 12 & 63];
        out[k++] = i + 1 < n ? B64[v >> 6 & 63] : '='; out[k++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[k] = 0;
}

static sock_t tcp_connect(const char* host, int port, char* why, size_t wn) {
    struct addrinfo hints, *res = NULL, *a;
    memset(&hints, 0, sizeof hints); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { snprintf(why, wn, "can't find the relay (%s)", host); return BADSOCK; }
    sock_t s = BADSOCK;
    for (a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == BADSOCK) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        closesock(s); s = BADSOCK;
    }
    freeaddrinfo(res);
    if (s == BADSOCK) snprintf(why, wn, "can't reach the relay (%s:%d)", host, port);
    else { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one); }
    return s;
}

static void ws_destroy(Ws* w) {
    if (w->tls) tls_free(w->tls);
    free(w);
}

static Ws* ws_open_sock(const char* host, int port, const char* path, int secure, char* why, size_t wn) {
    sock_t s = tcp_connect(host, port, why, wn);
    if (s == BADSOCK) return NULL;
    Ws* w = calloc(1, sizeof *w);
    w->s = s; mx_init(&w->send_mx);
    if (secure && !(w->tls = tls_connect((int)s, host, why, wn))) { closesock(s); ws_destroy(w); return NULL; }
    uint8_t key[16]; rand_bytes(key, 16); char k64[32]; b64(key, 16, k64);
    char req[1024];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s", path, host);
    if (!(secure && port == 443)) n += snprintf(req + n, sizeof req - (size_t)n, ":%d", port);
    n += snprintf(req + n, sizeof req - (size_t)n, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: DQIX-Enhanced\r\n\r\n", k64);
    if (!ws_wr(w, req, (size_t)n)) { closesock(s); ws_destroy(w); snprintf(why, wn, "the relay closed the connection"); return NULL; }
    char head[4096]; int hl = 0;
    while (hl < (int)sizeof head - 1) {
        if (!ws_rd(w, head + hl, 1)) { closesock(s); ws_destroy(w); snprintf(why, wn, "the relay closed the connection"); return NULL; }
        hl++;
        if (hl >= 4 && !memcmp(head + hl - 4, "\r\n\r\n", 4)) break;
    }
    head[hl] = 0;
    if (strncmp(head, "HTTP/1.1 101", 12)) {
        char* e = strchr(head, '\r'); if (e) *e = 0;
        snprintf(why, wn, "the relay answered \"%.60s\"", head); closesock(s); ws_destroy(w); return NULL;
    }
    return w;
}

#ifdef _WIN32
static Ws* ws_open_winhttp(const char* host, int port, const char* path, char* why, size_t wn) {
    wchar_t wh[256], wp[512];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wh, 256);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 512);
    HINTERNET ses = WinHttpOpen(L"DQIX-Enhanced", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { snprintf(why, wn, "WinHTTP isn't available (%lu)", GetLastError()); return NULL; }
    WinHttpSetTimeouts(ses, 10000, 10000, 10000, 0);       /* receive: no limit (a lobby can be quiet) */
    HINTERNET con = WinHttpConnect(ses, wh, (INTERNET_PORT)port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", wp, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    int ok = req && WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)
                 && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                 && WinHttpReceiveResponse(req, NULL);
    if (!ok) {
        DWORD e = GetLastError();
        snprintf(why, wn, e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "can't find the relay: is this PC online? (%lu)" :
                          e == ERROR_WINHTTP_TIMEOUT ? "the relay didn't answer (%lu)" : "can't reach the relay (WinHTTP error %lu)", e);
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return NULL;
    }
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    HINTERNET ws = status == 101 ? WinHttpWebSocketCompleteUpgrade(req, 0) : NULL;
    WinHttpCloseHandle(req);
    if (!ws) {
        snprintf(why, wn, status == 101 ? "the relay connection couldn't start (%lu)" : "the relay answered %lu", status == 101 ? GetLastError() : status);
        WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return NULL;
    }
    Ws* w = calloc(1, sizeof *w);
    w->ses = ses; w->con = con; w->ws = ws; w->s = BADSOCK; mx_init(&w->send_mx);
    return w;
}
#endif

static Ws* ws_open(const char* url, char* why, size_t wn) {
    int secure, port; char host[256], path[512];
    if (!parse_url(url, &secure, host, sizeof host, &port, path, sizeof path)) { snprintf(why, wn, "the relay's address isn't right (%s)", url); return NULL; }
#ifdef _WIN32
    if (secure) return ws_open_winhttp(host, port, path, why, wn);
#endif
    return ws_open_sock(host, port, path, secure, why, wn);
}

static int ws_send(Ws* w, const uint8_t* d, size_t n) {
    int ok;
    mx_lock(&w->send_mx);
#ifdef _WIN32
    if (w->ws) { ok = WinHttpWebSocketSend(w->ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (PVOID)d, (DWORD)n) == NO_ERROR; mx_unlock(&w->send_mx); return ok; }
#endif
    uint8_t h[14]; int hl = 0;
    h[hl++] = 0x82;                                            /* FIN, binary */
    if (n < 126) h[hl++] = (uint8_t)(0x80 | n);
    else if (n < 65536) { h[hl++] = 0x80 | 126; h[hl++] = (uint8_t)(n >> 8); h[hl++] = (uint8_t)n; }
    else { h[hl++] = 0x80 | 127; for (int i = 7; i >= 0; i--) h[hl++] = (uint8_t)((uint64_t)n >> (i * 8)); }
    uint8_t mask[4]; rand_bytes(mask, 4); memcpy(h + hl, mask, 4); hl += 4;
    uint8_t* m = malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) m[i] = d[i] ^ mask[i & 3];
    ok = ws_wr(w, h, (size_t)hl) && ws_wr(w, m, n);
    free(m);
    mx_unlock(&w->send_mx);
    return ok;
}

/* one whole message into *out (malloc'd); 0 = the connection ended */
static int ws_recv(Ws* w, uint8_t** out, size_t* len) {
#ifdef _WIN32
    if (w->ws) {
        size_t cap = 65536, n = 0; uint8_t* b = malloc(cap);
        for (;;) {
            if (cap - n < 16384) { cap *= 2; b = realloc(b, cap); }
            DWORD got = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE t;
            DWORD e = WinHttpWebSocketReceive(w->ws, b + n, (DWORD)(cap - n), &got, &t);
            if (e != NO_ERROR || t == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { free(b); return 0; }
            n += got;
            if (t == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || t == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) { *out = b; *len = n; return 1; }
        }
    }
#endif
    uint8_t* msg = NULL; size_t mlen = 0;
    for (;;) {
        uint8_t h[2];
        if (!ws_rd(w, h, 2)) { free(msg); return 0; }
        int op = h[0] & 15, fin = h[0] >> 7;
        uint64_t n = h[1] & 127;
        if (n >= 126) {
            uint8_t e[8]; int k = n == 126 ? 2 : 8;
            if (!ws_rd(w, e, (size_t)k)) { free(msg); return 0; }
            n = 0; for (int i = 0; i < k; i++) n = n << 8 | e[i];
        }
        uint8_t mask[4] = { 0 }; int masked = h[1] >> 7;
        if (masked && !ws_rd(w, mask, 4)) { free(msg); return 0; }
        if (n > (16u << 20)) { free(msg); return 0; }
        uint8_t* p = malloc(n ? (size_t)n : 1);
        size_t got = 0;
        while (got < n) { int k = ws_rd_some(w, p + got, (size_t)(n - got)); if (k <= 0) { free(p); free(msg); return 0; } got += (size_t)k; }
        if (masked) for (size_t i = 0; i < n; i++) p[i] ^= mask[i & 3];
        if (op == 8) { free(p); free(msg); return 0; }          /* close */
        if (op == 9) {                                         /* ping: pong */
            mx_lock(&w->send_mx);
            uint8_t ph[6] = { 0x8A, 0x80 }; rand_bytes(ph + 2, 4);
            ws_wr(w, ph, 6);
            mx_unlock(&w->send_mx);
            free(p); continue;
        }
        if (op == 10) { free(p); continue; }
        msg = realloc(msg, mlen + (size_t)n + 1);
        memcpy(msg + mlen, p, (size_t)n); mlen += (size_t)n; free(p);
        if (fin) { *out = msg; *len = mlen; return 1; }
    }
}

static void ws_close(Ws* w) {
#ifdef _WIN32
    if (w->ws) { WinHttpWebSocketClose(w->ws, 1000, NULL, 0); WinHttpCloseHandle(w->ws); WinHttpCloseHandle(w->con); WinHttpCloseHandle(w->ses); w->ws = NULL; return; }
#endif
    if (w->s != BADSOCK) { uint8_t c[6] = { 0x88, 0x80 }; rand_bytes(c + 2, 4); ws_wr(w, c, 6); closesock(w->s); w->s = BADSOCK; }
}

/* ---------------------------------------------------------------- the bridge */
#define MAXC 256
typedef struct {
    sock_t s;                     /* the TCP connection on this PC */
    int remote;                   /* the id the relay uses for it */
    uint8_t* buf; size_t len, cap;
    double first;                 /* when buf's oldest byte came */
    int closing;                  /* the relay said it ended: close it here */
} Conn;

static struct {
    Ws* ws;
    int host;                     /* 1 host, 0 friend */
    int port;                     /* host: the session's own port */
    sock_t listen;                /* friend: where the session connects */
    Conn c[MAXC];
    mutex_t mx;
    volatile int broken;
    double last_sent;
    int multi;                    /* the relay takes RM_MULTI (it said so in its RM_OK) */
} R;

int relay_broken(void) { return R.broken; }

static void send_msg(int type, int id, const uint8_t* d, size_t n) {
    uint8_t* m = malloc(n + 2);
    m[0] = (uint8_t)type; m[1] = (uint8_t)id;
    if (n) memcpy(m + 2, d, n);
    if (R.ws && !ws_send(R.ws, m, n + 2)) R.broken = 1;
    R.last_sent = now_ms();
    free(m);
}

static void flush(Conn* c) {
    if (!c->len) return;
    send_msg(RM_DATA, c->remote, c->buf, c->len);
    c->len = 0;
}
/* host: everything waiting for every friend, in one message (fewer messages: the
 * relay's daily allowance lasts longer; nothing waits longer than before) */
static void flush_all_host(void) {
    size_t total = 0; int n = 0;
    for (int i = 0; i < MAXC; i++) if (R.c[i].s != BADSOCK && R.c[i].len) { total += 3 * ((R.c[i].len + 65534) / 65535) + R.c[i].len; n++; }
    if (!n) return;
    if (n == 1) { for (int i = 0; i < MAXC; i++) if (R.c[i].s != BADSOCK && R.c[i].len) flush(&R.c[i]); return; }
    uint8_t* m = malloc(total + 2), *p = m + 2;
    m[0] = RM_MULTI; m[1] = 0;
    for (int i = 0; i < MAXC; i++) {
        Conn* c = &R.c[i];
        if (c->s == BADSOCK || !c->len) continue;
        size_t left = c->len; const uint8_t* d = c->buf;
        while (left) {                                            /* (pieces of at most 64 KB) */
            size_t k = left > 65535 ? 65535 : left;
            *p++ = (uint8_t)c->remote; *p++ = (uint8_t)(k & 255); *p++ = (uint8_t)(k >> 8);
            memcpy(p, d, k); p += k; d += k; left -= k;
        }
        c->len = 0;
    }
    if (R.ws && !ws_send(R.ws, m, (size_t)(p - m))) R.broken = 1;
    R.last_sent = now_ms();
    free(m);
}

static void conn_drop(int i) {
    Conn* c = &R.c[i];
    if (c->s != BADSOCK) closesock(c->s);
    c->s = BADSOCK; c->len = 0; c->closing = 0;
}

/* the relay -> this PC */
static THREAD_RET recv_thread(void* u) {
    (void)u;
    for (;;) {
        uint8_t* m; size_t n;
        if (!ws_recv(R.ws, &m, &n)) { rlog("the relay connection ended"); R.broken = 1; break; }
        if (n < 2) { free(m); continue; }
        int type = m[0], id = m[1];
        if (type == RM_OPEN && R.host) {                         /* a friend arrived: a connection to our own port for them */
            struct sockaddr_in a; memset(&a, 0, sizeof a);
            a.sin_family = AF_INET; a.sin_port = htons((uint16_t)R.port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            sock_t s = socket(AF_INET, SOCK_STREAM, 0);
            if (s == BADSOCK || connect(s, (struct sockaddr*)&a, sizeof a) != 0) {
                if (s != BADSOCK) closesock(s);
                rlog("friend %d arrived through the relay, but the game isn't listening on port %d", id, R.port);
                send_msg(RM_CLOSE, id, NULL, 0);
            } else {
                int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
                mx_lock(&R.mx);
                if (R.c[id].s != BADSOCK) conn_drop(id);
                R.c[id].s = s; R.c[id].remote = id; R.c[id].len = 0;
                mx_unlock(&R.mx);
                rlog("friend %d arrived through the relay", id);
            }
        } else if (type == RM_DATA) {
            int i = R.host ? id : 0;
            double t0 = now_ms();
            while (!R.host && R.c[0].s == BADSOCK && now_ms() - t0 < 5000) sleep_ms(2);   /* the session is about to connect */
            mx_lock(&R.mx); sock_t s = R.c[i].s; mx_unlock(&R.mx);
            if (s != BADSOCK && !send_all(s, m + 2, n - 2)) { mx_lock(&R.mx); R.c[i].closing = 1; mx_unlock(&R.mx); }
        } else if (type == RM_CLOSE) {
            int i = R.host ? id : 0;
            mx_lock(&R.mx); R.c[i].closing = 1; mx_unlock(&R.mx);
            rlog("connection %d ended at the relay", id);
        } else if (type == RM_ERROR) {
            rlog("the relay says: %.*s", (int)(n - 2), (const char*)m + 2);
        }
        free(m);
    }
    mx_lock(&R.mx);
    for (int i = 0; i < MAXC; i++) if (R.c[i].s != BADSOCK) R.c[i].closing = 1;
    mx_unlock(&R.mx);
    THREAD_END;
}

/* this PC -> the relay (and the friend's one local connection being accepted) */
static THREAD_RET pump_thread(void* u) {
    (void)u;
    for (;;) {
        fd_set rd; FD_ZERO(&rd);
        sock_t top = 0; int any = 0;
        double now = now_ms(), wait = 20;
        mx_lock(&R.mx);
        for (int i = 0; i < MAXC; i++) {
            Conn* c = &R.c[i];
            if (c->s == BADSOCK) continue;
            if (c->closing) { flush(c); conn_drop(i); continue; }
            if (c->len && now - c->first >= COALESCE_MS) { if (R.host && R.multi) flush_all_host(); else flush(c); }
            if (c->len) { double left = COALESCE_MS - (now - c->first); if (left < wait) wait = left; }
            FD_SET(c->s, &rd); if (c->s > top) top = c->s; any = 1;
        }
        mx_unlock(&R.mx);
        if (R.listen != BADSOCK) { FD_SET(R.listen, &rd); if (R.listen > top) top = R.listen; any = 1; }
        if (now - R.last_sent > KEEPALIVE_MS) send_msg(RM_KEEP, 0, NULL, 0);
        if (R.broken && !any) break;
        if (wait < 1) wait = 1;
        struct timeval tv = { 0, (long)(wait * 1000) };
        int k = any ? select((int)top + 1, &rd, NULL, NULL, &tv) : (sleep_ms((int)wait), 0);
        if (k <= 0) continue;
        if (R.listen != BADSOCK && FD_ISSET(R.listen, &rd)) {     /* the friend's session connects (once) */
            sock_t s = accept(R.listen, NULL, NULL);
            if (s != BADSOCK) {
                int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
                mx_lock(&R.mx); R.c[0].s = s; R.c[0].remote = 0; R.c[0].len = 0; mx_unlock(&R.mx);
                closesock(R.listen); R.listen = BADSOCK;
            }
        }
        mx_lock(&R.mx);
        for (int i = 0; i < MAXC; i++) {
            Conn* c = &R.c[i];
            if (c->s == BADSOCK || !FD_ISSET(c->s, &rd)) continue;
            if (c->cap - c->len < 16384) { c->cap = c->cap ? c->cap * 2 : 65536; c->buf = realloc(c->buf, c->cap); }
            int n = recv(c->s, (char*)c->buf + c->len, (int)(c->cap - c->len), 0);
            if (n <= 0) {                                         /* the game closed it */
                flush(c);
                send_msg(RM_CLOSE, c->remote, NULL, 0);
                conn_drop(i);
                if (!R.host) { rlog("the game left: closing the relay connection"); ws_close(R.ws); R.broken = 1; }
                continue;
            }
            if (!c->len) c->first = now_ms();
            c->len += (size_t)n;
            if (c->len >= 8192) { if (R.host && R.multi) flush_all_host(); else flush(c); }
        }
        mx_unlock(&R.mx);
    }
    THREAD_END;
}

static int begin(const char* code, int host, char* why, size_t wn, int* id) {
    static int once;
    if (!once) { once = 1; mx_init(&R.mx); for (int i = 0; i < MAXC; i++) R.c[i].s = BADSOCK; R.listen = BADSOCK; }
    const char* base = relay_url();
    if (!base || !base[0]) { snprintf(why, wn, "this version has no relay set up"); return 0; }
    size_t bl = strlen(base); while (bl && base[bl - 1] == '/') bl--;
    char url[700]; snprintf(url, sizeof url, "%.*s/room/%s?role=%s", (int)bl, base, code, host ? "host" : "join");
    double t0 = now_ms();
    Ws* w = ws_open(url, why, wn);
    if (!w) { rlog("relay: %s", why); return 0; }
    uint8_t* m; size_t n;
    if (!ws_recv(w, &m, &n)) { snprintf(why, wn, "the relay closed the connection"); ws_close(w); ws_destroy(w); return 0; }
    if (n >= 2 && m[0] == RM_ERROR) { snprintf(why, wn, "%.*s", (int)(n - 2), (const char*)m + 2); free(m); ws_close(w); ws_destroy(w); return 0; }
    if (n < 2 || m[0] != RM_OK) { snprintf(why, wn, "the relay answered something unexpected"); free(m); ws_close(w); ws_destroy(w); return 0; }
    *id = m[1];
    R.multi = n >= 3 && m[2] >= 1;                                 /* (a relay from before v34 sends no version) */
    free(m);
    R.ws = w; R.host = host; R.broken = 0; R.last_sent = now_ms();
    rlog("relay: connected as %s in %.1f s (%s)%s", host ? "the host" : "a friend", (now_ms() - t0) / 1000.0, base, R.multi ? ", bundled" : "");
    return 1;
}

int relay_host_start(const char* code, int port, char* why, size_t wn) {
    int id;
    if (!begin(code, 1, why, wn, &id)) return 0;
    R.port = port;
    spawn(recv_thread, NULL);
    spawn(pump_thread, NULL);
    return 1;
}

int relay_join_start(const char* code, int* port, char* why, size_t wn) {
    int id;
    if (!begin(code, 0, why, wn, &id)) return 0;
    sock_t l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    socklen_t al = sizeof a;
    if (l == BADSOCK || bind(l, (struct sockaddr*)&a, sizeof a) != 0 || listen(l, 1) != 0 || getsockname(l, (struct sockaddr*)&a, &al) != 0) {
        if (l != BADSOCK) closesock(l);
        snprintf(why, wn, "can't open a local port for the relay"); ws_close(R.ws); return 0;
    }
    R.listen = l;
    *port = ntohs(a.sin_port);
    rlog("relay: friend %d; the game connects through 127.0.0.1:%d", id, *port);
    spawn(recv_thread, NULL);
    spawn(pump_thread, NULL);
    return 1;
}
