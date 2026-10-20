/* The built-in Nintendo Wi-Fi Connection server (Wireless > Server > Built-in).
 *
 * Nintendo's servers are gone and the community ones no longer serve DQIX,
 * so the PC answers the game itself, offline:
 *
 *   DNS        every *.nintendowifi.net name -> WFC_IP (any other name: not found)
 *   conntest   http://conntest.nintendowifi.net/  -> 200 with X-Organization: Nintendo
 *   nas        /ac   login / svcloc (where the download server is) -> accepted
 *   dls1       /download  count / list / contents -> the files in wfc/<game code>/
 *
 * The files are the DQIX downloads as Nintendo served them (preserved by the
 * community: github.com/InternalLoss/DLS1-Files), listed in _list.txt as
 *   name \t \t attr1 \t attr2 \t attr3 \t size
 *
 * The game logs in over HTTPS with the DS's own SSL; like the community's
 * "NoSSL" patches, the login and download addresses in the game's network
 * library (overlay 31) are switched to http:// once it is in memory, so the
 * server speaks plain HTTP.
 *
 * TCP: the DS opens one short connection per request (HTTP/1.0). The frames
 * for WFC_IP never reach libslirp; a small responder here handles the
 * handshake, collects the request, sends the reply and closes. The link is
 * a memory queue, so retransmitting is only a safety net. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include "nds.h"

static const uint8_t WFC_IP[4] = { 192, 0, 2, 80 };   /* TEST-NET-1: never a real host */

/* the server's own dice: the same on every PC of a session (not the C library's, shared with the rest of the program) */
static uint32_t g_rand_state = 0x2545F491u;
static uint32_t wfc_rand(void) { g_rand_state = g_rand_state * 1103515245u + 12345u; return g_rand_state >> 1; }

static char g_dir[512] = "wfc";
static int g_on;
void wfc_enable(int on, const char* dir) {
    g_on = on;
    if (dir && dir[0]) snprintf(g_dir, sizeof g_dir, "%s", dir);
}
int wfc_enabled(void) { return g_on; }

static void ev(const char* fmt, ...) {
    char line[300];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    report_log_line(line);
    if (getenv("DQIX_NETLOG")) fprintf(stderr, "[f%llu] %s", (unsigned long long)g_frame, line);
}

/* ---------------------------------------------------------------- NoSSL
 * Every few frames, look through the overlay area for the network library's
 * https:// addresses (login, download) and make them http://. Only strings
 * naming Nintendo's servers or the download path are touched. */
void wfc_frame(void) {
    if (!g_on || g_frame % 8) return;
    for (uint32_t a = 0x200000; a < 0x300000; a += 4) {
        uint8_t* p = &g_main_ram[a];
        if (p[0] != 'h' || memcmp(p, "https://", 8)) continue;
        size_t n = strnlen((const char*)p, 80);
        if (n >= 80 || n < 10) continue;
        if (!strstr((const char*)p, "nintendowifi.net") && !strstr((const char*)p, "/download")) continue;
        memmove(p + 4, p + 5, n - 5);                    /* "https://x" -> "http://x" */
        p[n - 1] = 0;
        ev("wfc: %08x now %s (built-in server: no SSL)\n", 0x02000000u + a, (const char*)p);
    }
}

