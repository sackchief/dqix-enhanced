/* DQIX Enhanced (v33): a Cheats menu, patched into the exe.
 * Built position-independent into an added section; the exe's own functions
 * and globals are linked in by address (link.py). Functions the exe calls, or
 * that we call in the exe, use the Windows x64 convention (MS). */
#define MS __attribute__((ms_abi))
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned short wchar;

/* ---- the exe
 * Hidden visibility makes clang reach these with direct RIP-relative accesses.
 * Without it every one goes through a linker-made GOT holding absolute addresses
 * that nothing rebases, so under ASLR the first menu call crashes the game. */
#pragma GCC visibility push(hidden)
extern u8 g_main_ram[];                /* the DS's 4 MB main memory */
extern void* g_win;                     /* SDL_Window* */
extern void* g_bar;                     /* the menu bar (HMENU) */
extern char g_notice[160];              /* the title bar's short message */
extern u64 g_notice_until, g_perf_freq;
extern void* IMP_CreatePopupMenu, *IMP_AppendMenuW, *IMP_DrawMenuBar, *IMP_fopen, *IMP_fclose,
           *IMP_fgets, *IMP_fwrite, *IMP_free, *IMP_SDL_GetPerformanceCounter, *IMP_MessageBoxW;
extern MS void menu_check(int id, int on);
extern MS char* ask_text(void* win, const char* title, const char* prompt);
extern MS void orig_first_strike(u32* cpu);          /* fn_ov017_021970a0 */
/* the exe's own profile of function entries (dispatch.c prof_add, normally DQIX_PROF): a 65536-entry hash
 * of ARM address -> times entered, counted while state is 1 and from <= frame <= to */
extern MS u64 overlays_loaded_mask(void);            /* dispatch.c: bit n = overlay n is in memory (safe to ask any time) */
extern u32 g_prof_addr[65536], g_prof_cnt[65536];
extern int g_prof_state;
extern u64 g_prof_from, g_prof_to;
extern u32 g_arm9[41];                                /* the ARM9's Cpu: r0..r15, n z c v q, cpsr_ctl, fault, banked r13/r14/spsr */
extern u32 g_cyc;                                     /* the ARM9 cycles not yet turned into emulated time */
extern u8 g_dtcm[];                                   /* the ARM9's 16 KB DTCM, at g_dtcm_base (0x027E0000) */
extern u32 g_dtcm_base;
extern MS void fn_main_02083e28(u32* cpu);           /* the game's RecalcStats(r0 char, r1 0) */
/* the battle buff setters (r0 current stats, r1 levels to add; each keeps the level in -2..2): attack,
 * defence, agility, charm, magical might, magical mending, magic resistance, breath resistance */
extern MS void fn_main_020877c0(u32*), fn_main_020878b4(u32*), fn_main_020879a8(u32*), fn_main_02087a9c(u32*),
               fn_main_02087b90(u32*), fn_main_02087c84(u32*), fn_main_02087d78(u32*), fn_main_02087e6c(u32*);
#pragma GCC visibility pop

typedef MS void* (*fn_v)(void);
typedef MS int (*fn_append)(void* menu, u32 flags, u64 id, const wchar* text);
typedef MS int (*fn_p)(void*);
typedef MS void* (*fn_fopen)(const char*, const char*);
typedef MS char* (*fn_fgets)(char*, int, void*);
typedef MS u64 (*fn_fwrite)(const void*, u64, u64, void*);
typedef MS u64 (*fn_u64)(void);
typedef MS int (*fn_msgbox)(void*, const wchar*, const wchar*, u32);
#define CALL(type, imp) ((type)(imp))

#include "skills.h"                     /* the spells and abilities (gen_skills.py) */
typedef long long s64;
#define PKJ_ID 242                      /* mon_list.gp2: platinum king jewel */
#define GEM_ID 185                      /* ... and gem slime */
#include "items.h"                      /* who can equip what, and what it gives (gen_items.py, from the ROM) */

/* ---- small helpers (no C library here) */
void* memset(void* d, int c, u64 n) { u8* p = d; while (n--) *p++ = (u8)c; return d; }
void* memcpy(void* d, const void* s, u64 n) { u8* p = d; const u8* q = s; while (n--) *p++ = *q++; return d; }
static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static char* sput(char* o, const char* s) { while (*s) *o++ = *s++; *o = 0; return o; }
static char* iput(char* o, int v) {
    char t[12]; int n = 0; u32 u = v < 0 ? (u32)-v : (u32)v;
    if (v < 0) *o++ = '-';
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) *o++ = t[--n];
    *o = 0; return o;
}
static int sparse(const char* s, int* out) {               /* a whole number, spaces allowed around it */
    int v = 0, any = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = 1; if (v > 100000000) v = 100000000; }
    while (*s == ' ' || *s == '\r' || *s == '\n') s++;
    if (!any || *s) return 0;
    *out = v; return 1;
}
static int starts(const char* s, const char* p) { while (*p) if (*s++ != *p++) return 0; return 1; }