/* ---------------------------------------------------------------- frames */
static uint16_t cksum(const uint8_t* p, int n, uint32_t s) {
    for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)(p[n - 1] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
static void put16be(uint8_t* p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32be(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32be(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Ethernet + IPv4 header for a reply: swap the addresses of the frame q */
static int ip_reply(uint8_t* f, const uint8_t* q, int proto, int l4len, const uint8_t src_ip[4]) {
    memcpy(f, q + 6, 6); memcpy(f + 6, q, 6); f[12] = 8; f[13] = 0;
    uint8_t* ip = f + 14;
    static uint16_t id = 1;
    ip[0] = 0x45; ip[1] = 0; put16be(ip + 2, 20 + l4len); put16be(ip + 4, id++); ip[6] = 0x40; ip[7] = 0;
    ip[8] = 64; ip[9] = (uint8_t)proto; ip[10] = ip[11] = 0;
    memcpy(ip + 12, src_ip, 4); memcpy(ip + 16, q + 14 + 12, 4);
    put16be(ip + 10, cksum(ip, 20, 0));
    return 14 + 20 + l4len;
}
static uint32_t pseudo(const uint8_t* ip, int proto, int len) {
    uint32_t s = 0;
    for (int i = 12; i < 20; i += 2) s += (uint32_t)(ip[i] << 8 | ip[i + 1]);
    return s + (uint32_t)proto + (uint32_t)len;
}

/* ---------------------------------------------------------------- DNS */
static int dns(const uint8_t* q, int len) {
    const uint8_t* ip = q + 14; int ihl = (ip[0] & 15) * 4;
    const uint8_t* udp = ip + ihl; const uint8_t* d = udp + 8;
    int dn = len - 14 - ihl - 8;
    if (dn < 17) return 1;
    char name[128]; int o = 12, k = 0;
    while (o < dn && d[o] && k < 120) { int l = d[o++]; for (int i = 0; i < l && o < dn && k < 126; i++) name[k++] = (char)d[o++]; if (d[o]) name[k++] = '.'; }
    name[k] = 0;
    int qend = o + 5;                                       /* zero byte + type + class */
    if (qend > dn) return 1;
    int ours = strstr(name, "nintendowifi.net") != NULL;
    uint8_t f[600];
    uint8_t* r = f + 14 + 20 + 8;
    memcpy(r, d, (size_t)qend);
    r[2] = 0x81; r[3] = ours ? 0x80 : 0x83;                 /* response, recursion; NXDOMAIN if not ours */
    r[6] = 0; r[7] = ours ? 1 : 0; r[8] = r[9] = r[10] = r[11] = 0;
    int n = qend;
    if (ours) {
        static const uint8_t ans[12] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
        memcpy(r + n, ans, 12); memcpy(r + n + 12, WFC_IP, 4); n += 16;
    }
    uint8_t* u = f + 14 + 20;
    put16be(u, 53); put16be(u + 2, (unsigned)(udp[0] << 8 | udp[1])); put16be(u + 4, 8 + n); u[6] = u[7] = 0;
    int tot = ip_reply(f, q, 17, 8 + n, ip + 16);
    put16be(u + 6, cksum(u, 8 + n, pseudo(f + 14, 17, 8 + n)));
    net_deliver(f, tot);
    ev("wfc: %s -> %s\n", name, ours ? "built-in server" : "not found");
    return 1;
}

/* ---------------------------------------------------------------- DQVC
 * DQIX's download files are RC4-encrypted (key "XENLONPROJECTKEY", the game's
 * code name) and end with 4 bytes that make the whole encrypted file, read as
 * one big-endian number, divisible by 0xC2A030D4 (overlay 23, 021f6324).
 * Decrypted, a DQVC stock ("auction") file is
 *   u32 serial, u32 end of records, u32 27, u32 2
 *   records: u16 tag, u8 words, u8 type, words (u32 each)
 *     0x65 [0]  0x64 [20]  0x67 [2099 12 31] (valid until)
 *     0x68 [item, price, 1, quantity] per item: the week's pool; the game
 *          shows six of them, picked per player
 *   "YYYY/MM/DD hh:mm:ss\0" "YYMMDD\0", 0xFF to 32 bytes, then the 4 bytes
 *
 * wfc/<game>/dqvc.txt, when present, rotates the stock like the original
 * service (which ran the same 52 sales every year from July 2010 to 2014):
 *   week MM/DD <title>      a sale and the date it started in 2011-12
 *   <item id> <price> <quantity> <name...>
 * Each year's cycle starts on the Friday nearest 15 July, every sale keeps
 * its distance from that start (so they start on Fridays), and the stock
 * changes every day at 08:00 UTC: a share of the sale's items picked by the
 * date and the DS (its MAC address; DQVC_DAILY_ITEMS), from which the game
 * shows six. The DS's clock
 * (your PC's) decides which day it is.
 */
static void rc4(const uint8_t* key, int kl, uint8_t* d, int n) {
    uint8_t S[256]; int i, j = 0;
    for (i = 0; i < 256; i++) S[i] = (uint8_t)i;
    for (i = 0; i < 256; i++) { j = (j + S[i] + key[i % kl]) & 255; uint8_t t = S[i]; S[i] = S[j]; S[j] = t; }
    i = j = 0;
    for (int k = 0; k < n; k++) {
        i = (i + 1) & 255; j = (j + S[i]) & 255; uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
        d[k] ^= S[(S[i] + S[j]) & 255];
    }
}
/* encrypt plain[0..n) in place and append the check bytes; returns n + 4 */
static int seal(uint8_t* d, int n) {
    rc4((const uint8_t*)"XENLONPROJECTKEY", 16, d, n);
    const uint64_t M = 0xC2A030D4u;
    uint64_t h = 0;
    for (int i = 0; i < n; i++) h = ((h << 8) + d[i]) % M;
    for (int i = 0; i < 4; i++) h = (h << 8) % M;            /* h * 2^32 mod M */
    uint32_t x = (uint32_t)((M - h) % M);
    d[n] = (uint8_t)(x >> 24); d[n + 1] = (uint8_t)(x >> 16); d[n + 2] = (uint8_t)(x >> 8); d[n + 3] = (uint8_t)x;
    return n + 4;
}
static void put32le(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* days since 1970-01-01 for a date (proleptic Gregorian) */
static int64_t civil_days(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
static int civil_year(int64_t day) { int y = (int)(1970 + day / 366); while (civil_days(y + 1, 1, 1) <= day) y++; return y; }
/* the Friday nearest 15 July of year y (1970-01-01 was a Thursday) */
static int64_t cycle_start(int y) {
    int64_t d = civil_days(y, 7, 15);
    int dow = (int)(((d + 4) % 7 + 7) % 7);                 /* 0 Sunday .. 5 Friday */
    int delta = 5 - dow; if (delta > 3) delta -= 7; if (delta < -3) delta += 7;
    return d + delta;
}

static char* load_file(const char* path, int* size);
typedef struct { uint32_t id, price, qty; } StockItem;
#include "dqvc_schedule.h"
#define DQVC_DAILY_ITEMS 12             /* of the week's pool, per day */
/* The USA (YDQE) and European (YDQP) versions: the same downloads (the
 * quests and DQVC's items are numbers, the same in both); only the news
 * message is in its own file. */
static int builtin_game(const char* gamecd) { return !strcmp(gamecd, "YDQE") || !strcmp(gamecd, "YDQP"); }
static const char* news_file(const char* gamecd) { return !strcmp(gamecd, "YDQP") ? "infomes_45.bin" : "infomes_47.bin"; }

/* This week's stock for the game, built from dqvc.txt: a malloc'd file and
 * its name, or NULL (no schedule). */
/* The sale schedule: wfc/<game>/dqvc.txt if there is one (with the names,
 * for reading), else the one built in (numbers only, dqvc_schedule.h; USA's, used for Europe too). */
static int dqvc_schedule(const char* gamecd, DqvcWeek** weeks_out, DqvcItem** items_out, int* nweeks) {
    char p[700]; snprintf(p, sizeof p, "%s/%s/dqvc.txt", g_dir, gamecd);
    int n = 0; char* txt = load_file(p, &n);
    if (!txt) {
        if (!builtin_game(gamecd)) return 0;
        *weeks_out = (DqvcWeek*)DQVC_WEEKS; *items_out = (DqvcItem*)DQVC_ITEMS;
        *nweeks = (int)(sizeof DQVC_WEEKS / sizeof DQVC_WEEKS[0]);
        return 1;
    }
    DqvcWeek* w = calloc(128, sizeof *w); DqvcItem* it = calloc(4096, sizeof *it);
    int nw = 0, ni = 0;
    for (char* l = txt; l && *l; ) {
        char* e = strchr(l, '\n'); if (e) *e = 0;
        int mo, d; unsigned id, price, qty;
        if (!strncmp(l, "week", 4)) {
            if (nw < 128 && sscanf(l + 4, " %d/%d", &mo, &d) == 2) { w[nw].month = (uint8_t)mo; w[nw].day = (uint8_t)d; w[nw].first = (uint16_t)ni; w[nw].count = 0; nw++; }
        } else if (nw && ni < 4096 && sscanf(l, "%u %u %u", &id, &price, &qty) == 3 && id) {
            it[ni].id = (uint16_t)id; it[ni].price = price; it[ni].qty = (uint8_t)(qty ? qty : 1); ni++; w[nw - 1].count++;
        }
        l = e ? e + 1 : NULL;
    }
    free(txt);
    if (!nw) { free(w); free(it); return 0; }
    *weeks_out = w; *items_out = it; *nweeks = nw;
    return 2;                                               /* allocated */
}

/* Today's stock for the game: a malloc'd file and its name, or NULL (no schedule). */
static uint8_t* dqvc_week(const char* gamecd, char* name, int namecap, int* len, char* title, int titlecap) {
    DqvcWeek* sw; DqvcItem* si; int weeks;
    int src = dqvc_schedule(gamecd, &sw, &si, &weeks);
    if (!src) return NULL;
    /* each sale's start, as days after the first one (the dates are from the
     * 2011-12 cycle: July-December 2011, January-July 2012) */
    int starts[128]; starts[0] = -1;
    for (int i = 0; i < weeks && i < 128; i++) {
        int mo = sw[i].month, d = sw[i].day;
        starts[i] = mo >= 1 && mo <= 12 && d >= 1 && d <= 31 ? (int)(civil_days(mo >= 7 ? 2011 : 2012, mo, d) - civil_days(2011, 7, 15)) : -1;
    }
    if (weeks > 128) weeks = 128;
    if (starts[0] != 0) { if (src == 2) { free(sw); free(si); } return NULL; }
    /* Now, in UTC, with days starting at 08:00 UTC: when the original service
     * put out each day's stock. Each year's cycle starts on the Friday nearest
     * 15 July (the sales started on Fridays at 08:00 GMT). */
    time_t now = (time_t)rtc_base() + (time_t)(g_frame / 60);               /* the DS's clock (local time) */
    int64_t day = floor_div((int64_t)now - rtc_utc_offset() - 8 * 3600, 86400);   /* days since 1970, by 08:00 UTC */
    int y = civil_year(day);
    int64_t cycle = cycle_start(y);
    if (day < cycle) cycle = cycle_start(--y);
    int w = 0;
    for (int i = 0; i < weeks; i++) if (starts[i] >= 0 && starts[i] <= day - cycle) w = i;
    int64_t abs_week = (int64_t)(y - 2010) * weeks + w;
    int64_t abs_day = day - civil_days(2010, 7, 1);         /* a new stock file every day */
    /* the items of week w */
    StockItem items[200]; int ni = 0;
    snprintf(title, (size_t)titlecap, "sale %d", w + 1);
    for (int i = 0; i < sw[w].count && ni < 200; i++) {
        const DqvcItem* x = &si[sw[w].first + i];
        items[ni].id = x->id; items[ni].price = x->price; items[ni].qty = x->qty ? x->qty : 1; ni++;
    }
    if (src == 2) { free(sw); free(si); }
    if (!ni) return NULL;
    /* Each day's stock: the original changed daily within the week's theme
     * (which items it chose each day isn't recorded), so each day takes its
     * own share of the week's pool, picked by the date; the game then shows
     * six of them. */
    int full = ni;
    if (ni > DQVC_DAILY_ITEMS) {
        /* the day and this DS: every console gets its own share (the original's
         * stock was "generally different for each player"), the same all day.
         * The MAC is in the console's firmware, which a session sends to every
         * PC, so every PC picks the same for each player. */
        uint32_t fwlen; const uint8_t* fw = firmware_image(&fwlen);
        uint32_t mac = 2166136261u;
        for (int i = 0; i < 6; i++) mac = (mac ^ fw[0x36 + i]) * 16777619u;
        uint32_t r = 0x9E3779B9u ^ (uint32_t)abs_day * 2654435761u ^ mac;
        int pick[200]; for (int i = 0; i < ni; i++) pick[i] = i;
        for (int i = ni - 1; i > 0; i--) { r = r * 1103515245u + 12345u; int j = (int)((r >> 8) % (uint32_t)(i + 1)); int t = pick[i]; pick[i] = pick[j]; pick[j] = t; }
        int keep[200] = { 0 }; for (int i = 0; i < DQVC_DAILY_ITEMS; i++) keep[pick[i]] = 1;
        int o = 0; for (int i = 0; i < ni; i++) if (keep[i]) items[o++] = items[i];     /* in the list's order */
        ni = o;
    }
    int body = 16 + 8 + 8 + 16 + ni * 20;
    uint8_t* f = calloc(1, (size_t)body + 32 + 4);
    uint32_t serial = 1000 + (uint32_t)abs_day;            /* new every day, so the game takes it as new (it refuses numbers past 16 bits) */
    put32le(f, serial); put32le(f + 4, (uint32_t)body); put32le(f + 8, 27); put32le(f + 12, 2);
    uint8_t* q = f + 16;
    static const uint8_t t65[] = { 0x65, 0, 1, 0, 0, 0, 0, 0 }, t64[] = { 0x64, 0, 1, 0, 20, 0, 0, 0 };
    memcpy(q, t65, 8); q += 8; memcpy(q, t64, 8); q += 8;
    q[0] = 0x67; q[1] = 0; q[2] = 3; q[3] = 0x15; put32le(q + 4, 2099); put32le(q + 8, 12); put32le(q + 12, 31); q += 16;
    for (int i = 0; i < ni; i++) {
        q[0] = 0x68; q[1] = 0; q[2] = 4; q[3] = 0x55;
        put32le(q + 4, items[i].id); put32le(q + 8, items[i].price); put32le(q + 12, 1); put32le(q + 16, items[i].qty); q += 20;
    }
    { struct tm* g = gmtime(&now);
      char ds[40]; strftime(ds, sizeof ds, "%Y/%m/%d %H:%M:%S", g); memcpy(q, ds, 20);
      strftime(ds, sizeof ds, "%y%m%d", g); memcpy(q + 20, ds, 7);
      memset(q + 27, 0xFF, 5); }
    *len = seal(f, body + 32);
    snprintf(name, (size_t)namecap, "dqvc_d%05d.bin", (int)abs_day);
    ev("wfc: DQVC sale %d of %d (cycle week %lld), day %lld: %d of its %d items\n", w + 1, weeks, (long long)abs_week, (long long)abs_day, ni, full);
    return f;
}

/* ---------------------------------------------------------------- the downloadable quests
 * What the last quest download held (USA): which of the cartridge's
 * downloadable quests are switched on, in the order they came out, and the
 * order of the 23 special guests. Only numbers: the quests and guests
 * themselves are on the cartridge. The file built from these is the same,
 * byte for byte, as the one Nintendo served on 4 June 2010. */
static const uint8_t DLC_QUESTS[64] = {
    0x85, 0x86, 0xae, 0x91, 0xaf, 0x02, 0xb0, 0xb1, 0xb2, 0x95, 0xb3, 0x87, 0xb4, 0x89, 0xb5, 0x8a,
    0xb6, 0x9b, 0xb7, 0xa4, 0xb8, 0x7f, 0xb9, 0x9e, 0xba, 0xca, 0xbc, 0xbd, 0xbe, 0xbf, 0x8f, 0x7b,
    0xa1, 0x9c, 0xa5, 0xc0, 0xc1, 0x9a, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0x94, 0x98, 0x96, 0x92, 0x9d,
    0x99, 0xa2, 0x7a, 0xa0, 0x83, 0x97, 0xa8, 0x8b, 0xa6, 0xad, 0x8c, 0xa9, 0xaa, 0x9f, 0x84, 0x88 };
static const uint8_t DLC_GUESTS[23] = { 3, 19, 15, 4, 11, 1, 12, 21, 17, 8, 23, 16, 6, 5, 7, 20, 13, 2, 9, 10, 14, 18, 22 };
#define DLC_QUEST_FILE "quest52e.bin"

/* one record: tag, count, 2-bit types (4 in the header, then 16 per word), values */
static uint8_t* dls_record(uint8_t* q, int tag, const uint32_t* v, int n, int type) {
    q[0] = (uint8_t)tag; q[1] = 0; q[2] = (uint8_t)n; q[3] = 0;
    for (int i = 0; i < n && i < 4; i++) q[3] |= (uint8_t)(type << (2 * i));
    q += 4;
    int rest = n > 4 ? n - 4 : 0, words = (rest + 15) / 16;
    memset(q, 0xFF, (size_t)words * 4);
    for (int i = 0; i < rest; i++) { if (i % 4 == 0) q[i / 4] = 0; q[i / 4] |= (uint8_t)(type << (2 * (i % 4))); }
    q += words * 4;
    for (int i = 0; i < n; i++) { put32le(q, v[i]); q += 4; }
    return q;
}
static uint8_t* dlc_quest_file(int* len) {
    uint8_t* f = calloc(1, 1024);
    uint8_t* q = f + 16;
    uint32_t v[64];
    v[0] = 0; q = dls_record(q, 0x65, v, 1, 0);
    v[0] = 20; q = dls_record(q, 0x64, v, 1, 0);
    for (int i = 0; i < 64; i++) v[i] = DLC_QUESTS[i];
    q = dls_record(q, 0x6C, v, 64, 1);
    for (int i = 0; i < 23; i++) v[i] = DLC_GUESTS[i];
    q = dls_record(q, 0x6D, v, 23, 1);
    v[0] = 1; q = dls_record(q, 0x6E, v, 1, 1);
    int end = (int)(q - f); end = (end + 15) / 16 * 16;
    memset(q, 0xFF, (size_t)(f + end - q));
    put32le(f, 5); put32le(f + 4, (uint32_t)end); put32le(f + 8, 27); put32le(f + 12, 2);
    memcpy(f + end, "2010/06/04 17:42:30\0" "100604\0", 27);
    memset(f + end + 27, 0xFF, 5);
    *len = seal(f, end + 32);
    return f;
}

/* ---------------------------------------------------------------- HTTP */
static char* load_file(const char* path, int* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; *size = (int)n; }
    return b;
}

/* the DS's base64: '+' '/' '=' are sent as '.' '-' '*' */
static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '.' || c == '>') return 62;
    if (c == '/' || c == '-' || c == '?') return 63;
    return -1;
}
static int b64dec(const char* s, int n, char* out, int cap) {
    int o = 0, bits = 0; uint32_t acc = 0;
    for (int i = 0; i < n; i++) {
        int v = b64val((unsigned char)s[i]);
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (o < cap - 1) out[o++] = (char)(acc >> bits); }
    }
    out[o] = 0;
    return o;
}
static int b64enc(const char* s, char* out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int n = (int)strlen(s), o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(uint8_t)s[i] << 16 | (i + 1 < n ? (uint32_t)(uint8_t)s[i + 1] << 8 : 0) | (i + 2 < n ? (uint8_t)s[i + 2] : 0);
        out[o++] = T[v >> 18 & 63]; out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '*';
        out[o++] = i + 2 < n ? T[v & 63] : '*';
    }
    out[o] = 0;
    return o;
}
/* a form field, base64-decoded ("" if missing) */
static void field(const char* body, const char* key, char* out, int cap) {
    size_t kl = strlen(key);
    const char* p = body;
    out[0] = 0;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char* v = p + kl + 1; const char* e = strchr(v, '&');
            int n = e ? (int)(e - v) : (int)strlen(v);
            while (n > 0 && (v[n - 1] == '\r' || v[n - 1] == '\n')) n--;
            char raw[512]; int m = 0;
            for (int i = 0; i < n && m < 511; i++) {                 /* undo %xx */
                if (v[i] == '%' && i + 2 < n) { unsigned c; sscanf(v + i + 1, "%2x", &c); raw[m++] = (char)c; i += 2; }
                else raw[m++] = v[i];
            }
            b64dec(raw, m, out, cap);
            return;
        }
        p = strchr(p, '&'); if (p) p++;
    }
}
static void kv(char* out, const char* k, const char* v) {
    char e[512]; b64enc(v, e);
    if (out[0]) strcat(out, "&");
    strcat(out, k); strcat(out, "="); strcat(out, e);
}
static void now_str(char* s) {
    time_t t = (time_t)rtc_base() + (time_t)(g_frame / 60);   /* the DS's clock */
    struct tm* tm = gmtime(&t);
    strftime(s, 20, "%Y%m%d%H%M%S", tm);
}