/* ---- the DS's memory */
static int valid(u32 a) { return (a >> 22) == (0x02000000u >> 22); }
static u8* ram(u32 a) { return g_main_ram + ((a - 0x02000000u) & 0x3FFFFFu); }
static u32 r32(u32 a) { u8* p = ram(a); return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 r16(u32 a) { u8* p = ram(a); return (u16)(p[0] | p[1] << 8); }
static u8 r8(u32 a) { return *ram(a); }
static void w32(u32 a, u32 v) { u8* p = ram(a); p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void w16(u32 a, u16 v) { u8* p = ram(a); p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void w8(u32 a, u8 v) { *ram(a) = v; }

/* GameState (0x020F33D8): +8 GameObject* objects[0xE9]. A GameObject's first
 * halfword holds flags (0x800 party member, 0x80 in a battle), +0x134 its base
 * stats (their PrimaryCombatStats at +0x2C), +0x138 its current stats (at +0).
 * PrimaryCombatStats: HP, MP, max HP, max MP, attack, defence, agility,
 * deftness (halfwords), then charm, magical might, magical mending (10 bits each). */
#define GAMESTATE 0x020F33D8u
#define N_OBJ 0xE9
static u32 object(int i) { u32 o = r32(GAMESTATE + 8 + 4 * i); return valid(o) ? o : 0; }
static u32 base_primary(u32 o) { u32 b = r32(o + 0x134); return valid(b) ? b + 0x2C : 0; }
static u32 cur_primary(u32 o) { u32 c = r32(o + 0x138); return valid(c) ? c : 0; }

/* A character's own struct (the game's): GameState + 0x474 + slot * 0x964 (func_020108d8), and an
 * object reaches it through +0x150 (func_02053c6c). eidako2's records sit 0x3C into it. */
#define CHAR0 0x020F384Cu
#define CHAR_SIZE 0x964u
static u32 char_of(u32 o) {
    u32 c = r32(o + 0x150);
    for (u32 k = 0; k < 4; k++) if (c == CHAR0 + k * CHAR_SIZE) return c;
    return 0;
}
#define BATTLE_STATS 0x94u                                 /* a member's current battle stats: char + 0x94 */
static int in_battle(void) { return (int)(overlays_loaded_mask() & 1); }   /* the battle overlay, ov000 */
static int party(u32* out) {                              /* the party members, in order (up to 4) */
    int n = 0;
    for (int i = 0; i < N_OBJ && n < 4; i++) {
        u32 o = object(i);
        if (!o || !(r16(o) & 0x100)) continue;              /* 0x100: in use (func_0200ff1c) */
        u32 c = char_of(o); if (!c) continue;
        int dup = 0; for (int j = 0; j < n; j++) dup |= char_of(out[j]) == c;
        if (!dup) out[n++] = o;
    }
    return n;
}

/* Learned abilities are 36 bytes at char + 0x8EC and learned spells 9 bytes at + 0x910, right after
 * (func_02083b00 / func_02083bc0): bit b of the 360 sits at + 0x8EC + b/8. */
#define LEARNED 0x8ECu
#define N_LB 45                                            /* bytes of learned bits */
static u32 record(u32 o) { return char_of(o); }

/* stats: 0 max HP, 1 max MP, 2 attack, 3 defence, 4 agility, 5 deftness, 6 charm, 7 magical might, 8 magical mending */
#define N_STAT 9
static void stat_name(int s, char* out) {
    static const char names[] = "Max HP\0Max MP\0Attack\0Defence\0Agility\0Deftness\0Charm\0Magical might\0Magical mending\0";
    const char* p = names; while (s--) p += slen(p) + 1;
    sput(out, p);
}
static int stat_get(u32 p, int s) {
    switch (s) {
    case 0: return r16(p + 4);
    case 1: return r16(p + 6);
    case 2: case 3: case 4: case 5: return r16(p + 8 + 2 * (s - 2));
    default: return (r32(p + 0x10) >> (10 * (s - 6))) & 0x3FF;
    }
}
static void stat_set(u32 p, int s, int v) {
    if (v < 0) v = 0; if (v > 999) v = 999;
    switch (s) {
    case 0: w16(p + 4, (u16)v); if (r16(p) > v) w16(p, (u16)v); break;
    case 1: w16(p + 6, (u16)v); if (r16(p + 2) > v) w16(p + 2, (u16)v); break;
    case 2: case 3: case 4: case 5: w16(p + 8 + 2 * (s - 2), (u16)v); break;
    default: {
        int sh = 10 * (s - 6);
        w32(p + 0x10, (r32(p + 0x10) & ~(0x3FFu << sh)) | ((u32)v << sh));
    }
    }
}

/* ---- settings */
enum {
    ID_INF_HP = 401, ID_INF_MP, ID_MAXSTATS, ID_HEAL, ID_GOLD, ID_BANK, ID_MEDALS, ID_ITEMS99,
    ID_FS0 = 410, ID_FS1, ID_FS2, ID_INFO, ID_ALLOFF, ID_ABOUT, ID_USE_ALL, ID_DIAG, ID_REC, ID_UNNAMED,
    ID_EDIT0 = 420,                         /* + member * 10 + stat (0..8), + 9 = clear that member */
    ID_LAST = 459,
    ID_SK0 = 2000,                          /* + member * 400 + bit (0..359): learn or forget one spell or ability */
    ID_SKB = 3600,                          /* + member * 10 + SKB_*: the whole lot */
    ID_SK_LAST = 3639,
    ID_COUP = 3640, ID_TENSION,             /* battle: always a coup de grace, always 100 tension */
    ID_PERM_BUFFS = 3642,                   /* battle: buffs and debuffs last the whole battle */
    ID_CRIT,                                /* battle: the party's attacks are always critical hits */
    ID_COMBO,                               /* battle: the party's attacks always get the full combo bonus */
    ID_ALL_BUFFS,                           /* battle: every buff on the party now, at its highest level */
    ID_PKJ,                                 /* battle: every monster battle is against platinum king jewels */
    ID_NO_FLEE,                             /* battle: monsters can't run away */
    ID_GEM,                                 /* battle: every monster battle is against gem slimes */
    ID_VOC0 = 3650,                         /* + member * 15 + vocation (0 Guardian, 1..12): change vocation; + 13: best gear */
    ID_BEST_ALL = 3710, ID_AUTO_BEST,       /* best gear for everyone; best gear after every vocation change */
    ID_VOC_LAST = 3719,
};
enum { SKB_ALL_SPELLS, SKB_NO_SPELLS, SKB_ALL_ABIL, SKB_NO_ABIL, SKB_UNDO };
static struct {
    int loaded;
    int inf_hp, inf_mp, max_stats, first, info;
    int ov[4][N_STAT];                      /* edited stats per party member (-1: not edited) */
    u8 on[4][N_LB], off[4][N_LB];           /* learned bits kept on / kept off, per party member */
    u8 orig[4][N_LB];                       /* what those bits were before the first edit (for undo) */
    u8 shown[4][N_LB]; int shown_ok[4];     /* what the menu's ticks show */
    int use_all;                            /* everyone knows every spell and ability, whatever the vocation */
    int unnamed;                            /* ...and the bits the memory map has no name for (item-only ones?) */
    int coup, tension;                      /* in battles: coup de grace always ready, tension always 100 */
    int auto_best;                          /* a vocation change also puts on the best gear from the bag */
    int perm_buffs;                         /* buffs and debuffs never wear off in a battle (as in the old games) */
    int crit;                               /* every party attack that can be critical is */
    int combo;                              /* every party attack that can combo gets the 4-hit bonus (x2) */
    int pkj;                                /* every battle but a boss fight is against this species (0: off; 242, 185) */
    int no_flee;                            /* monsters' Flee does nothing */
    u8 added[4][N_LB];                      /* the bits use_all turned on (taken off again when it's turned off) */
} S;

static char* hput(char* o, const u8* b, int n) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { *o++ = hx[b[i] >> 4]; *o++ = hx[b[i] & 15]; }
    *o = 0; return o;
}
static int hget(const char* s, u8* b, int n) {             /* exactly n bytes of hex, then the end of the line */
    u8 t[N_LB];
    for (int i = 0; i < 2 * n; i++) {
        char c = s[i]; int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return 0;
        if (i & 1) t[i >> 1] = (u8)(t[i >> 1] << 4 | v); else t[i >> 1] = (u8)v;
    }
    for (const char* p = s + 2 * n; *p; p++) if (*p != ' ' && *p != '\r' && *p != '\n') return 0;
    memcpy(b, t, (u64)n); return 1;
}
static int any(const u8* b) { for (int i = 0; i < N_LB; i++) if (b[i]) return 1; return 0; }

static void save(void) {
    char buf[2400], *o = buf;                              /* at most ~1850 bytes; a frame over 4 KB would skip Windows' stack guard page */
    o = sput(o, "# DQIX cheats (the Cheats menu)\n");
    o = sput(o, "infinite_hp="); o = iput(o, S.inf_hp); o = sput(o, "\n");
    o = sput(o, "infinite_mp="); o = iput(o, S.inf_mp); o = sput(o, "\n");
    o = sput(o, "max_stats="); o = iput(o, S.max_stats); o = sput(o, "\n");
    o = sput(o, "first_strike="); o = iput(o, S.first); o = sput(o, "\n");
    o = sput(o, "battle_info="); o = iput(o, S.info); o = sput(o, "\n");
    o = sput(o, "use_every_spell_and_ability="); o = iput(o, S.use_all); o = sput(o, "\n");
    o = sput(o, "include_unnamed="); o = iput(o, S.unnamed); o = sput(o, "\n");
    o = sput(o, "always_coup_de_grace="); o = iput(o, S.coup); o = sput(o, "\n");
    o = sput(o, "always_max_tension="); o = iput(o, S.tension); o = sput(o, "\n");
    o = sput(o, "best_gear_after_vocation_change="); o = iput(o, S.auto_best); o = sput(o, "\n");
    o = sput(o, "permanent_buffs="); o = iput(o, S.perm_buffs); o = sput(o, "\n");
    o = sput(o, "always_critical="); o = iput(o, S.crit); o = sput(o, "\n");
    o = sput(o, "always_max_combo="); o = iput(o, S.combo); o = sput(o, "\n");
    o = sput(o, "replace_monsters_with="); o = iput(o, S.pkj); o = sput(o, "\n");   /* 242 platinum king jewel, 185 gem slime */
    o = sput(o, "enemies_cant_flee="); o = iput(o, S.no_flee); o = sput(o, "\n");
    for (int m = 0; m < 4; m++) if (any(S.added[m])) {
        o = sput(o, "learned_added "); o = iput(o, m + 1); o = sput(o, "="); o = hput(o, S.added[m], N_LB); o = sput(o, "\n");
    }
    for (int m = 0; m < 4; m++) for (int s = 0; s < N_STAT; s++) if (S.ov[m][s] >= 0) {
        o = sput(o, "edit "); o = iput(o, m + 1); o = sput(o, " "); o = iput(o, s); o = sput(o, "="); o = iput(o, S.ov[m][s]); o = sput(o, "\n");
    }
    for (int m = 0; m < 4; m++) if (any(S.on[m]) || any(S.off[m])) {
        o = sput(o, "learned_on "); o = iput(o, m + 1); o = sput(o, "="); o = hput(o, S.on[m], N_LB); o = sput(o, "\n");
        o = sput(o, "learned_off "); o = iput(o, m + 1); o = sput(o, "="); o = hput(o, S.off[m], N_LB); o = sput(o, "\n");
        o = sput(o, "learned_orig "); o = iput(o, m + 1); o = sput(o, "="); o = hput(o, S.orig[m], N_LB); o = sput(o, "\n");
    }
    void* f = CALL(fn_fopen, IMP_fopen)("cheats.ini", "wb");
    if (!f) return;
    CALL(fn_fwrite, IMP_fwrite)(buf, 1, (u64)(o - buf), f);
    CALL(fn_p, IMP_fclose)(f);
}
static int keyval(const char* line, const char* key, int* v) {
    if (!starts(line, key)) return 0;
    const char* p = line + slen(key);
    if (*p != '=') return 0;
    int x; if (!sparse(p + 1, &x)) return 0;
    *v = x; return 1;
}
static void load(void) {
    if (S.loaded) return;
    S.loaded = 1;
    for (int m = 0; m < 4; m++) for (int s = 0; s < N_STAT; s++) S.ov[m][s] = -1;
    void* f = CALL(fn_fopen, IMP_fopen)("cheats.ini", "rb");
    if (!f) return;
    char line[200];
    while (CALL(fn_fgets, IMP_fgets)(line, sizeof line, f)) {
        int v;
        if (keyval(line, "infinite_hp", &v)) S.inf_hp = v != 0;
        else if (keyval(line, "infinite_mp", &v)) S.inf_mp = v != 0;
        else if (keyval(line, "max_stats", &v)) S.max_stats = v != 0;
        else if (keyval(line, "first_strike", &v)) S.first = v >= 0 && v <= 2 ? v : 0;
        else if (keyval(line, "battle_info", &v)) S.info = v != 0;
        else if (keyval(line, "use_every_spell_and_ability", &v)) S.use_all = v != 0;
        else if (keyval(line, "include_unnamed", &v)) S.unnamed = v != 0;
        else if (keyval(line, "always_coup_de_grace", &v)) S.coup = v != 0;
        else if (keyval(line, "always_max_tension", &v)) S.tension = v != 0;
        else if (keyval(line, "best_gear_after_vocation_change", &v)) S.auto_best = v != 0;
        else if (keyval(line, "permanent_buffs", &v)) S.perm_buffs = v != 0;
        else if (keyval(line, "always_critical", &v)) S.crit = v != 0;
        else if (keyval(line, "always_max_combo", &v)) S.combo = v != 0;
        else if (keyval(line, "platinum_king_jewels", &v)) S.pkj = v ? PKJ_ID : 0;     /* v2.10's key */
        else if (keyval(line, "replace_monsters_with", &v)) S.pkj = v == PKJ_ID || v == GEM_ID ? v : 0;
        else if (keyval(line, "enemies_cant_flee", &v)) S.no_flee = v != 0;
        else if (starts(line, "edit ") && line[5] >= '1' && line[5] <= '4' && line[6] == ' ') {
            int m = line[5] - '1', s = 0; const char* p = line + 7;
            while (*p >= '0' && *p <= '9') s = s * 10 + (*p++ - '0');
            if (*p == '=' && s < N_STAT && sparse(p + 1, &v)) S.ov[m][s] = v > 999 ? 999 : v;
        }
        else {
            static const char keys[] = "learned_on \0learned_off \0learned_orig \0learned_added \0";
            const char* k = keys;
            for (int which = 0; which < 4; which++, k += slen(k) + 1) {
                if (!starts(line, k)) continue;
                const char* p = line + slen(k);
                if (p[0] < '1' || p[0] > '4' || p[1] != '=') break;
                int m = p[0] - '1';
                hget(p + 2, which == 0 ? S.on[m] : which == 1 ? S.off[m] : which == 2 ? S.orig[m] : S.added[m], N_LB);
                break;
            }
        }
    }
    CALL(fn_p, IMP_fclose)(f);
    for (int m = 0; m < 4; m++) for (int i = 0; i < N_LB; i++) {   /* only real spells and abilities, never both ways */
        S.on[m][i] &= sk_known[i]; S.off[m][i] &= sk_known[i] & ~S.on[m][i]; S.added[m][i] &= (u8)(i == 35 ? 0x7F : 0xFF);
    }
}

/* ---- the title bar */
static void say(const char* s) {
    int n = slen(s); if (n > 159) n = 159;
    memcpy(g_notice, s, (u64)n); g_notice[n] = 0;
    g_notice_until = CALL(fn_u64, IMP_SDL_GetPerformanceCounter)() + g_perf_freq * 2;
}

/* ---- learned spells and abilities */
static u32 member_record(int m) {                           /* party member m's record, or 0 if there isn't one */
    u32 p[4]; int n = party(p);
    return m < n ? record(p[m]) : 0;
}
static const wchar* sk_name(int b) {                        /* the menu text of bit b (0 if it's not a real one) */
    const wchar* s = sk_names;
    for (int k = 0; k < SK_N; k++) {
        if (sk_bit[k] == b) return s;
        while (*s) s++; s++;
    }
    return 0;
}
/* Has the member earned ability bit b with skill points (+0x428: points per skill line)? The game
 * doesn't seem to rebuild abilities on its own, so taking one back must never take an earned one. */
#define SKILL_PTS 0x464u                                   /* eidako2's +0x428 */
static int earned(u32 rec, int b) {
    for (int k = 0; k < SK_N; k++)
        if (sk_bit[k] == b) return sk_line[k] && sk_pts[k] && r8(rec + SKILL_PTS + sk_line[k]) >= sk_pts[k];
    return 0;
}
static u8 earned_mask(u32 rec, int i) {                    /* the earned bits of learned byte i */
    u8 e = 0;
    if (i < 36) for (int j = 0; j < 8; j++) if (earned(rec, i * 8 + j)) e |= (u8)(1 << j);
    return e;
}
static void sk_edit(int m, u32 rec, int b, int want) {     /* keep bit b on (want 1) or off (want 0) */
    int i = b >> 3; u8 mask = (u8)(1 << (b & 7));
    if (!(sk_known[i] & mask)) return;
    if (!((S.on[m][i] | S.off[m][i]) & mask))               /* first edit of this bit: remember it for undo */
        S.orig[m][i] = (u8)((S.orig[m][i] & ~mask) | (r8(rec + LEARNED + i) & mask));
    if (want) { S.on[m][i] |= mask; S.off[m][i] &= (u8)~mask; }
    else { S.off[m][i] |= mask; S.on[m][i] &= (u8)~mask; }
}
static void sk_apply(int m, u32 rec) {
    for (int i = 0; i < N_LB; i++) {
        u8 v = r8(rec + LEARNED + i), nv = (u8)((v | S.on[m][i]) & ~S.off[m][i]);
        if (nv != v) w8(rec + LEARNED + i, nv);
    }
}
static void sk_undo(int m) {                                /* back to what the member had before the edits */
    u32 rec = member_record(m);
    if (rec) for (int i = 0; i < N_LB; i++) {
        u8 e = S.on[m][i] | S.off[m][i];
        if (e) w8(rec + LEARNED + i, (u8)((r8(rec + LEARNED + i) & ~e) | ((S.orig[m][i] | earned_mask(rec, i)) & e)));
    }
    memset(S.on[m], 0, N_LB); memset(S.off[m], 0, N_LB); memset(S.orig[m], 0, N_LB);
}
static void sk_ticks(void) {                                /* tick what each member knows now */
    u32 p[4]; int n = party(p);
    for (int m = 0; m < 4; m++) {
        u32 rec = m < n ? record(p[m]) : 0;
        if (!rec) continue;
        u8 cur[N_LB];
        for (int i = 0; i < N_LB; i++) cur[i] = r8(rec + LEARNED + i);
        for (int k = 0; k < SK_N; k++) {
            int b = sk_bit[k], i = b >> 3; u8 mask = (u8)(1 << (b & 7));
            if (!S.shown_ok[m] || ((S.shown[m][i] ^ cur[i]) & mask)) menu_check(ID_SK0 + m * 400 + b, (cur[i] & mask) != 0);
        }
        memcpy(S.shown[m], cur, N_LB); S.shown_ok[m] = 1;
    }
}
static void say_member(const char* a, int m, const char* z) {
    char t[160], *o = t;
    o = sput(o, a); o = iput(o, m + 1); sput(o, z); say(t);
}
static void sk_toggle(int m, int b) {
    u32 rec = member_record(m);
    const wchar* nm = sk_name(b);
    if (!nm) return;
    if (!rec) { say_member("There's no party member ", m, " right now"); return; }
    int i = b >> 3; u8 mask = (u8)(1 << (b & 7));
    int want = !(r8(rec + LEARNED + i) & mask);
    sk_edit(m, rec, b, want);
    sk_apply(m, rec);
    char t[160], *o = t;
    o = sput(o, "Party member "); o = iput(o, m + 1); o = sput(o, want ? " now knows " : " no longer knows ");
    while (*nm && o - t < 150) { wchar c = *nm++; *o++ = c < 128 ? (char)c : '?'; }
    *o = 0; say(t);
}
static void sk_bulk(int m, int op) {
    if (op == SKB_UNDO) { sk_undo(m); say_member("Spell and ability edits undone for party member ", m, ""); return; }
    u32 rec = member_record(m);
    if (!rec) { say_member("There's no party member ", m, " right now"); return; }
    int spells = op == SKB_ALL_SPELLS || op == SKB_NO_SPELLS, want = op == SKB_ALL_SPELLS || op == SKB_ALL_ABIL;
    for (int k = 0; k < SK_N; k++) if ((sk_bit[k] >= 288) == spells) sk_edit(m, rec, sk_bit[k], want);
    sk_apply(m, rec);
    say_member(want ? (spells ? "Every spell learned by party member " : "Every ability learned by party member ")
                    : (spells ? "Every spell forgotten by party member " : "Every ability forgotten by party member "), m, "");
}

/* Every spell and ability, whatever the vocation. The game rebuilds a member's learned bits from
 * their vocation and skill points (func_02083c18 clears them, func_02083b60 learns each spell), and
 * the spell menus only test those bits (func_02083bc0), so keeping every bit on is enough. A spell
 * or ability someone was told to forget (the editor) stays forgotten. Returns 1 if bits were added. */
/* the bits use_all turns on: the named ones, and with "unnamed" every other bit too, but ability 287
 * (there are 0x11F abilities, 0..286) */
static u8 use_all_mask(int i) {
    if (!S.unnamed) return sk_known[i];
    return i == 35 ? 0x7F : 0xFF;
}
static int use_all_apply(int m, u32 rec) {
    int changed = 0;
    for (int i = 0; i < N_LB; i++) {
        u8 v = r8(rec + LEARNED + i), add = (u8)(use_all_mask(i) & ~S.off[m][i] & ~v);
        if (!add) continue;
        w8(rec + LEARNED + i, (u8)(v | add));
        u8 fresh = (u8)(add & ~S.on[m][i] & ~S.added[m][i]);
        if (fresh) { S.added[m][i] |= fresh; changed = 1; }
    }
    return changed;
}
static void use_all_off(void) {                             /* take back what it added (edits stay) */
    u32 p[4]; int n = party(p);
    for (int m = 0; m < 4; m++) {
        u32 rec = m < n ? record(p[m]) : 0;
        if (rec) for (int i = 0; i < N_LB; i++) {
            u8 rm = (u8)(S.added[m][i] & ~S.on[m][i]);
            if (rm) rm &= (u8)~earned_mask(rec, i);
            if (rm) w8(rec + LEARNED + i, (u8)(r8(rec + LEARNED + i) & ~rm));
        }
        memset(S.added[m], 0, N_LB);                        /* an absent member: the game's next rebuild puts them right */
    }
    S.use_all = 0;
}

/* func_0209ac28(r0 menu data, r1 object, r2 spell): 1 if the spell is on the member's vocation's
 * list at their vocation level. The spell menus (ov002_021665b0, its only caller) show a spell only
 * if this and IsSpellLearned both say yes, so learned bits alone can't get past a vocation. Our hook
 * on its entry calls this first: non-zero means "yes" without asking the game. */
static int voc_calls;
MS int voc_spell_ok(u32* cpu) {
    load();
    voc_calls++;
    u32 o = cpu[1], id = cpu[2];
    if (id >= 72 || !valid(o)) return 0;
    if (S.use_all) return 1;
    u32 c = char_of(o); if (!c) return 0;
    u32 p[4]; int n = party(p);
    for (int m = 0; m < n; m++)
        if (char_of(p[m]) == c) return (S.on[m][36 + (id >> 3)] >> (id & 7)) & 1;   /* turned on in the editor */
    return 0;
}

/* ov002_02157174 lists a member's spells from their vocation's own list unless the vocation is 0,
 * which lists every learned spell. Non-zero here: use the every-learned-spell list (r5 = the object). */
static int spell_list_calls[2], spell_list_yes;
MS int spell_list_all(u32* cpu, int objreg) {             /* objreg: the Cpu register holding the object */
    load();
    spell_list_calls[objreg == 4]++;
    int yes = 0;
    if (S.use_all) yes = 1;
    else {
        u32 ob = cpu[objreg & 15], c = valid(ob) ? char_of(ob) : 0, p[4]; int n = party(p);
        for (int m = 0; c && m < n; m++)
            if (char_of(p[m]) == c) for (int i = 36; i < N_LB; i++) yes |= S.on[m][i] != 0;   /* spells turned on in the editor */
    }
    spell_list_yes += yes;
    return yes;
}

/* ov000_0217c32c (the battle Abilities list): non-zero = usable whatever the weapon, as for vocation 0 */
static int ability_calls, ability_yes;
MS int ability_any(u32* cpu) {
    load();
    (void)cpu;
    ability_calls++;
    if (S.use_all) { ability_yes++; return 1; }
    return 0;
}

/* func_02083b28 ForgetAbility(r0 char, r1 id), called only to forget item-given abilities whose item
 * isn't equipped: non-zero = keep it (use_all covers it, or the editor turned it on for this member) */
static int forget_calls, forget_kept;
static int crit_calls, crit_forced, crit_spells;           /* crit_force (below) */
static int combo_calls, combo_forced;                      /* combo_fix (below) */
static int pkj_seen, pkj_done;                             /* pkj_battle (below) */
static int flee_seen, flee_blocked;                        /* flee_block (below) */
MS int keep_ability(u32* cpu) {
    load();
    forget_calls++;
    u32 c = cpu[0], id = cpu[1];
    if (id >= 287) return 0;
    int i = (int)(id >> 3); u8 mask = (u8)(1 << (id & 7));
    u32 p[4]; int n = party(p), m = -1;
    for (int k = 0; k < n; k++) if (char_of(p[k]) == c) m = k;
    int keep = 0;
    if (m >= 0 && (S.on[m][i] & mask)) keep = 1;
    else if (S.use_all && (use_all_mask(i) & mask) && !(m >= 0 && (S.off[m][i] & mask))) keep = 1;
    forget_kept += keep;
    return keep;
}

/* Cheats > Record: which game functions run while recording (to find what a menu calls) */
static int recording;
static void record_toggle(void) {
    if (!recording) {
        g_prof_state = 0;
        memset(g_prof_addr, 0, sizeof g_prof_addr); memset(g_prof_cnt, 0, sizeof g_prof_cnt);
        g_prof_from = 0; g_prof_to = ~0ull;
        g_prof_state = 1; recording = 1;
        say("Recording what the game does: open the menu, then choose Stop recording");
        return;
    }
    g_prof_state = 0; recording = 0;
    void* f = CALL(fn_fopen, IMP_fopen)("cheats-record.txt", "wb");
    if (!f) { say("Couldn't write cheats-record.txt"); return; }
    static char b[4096]; char* o = b; int n = 0;
    o = sput(o, "# DQIX cheats: game functions entered while recording (ARM address, times)\n");
    for (int i = 0; i < 65536; i++) {
        u32 a = g_prof_addr[i];
        if (!a || !g_prof_cnt[i] || (a >> 31)) continue;          /* the ARM7 side has the top bit set */
        static const char hx[] = "0123456789abcdef";
        for (int k = 28; k >= 0; k -= 4) *o++ = hx[(a >> k) & 15];
        *o++ = ' '; o = iput(o, (int)g_prof_cnt[i]); *o++ = '\n'; n++;
        if (o - b > 4000) { CALL(fn_fwrite, IMP_fwrite)(b, 1, (u64)(o - b), f); o = b; }
    }
    CALL(fn_fwrite, IMP_fwrite)(b, 1, (u64)(o - b), f);
    CALL(fn_p, IMP_fclose)(f);
    char t[100]; o = sput(t, "Saved cheats-record.txt ("); o = iput(o, n); sput(o, " functions)"); say(t);
}

/* Cheats > Diagnostics: what the cheats see, for when something doesn't work */
static int count_bits(u32 a, int n) { int k = 0; for (int i = 0; i < n; i++) for (u8 v = r8(a + i); v; v &= (u8)(v - 1)) k++; return k; }
static void diagnostics(void) {
    static char t[1500]; char* o = t;                       /* static: a 4 KB+ frame would skip the stack guard page */
    o = sput(o, "Party found by the cheats (objects whose +0x150 is a character slot):\r\n");
    u32 p[4]; int n = party(p);
    if (!n) o = sput(o, "  none\r\n");
    for (int m = 0; m < n; m++) {
        u32 c = char_of(p[m]), slot = (c - CHAR0) / CHAR_SIZE, w = r32(c + 0x950);
        o = sput(o, "  member "); o = iput(o, m + 1); o = sput(o, ": slot "); o = iput(o, (int)slot);
        o = sput(o, ", vocation "); o = iput(o, (int)(w & 0xFF)); o = sput(o, " level "); o = iput(o, r16(c + 0x16C + 2 * (w & 0xFF)));
        o = sput(o, ", spells known "); o = iput(o, count_bits(c + LEARNED + 36, 9));
        o = sput(o, ", abilities known "); o = iput(o, count_bits(c + LEARNED, 36));
        o = sput(o, ", flags "); { u8 h[2] = { (u8)(r16(p[m]) >> 8), (u8)r16(p[m]) }; o = hput(o, h, 2); }
        o = sput(o, "\r\n");
    }
    o = sput(o, "\r\nThe four character slots:\r\n");
    for (u32 k = 0; k < 4; k++) {
        u32 c = CHAR0 + k * CHAR_SIZE, w = r32(c + 0x950);
        o = sput(o, "  slot "); o = iput(o, (int)k); o = sput(o, ": vocation "); o = iput(o, (int)(w & 0xFF));
        o = sput(o, ", spells "); o = iput(o, count_bits(c + LEARNED + 36, 9));
        o = sput(o, ", abilities "); o = iput(o, count_bits(c + LEARNED, 36));
        o = sput(o, ", spell bytes "); { u8 b[9]; for (int i = 0; i < 9; i++) b[i] = r8(c + LEARNED + 36 + i); o = hput(o, b, 9); }
        o = sput(o, "\r\n");
    }
    o = sput(o, "\r\nSpell list hooks: ov002 "); o = iput(o, spell_list_calls[0]); o = sput(o, " calls, ov026 ");
    o = iput(o, spell_list_calls[1]); o = sput(o, " calls, ");
    o = iput(o, spell_list_yes); o = sput(o, " given every learned spell. Vocation check hook: "); o = iput(o, voc_calls); o = sput(o, " calls. Battle ability hook: ");
    o = iput(o, ability_calls); o = sput(o, " calls, "); o = iput(o, ability_yes); o = sput(o, " made usable. Item abilities kept: ");
    o = iput(o, forget_kept); o = sput(o, " of "); o = iput(o, forget_calls); o = sput(o, ". Critical-hit checks: ");
    o = iput(o, crit_calls); o = sput(o, " (spells "); o = iput(o, crit_spells); o = sput(o, "), forced "); o = iput(o, crit_forced);
    o = sput(o, ". Battles set up: "); o = iput(o, pkj_seen); o = sput(o, ", monsters replaced (jewels / gem slimes) "); o = iput(o, pkj_done);
    o = sput(o, ". Monster flee attempts: "); o = iput(o, flee_seen); o = sput(o, ", stopped "); o = iput(o, flee_blocked);
    o = sput(o, ". Combo checks: "); o = iput(o, combo_calls); o = sput(o, ", forced "); o = iput(o, combo_forced); o = sput(o, ".\r\n");
    o = sput(o, "In a battle: "); o = sput(o, in_battle() ? "yes" : "no");
    for (int m = 0; m < n; m++) { u32 b = char_of(p[m]) + BATTLE_STATS;
        o = sput(o, m ? ", " : ". Tension / coup bytes: "); o = iput(o, r8(b + 0x24)); o = sput(o, "/");
        { u8 h[2] = { r8(b + 0x17), r8(b + 0x3B) }; o = hput(o, h, 2); } }
    o = sput(o, "\r\nEvery spell and ability: "); o = sput(o, S.use_all ? "on" : "off");
    o = sput(o, ". Objects with flag 0x800: ");
    { int k = 0; for (int i = 0; i < N_OBJ; i++) { u32 ob = object(i); if (ob && (r16(ob) & 0x800)) k++; } o = iput(o, k); }
    o = sput(o, "\r\n\r\nThis is also saved in cheats-report.txt next to the game.");
    void* f = CALL(fn_fopen, IMP_fopen)("cheats-report.txt", "wb");
    if (f) { CALL(fn_fwrite, IMP_fwrite)(t, 1, (u64)(o - t), f); CALL(fn_p, IMP_fclose)(f); }
    static wchar w[1500]; int i = 0;
    for (; t + i < o && i < 1499; i++) w[i] = (wchar)(u8)t[i];
    w[i] = 0;
    CALL(fn_msgbox, IMP_MessageBoxW)(0, w, u"Cheats: diagnostics", 0x40);
}

/* ---- calling the game's own code
 * A recompiled function is void f(Cpu*), its arguments in r0.. as on the DS. The cheats run inside the
 * frame callback, so the ARM9 is somewhere in the middle of the game, often in the idle thread, whose
 * stack is tiny (v2.2 called on it and overwrote the heap next to it: the allocator crashed a few
 * seconds later). So: keep its whole Cpu, borrow the 4 KB below its stack pointer as the call's stack,
 * saving them first and putting them back after, so the call leaves no trace there whichever thread
 * it is. While cheat_in_call is set, the hook on arm9_sync drops the cycles instead of letting time
 * pass, so no interrupt, scanline or other thread runs inside the call. Only in the normal modes. */
int cheat_in_call;                                         /* read by hook_sync (hooks.S) */
static u32 call_save[41];
#define SCRATCH 0x1000u
static u8 scratch_save[SCRATCH];
static u8* arm_mem(u32 a, u32 n) {                         /* host memory for ARM9 [a, a+n): main RAM or DTCM, else 0 */
    if (a >= 0x02000000u && a + n <= 0x02400000u) return ram(a);
    u32 b = g_dtcm_base;
    if (a >= b && a + n <= b + 0x4000u) return g_dtcm + (a - b);
    return 0;
}
static int game_call(MS void (*f)(u32*), u32 r0, u32 r1) {
    u32 sp = g_arm9[13] & ~7u, mode = g_arm9[21] & 0x1F;
    if (mode != 0x1F && mode != 0x10) return 0;
    u32 lo = sp - SCRATCH;
    u8* m = arm_mem(lo, SCRATCH);
    if (!m) return 0;
    if (lo < 0x020F7000u && sp > 0x020F33D8u) return 0;    /* never on top of the party and bags the call writes */
    memcpy(scratch_save, m, SCRATCH);
    memcpy(call_save, g_arm9, sizeof call_save);
    u32 cyc = g_cyc;
    g_arm9[0] = r0; g_arm9[1] = r1; g_arm9[2] = g_arm9[3] = 0;
    g_arm9[13] = sp - 0x100;                               /* a push in progress never reaches that far; RecalcStats needs < 1 KB */
    cheat_in_call = 1;
    f(g_arm9);
    cheat_in_call = 0;
    memcpy(g_arm9, call_save, sizeof call_save);
    memcpy(m, scratch_save, SCRATCH);
    g_cyc = cyc;
    return 1;
}

/* ---- vocations and equipment
 * Game vocation ids (char + 0x950): 0 Guardian (the debug class), 1 warrior, 2 priest, 3 mage, 4 martial
 * artist, 5 thief, 6 minstrel, 7 gladiator, 8 armamentalist, 9 paladin, 10 sage, 11 luminary, 12 ranger.
 * SetVocation (func_02083ca0) only stores the word and sets bit v of the vocations-used mask (+0x954);
 * the levels are per vocation (+0x16C + v * 2). RecalcStats (func_02083e28) works the stats out again. */
#define VOC_WORD 0x950u
#define VOC_USED 0x954u
#define VOC_LEVEL 0x16Cu
#define EQUIP 0x488u                                       /* s16[10]: torso, legs, face, hair, arms, feet, head, weapon, shield, accessory (-1 empty) */
#define N_VOC 12
static const char voc_text[] = "Guardian\0warrior\0priest\0mage\0martial artist\0thief\0minstrel\0gladiator\0armamentalist\0paladin\0sage\0luminary\0ranger\0";
static const char* voc_name(int v) { const char* p = voc_text; if (v > N_VOC) v = 0; while (v--) p += slen(p) + 1; return p; }
static int voc_of(u32 c) { return (int)(r32(c + VOC_WORD) & 0xFF); }

/* The bags (eidako2's map; the game's bag object is at 0x020F5FB0: ids, counts, sizes, in this order):
 * weapon, shield, torso, legs, head, arms, feet, accessory. An empty entry has id -1 and count 0. */
static const u32 bag_ids[8] = { 0x020F5FB0, 0x020F62E0, 0x020F6370, 0x020F65B0, 0x020F66D0, 0x020F6880, 0x020F6970, 0x020F6AC0 };
static const u32 bag_cnt[8] = { 0x020F61D0, 0x020F6340, 0x020F64F0, 0x020F6670, 0x020F67F0, 0x020F6920, 0x020F6A50, 0x020F6B40 };
static const u16 bag_n[8] = { 272, 48, 192, 96, 144, 80, 112, 64 };
static const u8 slot_cat[10] = { 2, 3, 255, 255, 5, 6, 4, 0, 1, 7 };
static const u8 slot_order[8] = { 7, 8, 6, 0, 4, 1, 5, 9 };   /* weapon first: nothing depends on it, but it reads well */
static int bag_add(int cat, int id) {                      /* put one in the bag (like func_0207c378, but never past 99) */
    u32 ids = bag_ids[cat], cnt = bag_cnt[cat]; int n = bag_n[cat];
    for (int i = 0; i < n; i++) if ((short)r16(ids + 2 * i) == id && r8(cnt + i)) {
        if (r8(cnt + i) >= 99) return 0;
        w8(cnt + i, (u8)(r8(cnt + i) + 1)); return 1;
    }
    for (int i = 0; i < n; i++) if ((short)r16(ids + 2 * i) < 0 || !r8(cnt + i)) { w16(ids + 2 * i, (u16)id); w8(cnt + i, 1); return 1; }
    return 0;
}
static void bag_take(int cat, int id) {                    /* take one out (func_0207c484) */
    u32 ids = bag_ids[cat], cnt = bag_cnt[cat]; int n = bag_n[cat];
    for (int i = 0; i < n; i++) if ((short)r16(ids + 2 * i) == id && r8(cnt + i)) {
        u8 k = (u8)(r8(cnt + i) - 1);
        w8(cnt + i, k);
        if (!k) w16(ids + 2 * i, 0xFFFF);
        return;
    }
}
static int it_find(int id) {
    if (id < 0) return -1;
    int lo = 0, hi = IT_N - 1;
    while (lo <= hi) { int mid = (lo + hi) >> 1; if (it_asset[mid] == id) return mid; if (it_asset[mid] < id) lo = mid + 1; else hi = mid - 1; }
    return -1;
}
static int can_equip(int k, int voc) { return k >= 0 && (voc == 0 || (voc <= N_VOC && ((it_mask[k] >> (voc - 1)) & 1))); }
/* what each vocation wants (x10): attack, defence, agility, deftness, magical might, magical mending, max MP,
 * then block, evasion and critical rate (those are per mille, so 1 each) */
static const u8 voc_w[N_VOC + 1][IT_NS] = {
    { 20, 20, 5, 5, 5, 5, 2, 1, 1, 1 },    /* Guardian */
    { 30, 20, 5, 5, 0, 0, 0, 1, 1, 1 },    /* warrior */
    { 10, 20, 5, 0, 5, 30, 10, 1, 1, 0 },  /* priest */
    { 5, 20, 5, 0, 30, 5, 10, 1, 1, 0 },   /* mage */
    { 30, 15, 10, 10, 0, 0, 0, 1, 1, 1 },  /* martial artist */
    { 25, 15, 15, 15, 0, 0, 0, 1, 1, 1 },  /* thief */
    { 20, 20, 10, 5, 10, 10, 5, 1, 1, 1 }, /* minstrel */
    { 30, 20, 5, 5, 0, 0, 0, 1, 1, 1 },    /* gladiator */
    { 20, 20, 5, 5, 20, 5, 5, 1, 1, 1 },   /* armamentalist */
    { 15, 30, 5, 0, 0, 10, 3, 1, 1, 0 },   /* paladin */
    { 5, 20, 5, 0, 25, 25, 10, 1, 1, 0 },  /* sage */
    { 20, 20, 10, 10, 5, 5, 3, 1, 1, 1 },  /* luminary */
    { 30, 15, 10, 10, 0, 0, 0, 1, 1, 1 },  /* ranger */
};
static s64 item_score(int k, int voc) {                    /* better stats first, then the dearer one */
    s64 s = 0;
    for (int i = 0; i < IT_NS; i++) s += (s64)voc_w[voc][i] * it_stat[k][i];
    return s * 65536 + it_price[k];
}

/* RecalcStats, then no more HP or MP than the new maximums allow */
static int recalc(u32 o, u32 c) {
    if (!game_call(fn_main_02083e28, c, 0)) return 0;
    u32 ps[2] = { base_primary(o), cur_primary(o) };
    for (int k = 0; k < 2; k++) {
        u32 p = ps[k]; if (!p) continue;
        if (r16(p) > r16(p + 4)) w16(p, r16(p + 4));
        if (r16(p + 2) > r16(p + 6)) w16(p + 2, r16(p + 6));
    }
    return 1;
}

/* For each slot: the best thing this vocation can wear, out of what's on and what's in the bag (bag 0:
 * only take off what the vocation can't wear). The old piece goes back in the bag; nothing changes if the
 * bag has no room for it. Equipment the table doesn't know is left alone. Returns the slots changed. */
static int gear_up(u32 c, int voc, int bag) {
    int changed = 0;
    for (int j = 0; j < 8; j++) {
        int slot = slot_order[j], cat = slot_cat[slot];
        int cur = (short)r16(c + EQUIP + 2 * slot);
        if (cur == 0) cur = -1;                            /* empty is -1 (ov003_02165278 writes 0xFFFF); 0 isn't an item either */
        int ck = it_find(cur);
        if (cur >= 0 && (ck < 0 || it_cat[ck] != cat)) continue;
        int best = -1; s64 bs = 0;
        if (can_equip(ck, voc)) { best = cur; bs = item_score(ck, voc); }
        if (bag) for (int i = 0; i < bag_n[cat]; i++) {
            int id = (short)r16(bag_ids[cat] + 2 * i);
            if (id < 0 || !r8(bag_cnt[cat] + i) || id == best) continue;
            int k = it_find(id);
            if (k < 0 || it_cat[k] != cat || !can_equip(k, voc)) continue;
            s64 s = item_score(k, voc);
            if (s > bs) { bs = s; best = id; }
        }
        if (best == cur) continue;
        if (cur >= 0 && !bag_add(cat, cur)) continue;
        if (best >= 0) bag_take(cat, best);
        w16(c + EQUIP + 2 * slot, (u16)best);
        if (slot == 9) w8(c + 0x56E, 1);                   /* as func_02052d7c does for accessories */
        changed++;
    }
    return changed;
}

/* ---- every buff on the party (Battle menu, in a battle): the game's own setters, as Buff / Oomph / Acceleratle /
 * Magic Barrier / Insulatle would call them (func_020877c0..func_02087e6c, in the order of the 3-bit levels at
 * current stats + 0x58), each asked for +4 and so left at the maximum of +2, with its flag, its turns and, for
 * breath, the resistances worked out again. Not Bounce: it would bounce the party's own healing too. */
static int pend_buffs;
static void do_buffs(void) {
    if (!pend_buffs) return;
    if (!in_battle()) { pend_buffs = 0; say("Only in a battle"); return; }
    u32 p[4]; int n = party(p), k = 0;
    for (int m = 0; m < n; m++) {
        u32 st = cur_primary(p[m]);
        if (!st || !r16(st)) continue;                      /* the fallen can't be buffed */
        /* called one by one: a table or a switch of these addresses compiles to absolute relocations, which
         * nothing rebases under ASLR (build.py refuses them). A busy game: try them all again next frame. */
        if (!game_call(fn_main_020877c0, st, 4) || !game_call(fn_main_020878b4, st, 4) || !game_call(fn_main_020879a8, st, 4) ||
            !game_call(fn_main_02087a9c, st, 4) || !game_call(fn_main_02087b90, st, 4) || !game_call(fn_main_02087c84, st, 4) ||
            !game_call(fn_main_02087d78, st, 4) || !game_call(fn_main_02087e6c, st, 4)) return;
        k++;
    }
    pend_buffs = 0;
    char t[120], *o = sput(t, "Every buff at its highest on ");
    o = iput(o, k); sput(o, k == 1 ? " party member" : " party members");
    say(k ? t : "No one in the party can be buffed right now");
}

/* asked for in the menu, done in the next frame (out of battle only) */
static int pend_voc[4], pend_best[4], pend_wait;   /* pend_voc: vocation + 1 (0: nothing asked) */
static void say_vocation(int m, u32 c, const char* extra, int n) {
    char t[160], *o = t;
    int v = voc_of(c);
    o = sput(o, "Party member "); o = iput(o, m + 1); o = sput(o, ": "); o = sput(o, voc_name(v));
    o = sput(o, " (level "); o = iput(o, r16(c + VOC_LEVEL + 2 * v)); o = sput(o, ")");
    if (extra) { o = sput(o, extra); o = iput(o, n); o = sput(o, n == 1 ? " piece changed" : " pieces changed"); }
    say(t);
}
static void do_pending(void) {
    int any_p = 0;
    for (int m = 0; m < 4; m++) any_p |= pend_voc[m] | pend_best[m];
    if (!any_p) return;
    if (in_battle()) { memset(pend_voc, 0, sizeof pend_voc); memset(pend_best, 0, sizeof pend_best); say("Not during a battle: try again after it"); return; }
    u32 p[4]; int n = party(p);
    for (int m = 0; m < 4; m++) {
        int v = pend_voc[m] - 1, best = pend_best[m] | (v >= 0 && S.auto_best);
        if (v < 0 && !best) continue;
        if (m >= n) { pend_voc[m] = pend_best[m] = 0; say_member("There's no party member ", m, " right now"); continue; }
        u32 o = p[m], c = char_of(o);
        if (v >= 0 && voc_of(c) != v) {
            w32(c + VOC_WORD, (u32)v);
            w16(c + VOC_USED, (u16)(r16(c + VOC_USED) | 1u << v));
            if (!r16(c + VOC_LEVEL + 2 * v)) w16(c + VOC_LEVEL + 2 * v, 1);   /* a vocation never used: level 1 */
        }
        int k = gear_up(c, voc_of(c), best);
        if (!recalc(o, c)) {                               /* the game is busy right now: try again next frame */
            if (++pend_wait < 300) return;
            memset(pend_voc, 0, sizeof pend_voc); memset(pend_best, 0, sizeof pend_best); pend_wait = 0;
            say("The game didn't let the cheats in for 5 seconds: try again"); return;
        }
        pend_wait = 0;
        pend_voc[m] = pend_best[m] = 0;
        say_vocation(m, c, best ? ", best gear on: " : k ? ", gear it can't use taken off: " : 0, k);
    }
}
static int shown_voc[4];
static void voc_ticks(void) {
    u32 p[4]; int n = party(p);
    for (int m = 0; m < 4; m++) {
        int v = m < n ? voc_of(char_of(p[m])) : -1;
        if (v == shown_voc[m]) continue;
        for (int k = 0; k <= N_VOC; k++) menu_check(ID_VOC0 + m * 15 + k, k == v);
        shown_voc[m] = v;
    }
    menu_check(ID_AUTO_BEST, S.auto_best);
}

/* ---- the menu */
static void ticks(void) {
    menu_check(ID_USE_ALL, S.use_all);
    menu_check(ID_UNNAMED, S.unnamed);
    menu_check(ID_COUP, S.coup);
    menu_check(ID_TENSION, S.tension);
    menu_check(ID_PERM_BUFFS, S.perm_buffs);
    menu_check(ID_CRIT, S.crit);
    menu_check(ID_COMBO, S.combo);
    menu_check(ID_PKJ, S.pkj == PKJ_ID);
    menu_check(ID_GEM, S.pkj == GEM_ID);
    menu_check(ID_NO_FLEE, S.no_flee);
    sk_ticks();
    voc_ticks();
    menu_check(ID_INF_HP, S.inf_hp);
    menu_check(ID_INF_MP, S.inf_mp);
    menu_check(ID_MAXSTATS, S.max_stats);
    menu_check(ID_FS0, S.first == 0);
    menu_check(ID_FS1, S.first == 1);
    menu_check(ID_FS2, S.first == 2);
    menu_check(ID_INFO, S.info);
    for (int m = 0; m < 4; m++) for (int s = 0; s < N_STAT; s++) menu_check(ID_EDIT0 + m * 10 + s, S.ov[m][s] >= 0);
}
static void* popup(void) { return CALL(fn_v, IMP_CreatePopupMenu)(); }
static void add(void* m, int id, const wchar* t) { CALL(fn_append, IMP_AppendMenuW)(m, 0, (u64)id, t); }
static void sub(void* m, void* s, const wchar* t) { CALL(fn_append, IMP_AppendMenuW)(m, 0x10, (u64)s, t); }
static void sep(void* m) { CALL(fn_append, IMP_AppendMenuW)(m, 0x800, 0, 0); }

/* called from menu_init just before the Help menu is made */
MS void build_cheats_menu(void* bar) {
    load();
    void* c = popup();
    sub(bar, c, u"&Cheats");

    void* party_m = popup();
    sub(c, party_m, u"&Party");
    add(party_m, ID_INF_HP, u"Infinite &HP");
    add(party_m, ID_INF_MP, u"Infinite &MP");
    add(party_m, ID_MAXSTATS, u"&Keep every stat at 999 (max HP and MP, attack, defence, agility, deftness, charm, magic)");
    add(party_m, ID_HEAL, u"&Fully heal the party now");
    sep(party_m);
    add(party_m, ID_USE_ALL, u"Everyone &knows every spell and ability, in any vocation");
    add(party_m, ID_UNNAMED, u"   ...including the &unnamed ones (maybe the item-only spells and abilities; experimental)");
    void* edit = popup();
    sub(party_m, edit, u"&Edit a member's stats");
    static const wchar mnames[] = u"Party member &1 (your hero)\0Party member &2\0Party member &3\0Party member &4\0";
    static const wchar snames[] = u"Max &HP...\0Max &MP...\0&Attack...\0&Defence...\0A&gility...\0De&ftness...\0&Charm...\0Magical m&ight...\0Magical m&ending...\0";
    const wchar* mn = mnames;
    for (int m = 0; m < 4; m++) {
        void* mm = popup();
        sub(edit, mm, mn);
        while (*mn) mn++; mn++;
        const wchar* sn = snames;
        for (int s = 0; s < N_STAT; s++) {
            add(mm, ID_EDIT0 + m * 10 + s, sn);
            while (*sn) sn++; sn++;
        }
        sep(mm);
        add(mm, ID_EDIT0 + m * 10 + 9, u"&Undo this member's edits");
    }

    /* vocations: member > the twelve (a tick on the current one) and their best gear */
    void* vm = popup();
    sub(party_m, vm, u"Change &vocation (any time, anywhere but in a battle; also the Guardian)");
    static const wchar vnames[] = u"G&uardian (the debug class: wears anything, uses every learned spell; may freeze the game)\0"
        u"&Warrior\0&Priest\0&Mage\0M&artial artist\0&Thief\0Mi&nstrel\0&Gladiator\0Arma&mentalist\0Pala&din\0&Sage\0&Luminary\0&Ranger\0";
    mn = mnames;
    for (int m = 0; m < 4; m++) {
        void* mm = popup();
        sub(vm, mm, mn);
        while (*mn) mn++; mn++;
        const wchar* vn = vnames;
        for (int v = 0; v <= N_VOC; v++) {
            add(mm, ID_VOC0 + m * 15 + v, vn); while (*vn) vn++; vn++;
            if (!v) sep(mm);                                /* the Guardian on its own above the twelve */
        }
        sep(mm);
        add(mm, ID_VOC0 + m * 15 + 13, u"&Equip the best gear from the bag for this vocation");
        shown_voc[m] = -2;                                  /* a new menu: tick again */
    }
    sep(vm);
    add(vm, ID_BEST_ALL, u"Equip the &best gear from the bag for everyone");
    add(vm, ID_AUTO_BEST, u"Also equip the best gear after every vocation &change");

    /* spells and abilities: member > spells / weapon skills / other skills > group > one per bit */
    void* learn = popup();
    sub(party_m, learn, u"Edit a member's &spells and abilities");
    mn = mnames;
    for (int m = 0; m < 4; m++) {
        void* mm = popup();
        sub(learn, mm, mn);
        while (*mn) mn++; mn++;
        void* parents[3] = { popup(), popup(), popup() };
        sub(mm, parents[0], u"&Spells");
        sub(mm, parents[1], u"Abilities from &weapon skills");
        sub(mm, parents[2], u"Abilities from &other skills");
        const wchar* gn = sk_group_names; const wchar* nm = sk_names;
        int k = 0;
        for (int g = 0; g < SK_NGROUP; g++) {
            void* gm = popup();
            sub(parents[sk_group_parent[g]], gm, gn);
            while (*gn) gn++; gn++;
            for (; k < SK_N && sk_group[k] == g; k++) {
                add(gm, ID_SK0 + m * 400 + sk_bit[k], nm);
                while (*nm) nm++; nm++;
            }
        }
        sep(mm);
        add(mm, ID_SKB + m * 10 + SKB_ALL_SPELLS, u"Learn every spell");
        add(mm, ID_SKB + m * 10 + SKB_NO_SPELLS, u"Forget every spell");
        add(mm, ID_SKB + m * 10 + SKB_ALL_ABIL, u"Learn every ability");
        add(mm, ID_SKB + m * 10 + SKB_NO_ABIL, u"Forget every ability");
        sep(mm);
        add(mm, ID_SKB + m * 10 + SKB_UNDO, u"&Undo my spell and ability edits for this member");
        S.shown_ok[m] = 0;                                  /* a new menu: tick everything again */
    }

    void* battle = popup();
    sub(c, battle, u"&Battle");
    void* fs = popup();
    sub(battle, fs, u"&First strike");
    add(fs, ID_FS0, u"&Normal");
    add(fs, ID_FS1, u"&Party always strikes first");
    add(fs, ID_FS2, u"&Enemies always strike first");
    add(battle, ID_INFO, u"&Display info: enemies' HP and MP in the title bar");
    add(battle, ID_COUP, u"Always ready for a &coup de gr\u00e2ce");
    add(battle, ID_TENSION, u"Always at maximum &tension (100)");
    add(battle, ID_CRIT, u"Party attacks and spells are always c&ritical hits");
    add(battle, ID_PKJ, u"Every monster battle is against &platinum king jewels (bosses stay as they are)");
    add(battle, ID_GEM, u"Every monster battle is against g&em slimes (bosses stay as they are)");
    add(battle, ID_NO_FLEE, u"Monsters ca&n't run away");
    add(battle, ID_ALL_BUFFS, u"&Give all buffs to the party now (attack, defence, agility, charm, magic, magic and breath resistance)");
    add(battle, ID_COMBO, u"Party attacks always get the ma&x combo bonus (x2, as a 4-hit combo)");
    add(battle, ID_PERM_BUFFS, u"&Buffs and debuffs last the whole battle, like the old games (both sides)");

    void* money = popup();
    sub(c, money, u"&Money and items");
    add(money, ID_GOLD, u"Max &gold now (9,999,999)");
    add(money, ID_BANK, u"Max gold in the &bank now (9,999,999)");
    add(money, ID_MEDALS, u"Max &mini medals now (999)");
    sep(money);
    add(money, ID_ITEMS99, u"&99 of every item you have");

    sep(c);
    add(c, ID_ALLOFF, u"&Turn off every cheat");
    add(c, ID_DIAG, u"&Diagnostics (what the cheats see)");
    add(c, ID_REC, u"&Record what the game does (choose again to stop and save cheats-record.txt)");
    add(c, ID_ABOUT, u"&What is this?");
    ticks();
}

/* ---- actions */
static void heal_all(void) {
    u32 p[4]; int n = party(p);
    for (int i = 0; i < n; i++) {
        u32 c = cur_primary(p[i]);
        if (!c || r16(c) == 0) continue;                   /* the fallen stay fallen (a church or a spell) */
        w16(c, r16(c + 4)); w16(c + 2, r16(c + 6));
        u32 b = base_primary(p[i]);
        if (b && r16(b)) { w16(b, r16(b + 4)); w16(b + 2, r16(b + 6)); }
    }
    say(n ? "Party fully healed" : "No party to heal here");
}
/* the bags: item IDs (halfwords) then counts (bytes), per kind of item */
static void items99(void) {
    static const u32 bags[] = {
        0x020F5DE8, 0x020F5F18, 152,   /* everyday items */
        0x020F5FB0, 0x020F61D0, 272,   /* weapons */
        0x020F62E0, 0x020F6340, 48,    /* shields */
        0x020F6370, 0x020F64F0, 192,   /* torso */
        0x020F65B0, 0x020F6670, 96,    /* legs */
        0x020F66D0, 0x020F67F0, 144,   /* head */
        0x020F6880, 0x020F6920, 80,    /* arms */
        0x020F6970, 0x020F6A50, 112,   /* feet */
        0x020F6AC0, 0x020F6B40, 64,    /* accessories */
    };
    int changed = 0;
    for (int b = 0; b < 9; b++) {
        u32 ids = bags[3 * b], cnt = bags[3 * b + 1], n = bags[3 * b + 2];
        for (u32 i = 0; i < n; i++)
            if (r16(ids + 2 * i) && r8(cnt + i) && r8(cnt + i) < 99) { w8(cnt + i, 99); changed++; }
    }
    char t[80], *o = t;
    o = sput(o, "99 of every item you have ("); o = iput(o, changed); o = sput(o, " changed)");
    say(t);
}
static void edit_stat(int m, int s) {
    char title[64], prompt[300], *o;
    o = sput(title, "Party member "); iput(o, m + 1);
    u32 p[4]; int n = party(p);
    o = prompt;
    char nm[32]; stat_name(s, nm);
    o = sput(o, nm); o = sput(o, " for party member "); o = iput(o, m + 1);
    if (m < n) { o = sput(o, " (now "); o = iput(o, stat_get(cur_primary(p[m]), s)); o = sput(o, ")"); }
    o = sput(o, ": type a number from 0 to 999. It stays until you undo it, even when a level-up or new equipment would change it. Leave it empty to undo.");
    char* r = ask_text(g_win, title, prompt);
    if (!r) return;
    int v;
    if (r[0] == 0) S.ov[m][s] = -1;
    else if (sparse(r, &v)) S.ov[m][s] = v > 999 ? 999 : v;
    CALL(fn_p, IMP_free)(r);
    save(); ticks();
}

MS void cheat_action(int id) {
    load();
    switch (id) {
    case ID_INF_HP: S.inf_hp = !S.inf_hp; say(S.inf_hp ? "Infinite HP on" : "Infinite HP off"); break;
    case ID_INF_MP: S.inf_mp = !S.inf_mp; say(S.inf_mp ? "Infinite MP on" : "Infinite MP off"); break;
    case ID_MAXSTATS: S.max_stats = !S.max_stats; say(S.max_stats ? "Every stat kept at 999" : "Stats back to normal at the next level-up or change of equipment"); break;
    case ID_HEAL: heal_all(); break;
    case ID_GOLD: w32(0x020F6D48, 9999999); say("Gold: 9,999,999"); break;
    case ID_BANK: w32(0x020F6D44, 9999999); say("Gold in the bank: 9,999,999"); break;
    case ID_MEDALS: w32(0x020F6D50, 999); say("Mini medals: 999"); break;
    case ID_ITEMS99: items99(); break;
    case ID_FS0: case ID_FS1: case ID_FS2:
        S.first = id - ID_FS0;
        say(S.first == 1 ? "The party always strikes first" : S.first == 2 ? "The enemies always strike first" : "First strike as usual");
        break;
    case ID_USE_ALL:
        if (S.use_all) { use_all_off(); say("Spells and abilities back to what each vocation and its skills give"); }
        else {
            S.use_all = 1;
            u32 p[4]; int n = party(p);
            for (int m = 0; m < n; m++) { u32 rec = record(p[m]); if (rec) use_all_apply(m, rec); }
            say("Everyone knows every spell and ability, in any vocation");
        }
        break;
    case ID_UNNAMED:
        if (S.unnamed) {                                    /* take back just the unnamed bits it added */
            u32 p[4]; int n = party(p);
            for (int m = 0; m < 4; m++) {
                u32 rec = m < n ? record(p[m]) : 0;
                for (int i = 0; i < N_LB; i++) {
                    u8 rm = (u8)(S.added[m][i] & ~sk_known[i] & ~S.on[m][i]);
                    if (rm && rec) w8(rec + LEARNED + i, (u8)(r8(rec + LEARNED + i) & ~rm));
                    S.added[m][i] &= sk_known[i];
                }
            }
            S.unnamed = 0; say("Unnamed spells and abilities off");
        } else {
            S.unnamed = 1;
            if (S.use_all) { u32 p[4]; int n = party(p); for (int m = 0; m < n; m++) { u32 rec = record(p[m]); if (rec) use_all_apply(m, rec); } }
            say(S.use_all ? "Unnamed spells and abilities on too" : "Unnamed ones on: turn on \"Everyone knows every spell and ability\" as well");
        }
        break;
    case ID_BEST_ALL: {
        if (in_battle()) { say("Not during a battle: try again after it"); return; }
        u32 p[4]; int n = party(p);
        for (int m = 0; m < n; m++) pend_best[m] = 1;
        if (!n) say("No party here");
        return;
    }
    case ID_AUTO_BEST: S.auto_best = !S.auto_best; say(S.auto_best ? "A vocation change also puts on the best gear from the bag" : "A vocation change only takes off what the new vocation can't use"); break;
    case ID_COUP: S.coup = !S.coup; say(S.coup ? "Coup de grace always ready in battles" : "Coup de grace as usual"); break;
    case ID_CRIT: S.crit = !S.crit; say(S.crit ? "The party's attacks and spells are always critical" : "Critical hits as usual"); break;
    case ID_ALL_BUFFS:
        if (!in_battle()) { say("Only in a battle"); return; }
        pend_buffs = 1; return;                             /* done in the next frame (do_buffs) */
    case ID_PKJ: S.pkj = S.pkj == PKJ_ID ? 0 : PKJ_ID; say(S.pkj ? "From the next battle on, the monsters are platinum king jewels (not bosses)" : "Monsters as usual"); break;
    case ID_GEM: S.pkj = S.pkj == GEM_ID ? 0 : GEM_ID; say(S.pkj ? "From the next battle on, the monsters are gem slimes (not bosses)" : "Monsters as usual"); break;
    case ID_NO_FLEE: S.no_flee = !S.no_flee; say(S.no_flee ? "Monsters can't run away" : "Monsters can run away again"); break;
    case ID_COMBO: S.combo = !S.combo; say(S.combo ? "Every party attack gets the full combo bonus" : "Combos as usual"); break;
    case ID_PERM_BUFFS: S.perm_buffs = !S.perm_buffs; say(S.perm_buffs ? "Buffs and debuffs last the whole battle" : "Buffs and debuffs wear off as usual"); break;
    case ID_TENSION: S.tension = !S.tension; say(S.tension ? "Tension always at 100 in battles" : "Tension as usual"); break;
    case ID_INFO: S.info = !S.info; say(S.info ? "Enemies' HP and MP show in the title bar during battles" : "Display info off"); break;
    case ID_ALLOFF:
        S.inf_hp = S.inf_mp = S.max_stats = S.first = S.info = S.coup = S.tension = S.auto_best = S.perm_buffs = S.crit = S.combo = S.pkj = S.no_flee = 0;
        for (int m = 0; m < 4; m++) for (int s = 0; s < N_STAT; s++) S.ov[m][s] = -1;
        if (S.use_all) use_all_off();
        for (int m = 0; m < 4; m++) sk_undo(m);
        say("Every cheat off");
        break;
    case ID_DIAG: diagnostics(); return;
    case ID_REC: record_toggle(); menu_check(ID_REC, recording); return;
    case ID_ABOUT:
        CALL(fn_msgbox, IMP_MessageBoxW)(0,
            u"Cheats for this copy of the game (added by a patch to the exe).\n\n"
            u"Party: infinite HP and MP, every stat kept at 999, a full heal, and editing one member's stats "
            u"(an edited stat stays until you undo it), and choosing which spells and abilities each member knows "
            u"(a tick means they know it now; click to learn or forget it, and it stays that way until you undo it), "
            u"or giving everyone every spell and ability whatever their vocation. "
            u"Changing any member's vocation at any time (not in a battle), and putting on the best gear in the bag for their vocation.\n"
            u"Battle: who strikes first, coup de gr\u00e2ce always ready, tension always at 100, buffs and debuffs that last the whole battle, critical hits and spells every time, the full combo bonus on every attack, every buff on the party at once, platinum king jewels or gem slimes in every monster battle, monsters that can't run away, and the enemies' HP and MP shown in the window's title bar.\n"

            u"Money and items: max gold, bank and mini medals, and 99 of every item you already have.\n\n"
            u"Your choices are kept in cheats.ini next to the game. Updating the game from the Help menu "
            u"replaces the patched exe and removes this menu.",
            u"Cheats", 0x40);
        return;
    default:
        if (id >= ID_SK0 && id < ID_SK0 + 4 * 400) { sk_toggle((id - ID_SK0) / 400, (id - ID_SK0) % 400); break; }
        if (id >= ID_SKB && id <= ID_SK_LAST) {
            int m = (id - ID_SKB) / 10, op = (id - ID_SKB) % 10;
            if (op <= SKB_UNDO) sk_bulk(m, op);
            break;
        }
        if (id >= ID_VOC0 && id < ID_VOC0 + 4 * 15) {
            int m = (id - ID_VOC0) / 15, v = (id - ID_VOC0) % 15;
            if (in_battle()) { say("Not during a battle: try again after it"); return; }
            if (v <= N_VOC) pend_voc[m] = v + 1;
            else if (v == 13) pend_best[m] = 1;
            return;                                         /* done in the next frame (cheat_frame) */
        }
        if (id >= ID_EDIT0 && id <= ID_LAST) {
            int m = (id - ID_EDIT0) / 10, s = (id - ID_EDIT0) % 10;
            if (s == 9) { for (int k = 0; k < N_STAT; k++) S.ov[m][k] = -1; say("Edits undone"); }
            else { edit_stat(m, s); return; }
        }
        break;
    }
    save(); ticks();
}

/* ---- coup de grace and tension: battle state in a member's current stats (char + 0x94 = eidako2's
 * record + 0x58, where the Action Replay codes write): tension level at +0x24 (4 = 100),
 * "super high tension" = bit 0x01 of status byte +0x17 (the other bits there are other buffs), coup de
 * grace = bits 0x98 of +0x3B. Bits are ORed in, not written whole, so other flags survive. Only while
 * the battle overlay (ov000) is in memory. */
static void battle_cheats(void) {
    if (!(S.coup || S.tension) || !in_battle()) return;
    u32 p[4]; int n = party(p);
    for (int m = 0; m < n; m++) {
        u32 c = char_of(p[m]); if (!c) continue;
        u32 b = c + BATTLE_STATS;
        if (r16(b) == 0) continue;                          /* the fallen can't use either */
        if (S.tension) {
            if (r8(b + 0x24) < 4) w8(b + 0x24, 4);
            if (!(r8(b + 0x17) & 0x01)) w8(b + 0x17, (u8)(r8(b + 0x17) | 0x01));
        }
        if (S.coup && (r8(b + 0x3B) & 0x98) != 0x98) w8(b + 0x3B, (u8)(r8(b + 0x3B) | 0x98));
    }
}

/* ---- buffs and debuffs that never wear off. A combatant's current stats (object + 0x138; a party member's
 * are char + 0x94) hold, per effect, a flag in the status word at +0x14, a duration at +0x60 + i and a
 * countdown at +0x83 + i (the setters, func_02087800.., write both). Each round the battle overlay
 * (ov000_021586xx) takes 1 off the countdown and ends the effect with chance T[countdown]
 * (0x02182AD4 / 0x02182BD4: 1, .875, .75, .625, ~0, 0, 0, 0). Keeping an active countdown at 6 makes it 5
 * after the tick, where the chance is 0. A countdown of 0 means the tick leaves it alone. The effects:
 * the six stat levels (attack, defence, agility, charm, magical might, magical mending: +0x6E..+0x73,
 * flags 0x400..0x8000), magic resistance (Magic Barrier, +0x74, 0x10000), breath resistance (Insulate,
 * +0x75, 0x20000) and Bounce (+0x61, 0x200). Ailments (sleep, confusion, poison, Fizzle, Dazzle...) aren't. */
static const u8 perm_slot[9] = { 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x61 };
static const u32 perm_flag[9] = { 0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000, 0x20000, 0x200 };
static void perm_buffs(void) {
    if (!S.perm_buffs || !in_battle()) return;
    for (int i = 0; i < N_OBJ; i++) {
        u32 o = object(i);
        /* a combatant: as the game's own lookup (func_0200fe68, used by the round tick) only flag 0x80.
         * v2.5 also wanted 0x100 ("in use"), which monsters don't have: their effects still wore off. */
        if (!o || !(r16(o) & 0x80)) continue;
        u32 st = cur_primary(o); if (!st) continue;
        u32 fl = r32(st + 0x14);
        for (int k = 0; k < 9; k++) {
            if (!(fl & perm_flag[k])) continue;
            u32 cd = st + perm_slot[k] + 0x23;
            u8 n = r8(cd);
            if (n >= 1 && n <= 5) w8(cd, 6);
        }
    }
}

/* ---- every frame */
static void apply_stats(u32 o, int m) {
    u32 ps[2] = { base_primary(o), cur_primary(o) };
    for (int k = 0; k < 2; k++) {
        u32 p = ps[k]; if (!p) continue;
        if (S.max_stats) for (int s = 0; s < N_STAT; s++) if (stat_get(p, s) != 999) stat_set(p, s, 999);
        for (int s = 0; s < N_STAT; s++) if (S.ov[m][s] >= 0 && stat_get(p, s) != S.ov[m][s]) stat_set(p, s, S.ov[m][s]);
        if (S.inf_hp && r16(p) && r16(p) != r16(p + 4)) w16(p, r16(p + 4));
        if (S.inf_mp && r16(p + 2) != r16(p + 6)) w16(p + 2, r16(p + 6));
    }
}
static void battle_info(void) {
    static int tick;
    if (++tick % 20) return;
    char t[160], *o = t; int k = 0;
    for (int i = 4; i < N_OBJ && k < 8; i++) {
        u32 ob = object(i);
        if (!ob || !(r16(ob) & 0x80) || (r16(ob) & 0x800)) continue;
        u32 c = cur_primary(ob); if (!c) continue;
        if (o - t > 120) break;
        o = sput(o, k ? "   " : "Enemies: ");
        o = iput(o, ++k); o = sput(o, ") HP "); o = iput(o, r16(c)); o = sput(o, "/"); o = iput(o, r16(c + 4));
        if (r16(c + 6)) { o = sput(o, " MP "); o = iput(o, r16(c + 2)); }
    }
    if (k) say(t);
}
MS void cheat_frame(void) {
    load();
    if (!valid(r32(GAMESTATE + 8)) && !valid(r32(GAMESTATE + 12))) return;      /* no game objects yet */
    int edits = 0;
    for (int m = 0; m < 4; m++) for (int s = 0; s < N_STAT; s++) edits |= S.ov[m][s] >= 0;
    if (S.inf_hp || S.inf_mp || S.max_stats || edits) {
        u32 p[4]; int n = party(p);
        for (int i = 0; i < n; i++) apply_stats(p[i], i);
    }
    {
        u32 p[4]; int n = party(p);
        int added = 0;
        for (int m = 0; m < n; m++) {
            u32 rec = record(p[m]); if (!rec) continue;
            if (any(S.on[m]) || any(S.off[m])) sk_apply(m, rec);
            if (S.use_all) added |= use_all_apply(m, rec);
        }
        if (added) save();                                  /* so turning it off later takes back the right bits */
    }
    do_pending();                          /* vocation changes and gear asked for in the menu */
    do_buffs();
    static int tick;
    if (++tick % 15 == 0) { sk_ticks(); voc_ticks(); }
    battle_cheats();                       /* level-ups, new vocations and skill points change them too */
    perm_buffs();
    if (S.info) battle_info();
}

/* ---- critical hits: ov000_02156cc4 (r1 = the attacker's combatant index, 0..3 the party; r2 the action,
 * r3 the number of hits) answers "is this hit critical?": an action flagged always-critical says yes, the
 * party rolls against CalculateCritRate (func_020747a4: deftness, accessories, books, skills), everyone
 * else against a monster rate. Its only callers are the damage code in ov024_021eb5d0. Spells go through
 * it too: func_02085ab8 (the "skill bonus" it feeds CalculateCritRate) gives actions of type 2 (r2 + 0x18,
 * bits 12..15: spells) the Spellcraft "critical spell rate up" bonus (ability 103, the only place the game
 * reads it), and nothing else in any overlay decides spell criticals. Our hook on its entry: non-zero = say
 * yes (r0 = 1) without rolling, for physical attacks and spells alike. */
MS int crit_force(u32* cpu) {
    load();
    crit_calls++;
    int spell = valid(cpu[2]) && ((r32(cpu[2] + 0x18) >> 12) & 0xF) == 2;
    crit_spells += spell;
    if (S.crit && cpu[1] <= 3) { crit_forced++; return 1; }
    return 0;
}

/* ---- combos: the battle struct ([ctx + 0x10]) keeps the combo at +0x8E83 (hits so far - 1), the side of the
 * last attacker at +0x8E82 (1 party, 2 enemies), its action at +0x8E52, target at +0x8E50, attacker at +0x8E58
 * (ov024_021ea584 counts it up or resets it). The damage code ov024_021e6a90 (r0 ctx, r1 attacker's combatant
 * index, r2 target, r3 action), for actions that can combo (action + 0x2C bit 27), reads that byte, caps it at
 * 3 and multiplies by {1, 1.2, 1.5, 2}[combo] (0x021FE778); the same value sets the "combo" display bits. Its
 * mem_read8 call there (x86 0x140A560A8) goes through hook_combo: the party (r9 = attacker 0..3) gets 3. */
MS int combo_fix(u32* cpu, int v) {
    load();
    combo_calls++;
    if (S.combo && cpu[9] <= 3) { combo_forced++; return 3; }
    return v;
}

/* ---- platinum king jewels. ov000_02165490 (r0 = battle ctx) sets a battle up: from the battle object
 * B = [ctx + 0x29C] it lists the enemy species (ov000_0215ed34: groups = bits 4..5 of B + 0x81B1, group k's
 * species at B + 0x81B4 + 24k, u16), loads their battle data (mon_btldata.nat) and makes each enemy
 * (ov000_02166070). Our hook on its entry rewrites the species of every group to the one picked (S.pkj): 242,
 * the platinum king jewel (mon_list.gp2; 296 is a second copy), or 185, the gem slime, unless a group is a
 * boss: story, grotto and legacy bosses all
 * have ids from 300 up (hexagoon 300, Wight Knight 301, Morag 302 ... Dragonlord 500, Nokturnus 769..). */
MS void pkj_battle(u32* cpu) {
    load();
    pkj_seen++;
    if (!S.pkj) return;
    u32 ctx = cpu[0];
    if (!valid(ctx)) return;
    u32 b = r32(ctx + 0x29C);
    if (!valid(b) || !valid(b + 0x8200)) return;
    int n = (r8(b + 0x81B1) >> 4) & 3;
    if (!n) return;
    for (int k = 0; k < n; k++) { int id = r16(b + 0x81B4 + 24 * k); if (!id || id >= 300) return; }
    for (int k = 0; k < n; k++) w16(b + 0x81B4 + 24 * k, (u16)S.pkj);
    pkj_done++;
}

/* ---- monsters that can't run away. ov024_021da670 (r0 ctx, r1 actor, r2 target, r3 action; reached through
 * the action handler table, so no direct callers) carries out a few self-targeted actions; for Flee
 * (actname.nat: action 225 "Flee", 917 the monsters' "Flee") it takes the actor out of the battle (its HP to
 * 0 and away). Our hook on its entry: a monster (combatant 4 and up) using Flee gets r0 = 0, "nothing
 * happened", without leaving. The party's own Flee command isn't this and is never touched. */
MS int flee_block(u32* cpu) {
    load();
    u32 act = cpu[3];
    if (!valid(act)) return 0;
    int id = (int)(r32(act + 4) & 0xFFF);
    if ((id != 225 && id != 917) || cpu[1] < 4) return 0;
    flee_seen++;
    if (!S.no_flee) return 0;
    flee_blocked++;
    return 1;
}

/* ---- first strike: replaces the call to fn_ov017_021970a0 (0 normal, 1 party first, 2 enemies first) */
MS void first_strike(u32* cpu) {
    load();
    if (S.first) cpu[0] = (u32)S.first;
    else orig_first_strike(cpu);
}