/* the reply to one request; returns a malloc'd buffer */
static char* http(const char* req, int reqlen, int* outlen) {
    char method[8] = "", path[128] = "";
    sscanf(req, "%7s %127s", method, path);
    const char* body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : req + reqlen;
    char* out = malloc(4096 + 65536);
    char text[4096] = ""; const char* ctype = "text/plain"; const char* extra = "";
    char* bin = NULL; int binlen = 0, status = 200;
    char extrab[256] = "";

    if (!strcmp(method, "GET")) {                                  /* conntest */
        strcpy(text, "ok"); ctype = "text/html"; extra = "X-Organization: Nintendo\r\nServer: BigIP\r\n";
        ev("wfc: connection test answered\n");
    } else if (!strcmp(path, "/ac")) {                             /* nas */
        char action[32], svc[16], gamecd[8], userid[32];
        field(body, "action", action, sizeof action); field(body, "svc", svc, sizeof svc);
        field(body, "gamecd", gamecd, sizeof gamecd); field(body, "userid", userid, sizeof userid);
        for (char* c = action; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        char dt[20]; now_str(dt);
        char token[100] = "NDS";
        for (int i = 3; i < 99; i++) token[i] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"[(unsigned)wfc_rand() % 62];
        token[99] = 0;
        if (!strcmp(action, "acctcreate")) {
            kv(text, "retry", "0"); kv(text, "returncd", "002"); kv(text, "userid", userid[0] ? userid : "1000000000001");
        } else if (!strcmp(action, "login")) {
            kv(text, "retry", "0"); kv(text, "returncd", "001"); kv(text, "locator", "gamespy.com");
            kv(text, "challenge", "DQIXPC00"); kv(text, "token", token);
        } else {                                                   /* svcloc and anything else */
            kv(text, "retry", "0"); kv(text, "returncd", "007"); kv(text, "statusdata", "Y");
            kv(text, "svchost", "dls1.nintendowifi.net");
            kv(text, !strcmp(svc, "9000") ? "token" : "servicetoken", token);
        }
        kv(text, "datetime", dt);
        strcat(text, "\r\n");
        extra = "NODE: wifiappe1\r\n";
        ev("wfc: login (%s, game %s, service %s) accepted\n", action, gamecd, svc[0] ? svc : "-");
    } else if (!strcmp(path, "/download")) {                       /* dls1 */
        char action[16], gamecd[8], a1[32], a2[32], a3[32], name[64];
        field(body, "action", action, sizeof action); field(body, "gamecd", gamecd, sizeof gamecd);
        field(body, "attr1", a1, sizeof a1); field(body, "attr2", a2, sizeof a2); field(body, "attr3", a3, sizeof a3);
        field(body, "contents", name, sizeof name);
        for (char* c = action; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        int okcd = strlen(gamecd) == 4;
        for (int i = 0; i < 4 && okcd; i++) okcd = (gamecd[i] >= 'A' && gamecd[i] <= 'Z') || (gamecd[i] >= '0' && gamecd[i] <= '9');
        char p[700]; snprintf(p, sizeof p, "%s/%s/_list.txt", g_dir, okcd ? gamecd : "none");
        int ln = 0; char* list = okcd ? load_file(p, &ln) : NULL;
        if (!list && okcd && builtin_game(gamecd)) {
            /* no _list.txt: the quests and DQVC built in, and the news if the build fetched it */
            int ql = 0; free(dlc_quest_file(&ql));
            list = malloc(512);
            int o = snprintf(list, 512, DLC_QUEST_FILE "\t\tquest\t\t\t%d\r\nauction_0.bin\t\tauction\t\t\t0\r\n", ql);
            char np[700]; snprintf(np, sizeof np, "%s/%s/%s", g_dir, gamecd, news_file(gamecd));
            int nl = 0; char* news = load_file(np, &nl);
            if (news) { snprintf(list + o, (size_t)(512 - o), "%s\t\tmes\t\t\t%d\r\n", news_file(gamecd), nl); free(news); }
        }
        char hits[2048] = ""; int count = 0;
        char wname[64] = "", wtitle[128]; int wlen = 0;
        uint8_t* wk = okcd ? dqvc_week(gamecd, wname, sizeof wname, &wlen, wtitle, sizeof wtitle) : NULL;
        free(wk);
        char wline[160] = "";
        if (wk) snprintf(wline, sizeof wline, "%s\t\tauction\t\t\t%d", wname, wlen);
        for (char* line = list ? strtok(list, "\r\n") : NULL; line; line = strtok(NULL, "\r\n")) {
            if (wk && strstr(line, "\tauction\t")) line = wline;          /* this week's DQVC stock */
            char f[6][64] = { { 0 } }; int nf = 0; const char* s = line;
            while (nf < 6) { const char* t = strchr(s, '\t'); int l = t ? (int)(t - s) : (int)strlen(s); if (l > 63) l = 63;
                             memcpy(f[nf], s, (size_t)l); f[nf][l] = 0; nf++; if (!t) break; s = t + 1; }
            if (nf < 6) continue;
            if ((a1[0] && strcmp(a1, f[2])) || (a2[0] && strcmp(a2, f[3])) || (a3[0] && strcmp(a3, f[4]))) continue;
            count++;
            if (strlen(hits) + strlen(line) + 3 < sizeof hits) { strcat(hits, line); strcat(hits, "\r\n"); }
        }
        free(list);
        if (!strcmp(action, "count")) snprintf(text, sizeof text, "%d", count);
        else if (!strcmp(action, "list")) strcpy(text, count ? hits : "\r\n");
        else if (!strcmp(action, "contents")) {
            int okn = name[0] && !strstr(name, "..") && !strchr(name, '/') && !strchr(name, '\\');
            snprintf(p, sizeof p, "%s/%s/%s", g_dir, okcd ? gamecd : "none", name);
            if (okn && okcd && !strncmp(name, "dqvc_", 5)) {
                char wn[64], wt[128]; int wl = 0;
                bin = (char*)dqvc_week(gamecd, wn, sizeof wn, &wl, wt, sizeof wt);
                if (bin && strcmp(wn, name)) { free(bin); bin = NULL; }     /* a week that is over */
                binlen = wl;
            } else
            bin = okn && okcd ? load_file(p, &binlen) : NULL;
            if (!bin && okn && builtin_game(gamecd) && !strcmp(name, DLC_QUEST_FILE)) bin = (char*)dlc_quest_file(&binlen);   /* built in */
            if (bin) { ctype = "application/x-dsdl"; snprintf(extrab, sizeof extrab, "Content-Disposition: attachment; filename=\"%s\"\r\n", name); extra = extrab; }
            else status = 404;
        }
        extra = extra[0] ? extra : "X-DLS-Host: http://127.0.0.1/\r\n";
        if (!strcmp(action, "contents")) ev("wfc: download %s %s (%s)\n", gamecd, name, bin ? "sent" : "not found");
        else ev("wfc: download %s %s attr %s/%s/%s: %d file%s\n", gamecd, action, a1, a2, a3, count, count == 1 ? "" : "s");
        if (okcd && !count && !strcmp(action, "count") && !list) ev("wfc: no files in %s/%s (copy the game's files there)\n", g_dir, gamecd);
    } else {
        status = 404;
        ev("wfc: %s %s: not handled\n", method, path);
    }
    const char* payload = bin ? bin : text;
    int plen = bin ? binlen : (int)strlen(text);
    char date[64];
    { time_t t = (time_t)rtc_base() + (time_t)(g_frame / 60); strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&t)); }
    int h = snprintf(out, 4096, "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: Nintendo Wii (http)\r\nContent-Type: %s\r\n%sContent-Length: %d\r\nConnection: close\r\n\r\n",
                     status, status == 200 ? "OK" : "Not Found", date, ctype, extra, status == 200 ? plen : 0);
    if (status == 200 && plen <= 65536) { memcpy(out + h, payload, (size_t)plen); h += plen; }
    free(bin);
    *outlen = h;
    if (getenv("DQIX_WFCDEBUG")) {
        fprintf(stderr, "---- request\n%.*s\n---- decoded:", reqlen, req);
        for (const char* q = body; q && *q; ) {
            const char* e = strchr(q, '='); if (!e) break;
            char k[40] = ""; snprintf(k, sizeof k, "%.*s", (int)(e - q) < 39 ? (int)(e - q) : 39, q);
            char v[300]; field(body, k, v, sizeof v); fprintf(stderr, " %s=[%s]", k, v);
            q = strchr(e, '&'); if (q) q++;
        }
        fprintf(stderr, "\n---- reply\n%.*s\n----\n", h, out);
    }
    return out;
}

/* ---------------------------------------------------------------- TCP */
typedef struct {
    int used;
    uint8_t mac_ds[6], mac_gw[6], ip_ds[4];
    uint16_t port;                 /* the DS's port */
    uint32_t rcv_nxt, iss, snd_una, snd_nxt;
    uint16_t wnd;
    char req[8192]; int reqlen;
    char* resp; int resplen;       /* reply; bytes [0, resplen) at seq iss+1.. ; FIN after */
    int fin_in, fin_sent;
    int64_t last;                  /* frame of the last progress (retransmit) */
} Conn;
static Conn g_c[4];

static void tcp_send(Conn* c, uint32_t seq, int flags, const uint8_t* data, int n) {
    uint8_t f[1600];
    uint8_t q[34];                                  /* a fake request frame for ip_reply */
    memcpy(q, c->mac_gw, 6); memcpy(q + 6, c->mac_ds, 6); memcpy(q + 14 + 12, c->ip_ds, 4);
    int opt = (flags & 0x02) ? 4 : 0;               /* SYN: MSS */
    uint8_t* t = f + 14 + 20;
    put16be(t, 80); put16be(t + 2, c->port); put32be(t + 4, seq); put32be(t + 8, c->rcv_nxt);
    t[12] = (uint8_t)((20 + opt) / 4 << 4); t[13] = (uint8_t)flags; put16be(t + 14, 8192); t[16] = t[17] = 0; t[18] = t[19] = 0;
    if (opt) { t[20] = 2; t[21] = 4; put16be(t + 22, 1400); }
    if (n) memcpy(t + 20 + opt, data, (size_t)n);
    int l4 = 20 + opt + n;
    int tot = ip_reply(f, q, 6, l4, WFC_IP);
    memcpy(f, c->mac_ds, 6); memcpy(f + 6, c->mac_gw, 6);
    memcpy(f + 14 + 16, c->ip_ds, 4);
    f[14 + 10] = f[14 + 11] = 0; put16be(f + 14 + 10, cksum(f + 14, 20, 0));
    put16be(t + 16, cksum(t, l4, pseudo(f + 14, 6, l4)));
    net_deliver(f, tot);
}
/* send what the window allows, then FIN */
static void tcp_push(Conn* c) {
    if (!c->resp) return;
    uint32_t base = c->iss + 1;
    while (1) {
        int off = (int)(c->snd_nxt - base);
        int inflight = (int)(c->snd_nxt - c->snd_una);
        if (off < c->resplen) {
            int n = c->resplen - off; if (n > 1400) n = 1400;
            if (inflight + n > (c->wnd ? c->wnd : 1400)) break;
            tcp_send(c, c->snd_nxt, 0x18, (const uint8_t*)c->resp + off, n);
            c->snd_nxt += (uint32_t)n;
        } else {
            if (!c->fin_sent) { tcp_send(c, c->snd_nxt, 0x11, NULL, 0); c->snd_nxt++; c->fin_sent = 1; }
            break;
        }
    }
}

static int tcp(const uint8_t* q, int len) {
    const uint8_t* ip = q + 14; int ihl = (ip[0] & 15) * 4;
    const uint8_t* t = ip + ihl;
    int tl = (ip[2] << 8 | ip[3]) - ihl;
    if (tl < 20 || 14 + ihl + tl > len) return 1;
    uint16_t sport = (uint16_t)(t[0] << 8 | t[1]), dport = (uint16_t)(t[2] << 8 | t[3]);
    uint32_t seq = get32be(t + 4), ack = get32be(t + 8);
    int doff = (t[12] >> 4) * 4, flags = t[13];
    uint16_t wnd = (uint16_t)(t[14] << 8 | t[15]);
    const uint8_t* data = t + doff; int dl = tl - doff;
    Conn* c = NULL;
    for (int i = 0; i < 4; i++) if (g_c[i].used && g_c[i].port == sport) c = &g_c[i];

    if (flags & 0x04) { if (c) { free(c->resp); c->used = 0; } return 1; }       /* RST */
    if ((flags & 0x12) == 0x02) {                                                  /* SYN */
        if (dport != 80) {                                                        /* only HTTP: refuse */
            Conn tmp = { 0 }; memcpy(tmp.mac_ds, q + 6, 6); memcpy(tmp.mac_gw, q, 6); memcpy(tmp.ip_ds, ip + 12, 4);
            tmp.port = sport; tmp.rcv_nxt = seq + 1;
            tcp_send(&tmp, 0, 0x14, NULL, 0);
            ev("wfc: refused a connection to port %d\n", dport);
            return 1;
        }
        if (c && c->rcv_nxt == seq + 1) { tcp_send(c, c->iss, 0x12, NULL, 0); return 1; }   /* repeated SYN */
        if (c) { free(c->resp); c->used = 0; }
        for (int i = 0; i < 4 && !c; i++) if (!g_c[i].used) c = &g_c[i];
        if (!c) { c = &g_c[0]; free(c->resp); }
        memset(c, 0, sizeof *c);
        c->used = 1; memcpy(c->mac_ds, q + 6, 6); memcpy(c->mac_gw, q, 6); memcpy(c->ip_ds, ip + 12, 4);
        c->port = sport; c->rcv_nxt = seq + 1; c->iss = (uint32_t)wfc_rand() << 8; c->snd_una = c->iss; c->snd_nxt = c->iss + 1;
        c->wnd = wnd; c->last = (int64_t)g_frame;
        tcp_send(c, c->iss, 0x12, NULL, 0);
        return 1;
    }
    if (!c) { if (!(flags & 0x04)) { Conn tmp = { 0 }; memcpy(tmp.mac_ds, q + 6, 6); memcpy(tmp.mac_gw, q, 6); memcpy(tmp.ip_ds, ip + 12, 4);
                                     tmp.port = sport; tmp.rcv_nxt = seq + (uint32_t)dl; tcp_send(&tmp, ack, 0x04, NULL, 0); } return 1; }
    c->wnd = wnd;
    if (flags & 0x10) {                                                            /* ACK */
        if ((int32_t)(ack - c->snd_una) > 0 && (int32_t)(ack - c->snd_nxt) <= 0) { c->snd_una = ack; c->last = (int64_t)g_frame; }
    }
    if (dl > 0) {
        if (seq == c->rcv_nxt && c->reqlen + dl < (int)sizeof c->req) {
            memcpy(c->req + c->reqlen, data, (size_t)dl); c->reqlen += dl; c->rcv_nxt += (uint32_t)dl;
            c->req[c->reqlen] = 0;
        }
        tcp_send(c, c->snd_nxt, 0x10, NULL, 0);
        /* a whole request? headers, then Content-Length bytes */
        const char* hdr_end = strstr(c->req, "\r\n\r\n");
        if (hdr_end && !c->resp) {
            int need = 0;
            const char* cl = strstr(c->req, "Content-Length:"); if (!cl) cl = strstr(c->req, "content-length:");
            if (cl && cl < hdr_end) need = atoi(cl + 15);
            if (c->reqlen - (int)(hdr_end + 4 - c->req) >= need) {
                c->resp = http(c->req, c->reqlen, &c->resplen);
            }
        }
    }
    if ((flags & 0x01) && seq + (uint32_t)dl == c->rcv_nxt && !c->fin_in) {
        c->rcv_nxt++; c->fin_in = 1;
        tcp_send(c, c->snd_nxt, 0x10, NULL, 0);
    }
    tcp_push(c);
    if (c->fin_sent && c->fin_in && c->snd_una == c->snd_nxt) { free(c->resp); c->used = 0; }
    return 1;
}

/* resend anything unacknowledged for half a second (safety net) */
void wfc_poll(void) {
    for (int i = 0; i < 4; i++) {
        Conn* c = &g_c[i];
        if (!c->used) continue;
        if ((int64_t)g_frame - c->last > 600) { free(c->resp); c->used = 0; continue; }   /* gone quiet: forget it */
        if (c->snd_una != c->snd_nxt && (int64_t)g_frame - c->last > 30) {
            c->last = (int64_t)g_frame;
            if (c->snd_una == c->iss) { tcp_send(c, c->iss, 0x12, NULL, 0); continue; }
            c->snd_nxt = c->snd_una;
            if (c->fin_sent && (int)(c->snd_una - (c->iss + 1)) >= c->resplen) c->fin_sent = 0;
            tcp_push(c);
        }
    }
}

/* A frame from the DS: 1 if the built-in server took it. */
int wfc_input(const uint8_t* q, int len) {
    if (!g_on || len < 34 || q[12] != 8 || q[13] != 0) return 0;
    const uint8_t* ip = q + 14; int ihl = (ip[0] & 15) * 4;
    if (len < 14 + ihl + 8) return 0;
    int proto = ip[9];
    if (proto == 17 && (ip[ihl + 2] << 8 | ip[ihl + 3]) == 53) return dns(q, len);
    if (proto == 6 && !memcmp(ip + 16, WFC_IP, 4)) return tcp(q, len);
    return 0;
}
