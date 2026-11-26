/* Transmog: a character can look as if they wore other equipment, while the
 * equipment they really wear (and so every number in the game) stays the same.
 *
 * How (USA addresses; the European ROM's are 0x10 higher):
 *   func_02072afc(object, out part ids[11], out item ids[11]) lists the items a
 *     character's model is made from (0 torso, 1 legs, 2 face, 3-4 hairstyle,
 *     5 arms, 6 feet, 7 head, 8 weapon, 9 shield, 10 accessory);
 *   func_020deef4 fills the party member's item cache from that list; the
 *     game's numbers (attack, defence...) are added up from this cache too,
 *     so it must always hold what is really worn;
 *   func_02072e94 makes each part's model file name (p_<part><number>) from a
 *     cache entry (field, battles, cutscenes with the field models).
 *   func_ov017_0218f5a4 makes a party member's model again (when the
 *     equipment changes; transmog.c forces it after Reload, at the end of
 *     func_ov017_0218e8c8, the party's frame on the field).
 * The game's own code runs as it is (wrap.txt). When the cache is filled, the
 * game is asked to fill it once more with the chosen looks, to learn their
 * model numbers, and then again with the real items. While a part's file name
 * is made, the cache says the chosen look for that moment only. The save,
 * the cache and every number stay exactly as without transmog.
 *
 * Which character: object -> [object + 0x150] -> the party member's data
 * (0x964 bytes each, in GameState), with the equipment at +0x488
 * (func_02052e14) and the name as text at +0x3c. Characters are told apart by
 * name: the data belongs to a place in the party, not to a character, and a
 * name follows its character through the inn and to another save slot.
 *
 * Off while playing together (every PC runs every player's DS, and they must
 * stay identical in memory) and in replays of bug reports (replays stay exact).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "nds.h"
#include "transmog.h"

#define OBJ_CHARDATA   0x150   /* object -> the character's data */
#define CHAR_EQUIPMENT 0x488   /* in it: the equipment (func_02052e14) */
#define CHAR_NAME      0x3c    /* in it: the name, as text */
#define NAME_LEN       16
#define GAMESTATE      0x020f33d8u               /* GameState's instance (USA) */
#define MAX_CHARS      64

/* the save editor's slot numbers (1 torso, 2 legs, 5 arms, 6 feet, 7 head,
 * 8 weapon, 9 shield, 10 accessory) -> the index in the game's list above */
static const int SLOT_INDEX[TM_SLOTS] = { 0, 1, 5, 6, 7, 8, 9, 10 };
const char* const g_tm_slot_names[TM_SLOTS] = { "torso", "legs", "arms", "feet", "head", "weapon", "shield", "accessory" };
/* The item numbers each slot's equipment has (the thousands say the kind; as
 * the DQIX save editor lists them). A number of another kind would put, say,
 * a torso where the head goes: the game makes a broken model of it and the
 * character disappears, so such a look is ignored. */
static const int SLOT_LO[TM_SLOTS] = { 13000, 16000, 14000, 17000, 12000, 19000, 21000, 18000 };
static const int SLOT_HI[TM_SLOTS] = { 13999, 16999, 15999, 17999, 12999, 20999, 21999, 18999 };
static int fits(int slot, int item) { return item >= SLOT_LO[slot] && item <= SLOT_HI[slot]; }
/* A weapon's hundreds say its kind (190xx knives, 191xx hammers, 200xx
 * swords, 201xx fans, 202xx axes, 203xx whips, 204xx staves, 205xx spears,
 * 206xx clubs, 207xx boomerangs, 208xx claws, 209xx bows). In battle, how a
 * weapon is held, drawn and swung goes by the kind of the one really worn: a
 * whip is drawn only when it strikes, a bow is held in the other hand, and so
 * on. So a weapon look is used only on a weapon of its own kind (and not
 * bare-handed); other slots only need to fit. */
#define SLOT_WEAPON 5
static int usable(int slot, int look, int real) {
    if (look < 0 || !fits(slot, look)) return 0;
    return slot != SLOT_WEAPON || (fits(slot, real) && look / 100 == real / 100);
}
static int g_ignored;                /* looks in transmog.ini that don't fit their slot */
int transmog_ignored(void) { return g_ignored; }

typedef struct { uint8_t name[NAME_LEN + 1]; int16_t look[TM_SLOTS], worn[TM_SLOTS]; int seen; } Entry;   /* look: -1 = as worn */
static Entry g_tm[MAX_CHARS];
static int g_ntm;
static int g_on;                     /* the player's switch (Mods > Transmog) */
static int g_blocked;                /* playing together, or a replay */
static char g_path[512];
static int g_log = -1;

/* ---------------------------------------------------------------- names */
static int name_ok(const uint8_t* n) { return n[0] >= 32 && n[0] != '[' && n[0] != ']'; }

static Entry* find(const uint8_t* name, int create) {
    for (int i = 0; i < g_ntm; i++) if (!strncmp((const char*)g_tm[i].name, (const char*)name, NAME_LEN)) return &g_tm[i];
    if (!create || g_ntm >= MAX_CHARS || !name_ok(name)) return NULL;
    Entry* e = &g_tm[g_ntm++];
    memset(e->name, 0, sizeof e->name);
    strncpy((char*)e->name, (const char*)name, NAME_LEN);
    for (int s = 0; s < TM_SLOTS; s++) e->look[s] = e->worn[s] = -1;
    e->seen = 0;
    return e;
}

/* ---------------------------------------------------------------- transmog.ini
 *   [Riastea]
 *   torso     = 13621   # wears 13619
 *   legs      = worn    # wears 16214
 * One section per character, by name; "worn" (or a slot not listed) shows
 * what is worn, "empty" shows the slot as if nothing were worn there. "torso: 13621" works too. Item numbers are the ones the DQIX
 * save editor shows. */
int transmog_load(const char* path) {
    if (path) snprintf(g_path, sizeof g_path, "%s", path);
    for (int i = 0; i < g_ntm; i++) for (int s = 0; s < TM_SLOTS; s++) g_tm[i].look[s] = -1;   /* keep what was seen worn */
    g_ignored = 0;
    FILE* f = g_path[0] ? fopen(g_path, "r") : NULL;
    if (!f) return 0;
    char line[256];
    Entry* cur = NULL;
    int first = 1;
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        if (first && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;   /* UTF-8 BOM (Windows editors) */
        first = 0;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || !*p) continue;
        if (*p == '[') {
            char* q = strchr(p, ']');
            if (!q) { cur = NULL; continue; }
            *q = 0;
            char* q0 = p + 1;
            while (*q0 == ' ' || *q0 == '\t') q0++;
            while (q > q0 && (q[-1] == ' ' || q[-1] == '\t')) *--q = 0;
            uint8_t n[NAME_LEN + 1] = { 0 };
            strncpy((char*)n, q0, NAME_LEN);
            cur = find(n, 1);
            continue;
        }
        if (!cur) continue;
        /* slot = number (or "slot: number"); "worn" or nothing: as worn; # or ; starts a comment */
        char* c = strpbrk(p, "#;");
        if (c) *c = 0;
        char key[32] = { 0 }; int n = 0;
        if (sscanf(p, "%31[A-Za-z] %n", key, &n) != 1) continue;
        p += n;
        if (*p != '=' && *p != ':') continue;
        p++;
        while (isspace((unsigned char)*p)) p++;
        int v = -1;
        if (isdigit((unsigned char)*p)) v = atoi(p);
        else if (tolower((unsigned char)p[0]) == 'e' && tolower((unsigned char)p[1]) == 'm' && tolower((unsigned char)p[2]) == 'p'
                 && tolower((unsigned char)p[3]) == 't' && tolower((unsigned char)p[4]) == 'y') v = TM_EMPTY;
        for (char* k = key; *k; k++) *k = (char)tolower((unsigned char)*k);
        for (int s = 0; s < TM_SLOTS; s++) {
            if (strcmp(key, g_tm_slot_names[s])) continue;
            if (v >= 0 && !fits(s, v)) { g_ignored++; v = -1; }
            cur->look[s] = (int16_t)v;
        }
    }
    fclose(f);
    int looks = 0;
    for (int i = 0; i < g_ntm; i++) for (int s = 0; s < TM_SLOTS; s++) looks += g_tm[i].look[s] != -1;
    return looks;
}

int transmog_save(void) {
    if (!g_path[0]) return 0;
    FILE* f = fopen(g_path, "w");
    if (!f) return 0;
    fprintf(f, "# Transmog (Mods > Transmog): what each character looks like they wear.\n"
               "# Replace \"worn\" with the number of the item it should look like, e.g. torso = 13621,\n"
               "# save this file, then Mods > Reload transmog.ini (the party is dressed again at once).\n"
               "# Each slot takes its own kind: head 12xxx, torso 13xxx, arms 14xxx-15xxx, legs 16xxx,\n"
               "# feet 17xxx, accessory 18xxx, weapon 19xxx-20xxx, shield 21xxx. A slot nothing is worn in shows nothing.\n"
               "# A weapon look is used only on a weapon of the same kind (the same hundreds: a whip, 203xx, for a whip).\n"
               "# \"worn\": shows what is really worn; \"empty\": shows the slot as if nothing were worn there\n"
               "# (no helmet: the hair; no armour: what is under it). What is really worn (and every number) doesn't change.\n"
               "# Numbers: Mods > Item numbers for transmog (transmog_items.txt) lists them all, with names,\n"
               "# or copy another character's \"# wears\" ones below.\n");
    for (int i = 0; i < g_ntm; i++) {
        int any = g_tm[i].seen;
        for (int s = 0; s < TM_SLOTS; s++) any |= g_tm[i].look[s] != -1;
        if (!any) continue;
        fprintf(f, "\n[%s]\n", (const char*)g_tm[i].name);
        for (int s = 0; s < TM_SLOTS; s++) {
            char v[16];
            if (g_tm[i].look[s] >= 0) snprintf(v, sizeof v, "%d", g_tm[i].look[s]);
            else snprintf(v, sizeof v, "%s", g_tm[i].look[s] == TM_EMPTY ? "empty" : "worn");
            if (!g_tm[i].seen) { fprintf(f, "%-9s = %s\n", g_tm_slot_names[s], v); continue; }
            if (fits(s, g_tm[i].worn[s])) fprintf(f, "%-9s = %-6s  # wears %d\n", g_tm_slot_names[s], v, g_tm[i].worn[s]);   /* (the game lists stand-ins for empty slots) */
            else fprintf(f, "%-9s = %-6s  # wears nothing\n", g_tm_slot_names[s], v);
        }
    }
    fclose(f);
    return 1;
}

static int g_refresh;                /* the party's models are to be made again (transmog_refresh) */
static unsigned long long g_last_part, g_asked_at;   /* the frame a model part was last made; the last refresh asked for */
static int g_asked;                  /* a refresh was asked for and no part has been made since */
static uint32_t g_field;             /* the field the game last checked a model on (func_ov017_0218f5a4) */
void transmog_enable(int on) { g_on = on != 0; }
void transmog_refresh(void) { g_refresh = 1; }
void transmog_block(int blocked) { g_blocked = blocked != 0; }
int transmog_active(void) {
    if (getenv("DQIX_INPUT_FILE") && !getenv("DQIX_TRANSMOG_IN_REPLAYS")) return 0;   /* replays stay exact */
    return g_on && !g_blocked;
}

int transmog_get(const uint8_t* name, int slot) {
    Entry* e = find(name, 0);
    return e && slot >= 0 && slot < TM_SLOTS ? e->look[slot] : -1;
}
void transmog_set(const uint8_t* name, int slot, int item) {
    if (slot < 0 || slot >= TM_SLOTS || (item >= 0 && !fits(slot, item))) return;
    if (item < 0 && item != TM_EMPTY) item = -1;
    Entry* e = find(name, item != -1);
    if (e) e->look[slot] = (int16_t)item;
}

/* ---------------------------------------------------------------- the party, for the menu */
static uint32_t gamestate(void) { return GAMESTATE; }   /* the same in the European ROM (data after the code doesn't move) */

/* The party member's data of the game object at index i, or 0. */
static uint32_t object_chardata(int i) {
    if (i < 0 || i >= 0xe9) return 0;
    uint32_t obj = mem_read32(gamestate() + 8 + 4u * (uint32_t)i);
    if ((obj >> 24) != 0x02) return 0;
    uint32_t d = mem_read32(obj + OBJ_CHARDATA);
    if ((d >> 24) != 0x02) return 0;
    return d;
}
static void read_name(uint32_t d, uint8_t* name) {
    memset(name, 0, NAME_LEN + 1);
    for (int i = 0; i < NAME_LEN; i++) { name[i] = mem_read8(d + CHAR_NAME + (uint32_t)i); if (!name[i]) break; }
}

/* ---------------------------------------------------------------- the hooks
 * Look-list index (func_02072afc's order) -> the index in the equipment
 * array at +0x488 (torso, legs, face, hairstyle, arms, feet, head, weapon,
 * shield, accessory: one entry for the hairstyle, where the list has two). */
static int equip_index(int k) { return k <= 3 ? k : k - 1; }

/* What an item looks like, as the game's item cache (11 entries of 0x20
 * bytes at +0x194 in the party member's data, one per look-list item) keeps
 * it: +0x10 packs the model numbers (bits 0-9 for men, 10-19 for women, 999:
 * the other one) and the part letter (bits 20-27); +0x18 is the item number.
 * Learnt from the game itself (fill_items below), so nothing about the items
 * is kept in this project. */
#define CACHE      0x194
#define ENTRY      0x20
#define N_LOOK     11
static uint32_t g_looks[0x8000];          /* item number -> its cache +0x10 word */
static uint8_t g_looks_known[0x8000];
static int g_nknown;
static int known_look(int item, uint32_t* look) {
    if (item < 0 || item >= 0x8000 || !g_looks_known[item]) return 0;
    *look = g_looks[item];
    return 1;
}
static void learn(int item, uint32_t look) {
    if (item < 0 || item >= 0x8000) return;
    if (!g_looks_known[item]) g_nknown++;
    g_looks[item] = look; g_looks_known[item] = 1;
}

/* "empty": a slot as the game makes it when nothing is worn there
 * (func_02072afc): the look list then has -1 for the head, the weapon, the
 * shield and the accessory, and stand-ins for the body: 1000 for the torso,
 * 8001 for the legs, 994 for the feet, and for the arms the torso's sleeves
 * (the word after the equipment, +0x49e) or 8010. Their cache entries are
 * learnt whole from the game (+0x08..+0x1f; the -1 one is the same for
 * everyone), and in the equipment the slot says -1, as when it is empty. */
#define EMPTY_WORDS 6                     /* +0x08..+0x1f of a cache entry */
static int empty_item(int k, uint32_t d) {
    switch (k) {
    case 0: return 1000;
    case 1: return 8001;
    case 6: return 994;
    case 5: { int sleeves = (int16_t)mem_read16(d + CHAR_EQUIPMENT + 0x16); return sleeves >= 0 ? sleeves : 8010; }
    default: return -1;
    }
}
typedef struct { int item; uint32_t w[EMPTY_WORDS]; } EmptyEntry;
static EmptyEntry g_empty[64];
static int g_nempty;
static const EmptyEntry* known_empty(int item) {
    for (int i = 0; i < g_nempty; i++) if (g_empty[i].item == item) return &g_empty[i];
    return NULL;
}
static void learn_empty(int item, uint32_t ent) {
    if (known_empty(item) || g_nempty >= (int)(sizeof g_empty / sizeof g_empty[0])) return;
    EmptyEntry* x = &g_empty[g_nempty++];
    x->item = item;
    for (int j = 0; j < EMPTY_WORDS; j++) x->w[j] = mem_read32(ent + 8 + 4u * (uint32_t)j);
}

/* The party member (game object index 0..) whose data holds addr, or 0. */
static uint32_t chardata_at(uint32_t base, uint32_t off) {
    for (int i = 0; i < 16; i++) { uint32_t d = object_chardata(i); if (d && d + off == base) return d; }
    return 0;
}

/* Tests: DQIX_TRANSMOG_RELOAD="frame:file,frame:file,..." reads each file
 * from its frame on, like Mods > Reload transmog.ini while playing. */
static void test_reload(void) {
    static int n = -1, next; static int at[64]; static char file[64][128];
    if (n < 0) {
        n = 0;
        const char* e = getenv("DQIX_TRANSMOG_RELOAD");
        while (e && *e && n < 64) {
            if (sscanf(e, "%d:%127[^,]", &at[n], file[n]) != 2) break;
            n++;
            e = strchr(e, ','); if (e) e++;
        }
    }
    while (next < n && g_frame >= (unsigned long long)at[next]) {
        if (!strncmp(file[next], "write=", 6)) {               /* "frame:write=file": as Choose the looks */
            snprintf(g_path, sizeof g_path, "%s", file[next] + 6);
            transmog_load(NULL); transmog_save();
            next++;
            continue;
        }
        int looks = transmog_load(file[next]);
        if (g_log) fprintf(stderr, "[f%llu] transmog: reloaded %s: %d looks\n", (unsigned long long)g_frame, file[next], looks);
        transmog_refresh();
        next++;
    }
}

/* func_02072afc: the 11 items a character's model is made from. The game
 * also fills the item cache from this list (and its numbers come from the
 * cache), so the list itself is left as it is; this only notes what each
 * character wears, for transmog.ini. */
void wrap_look_items_orig(Cpu* c);
void wrap_look_items(Cpu* c) {
    int index = (int)c->r[0];
    uint32_t out = c->r[2], lr = c->r[14];
    wrap_look_items_orig(c);
    if (g_log < 0) {
        g_log = getenv("DQIX_TRANSMOGLOG") != NULL;
        const char* t = getenv("DQIX_TRANSMOG");                /* tests: DQIX_TRANSMOG=transmog.ini (on) */
        if (t && *t) { transmog_load(t); g_on = 1; }
    }
    test_reload();
    uint32_t d = object_chardata(index);
    if (!d) return;
    uint8_t name[NAME_LEN + 1];
    read_name(d, name);
    Entry* e = find(name, 1);                                   /* remembers what they wear, for transmog.ini */
    if (!e) return;
    e->seen = 1;
    for (int s = 0; s < TM_SLOTS; s++) e->worn[s] = (int16_t)mem_read16(out + 2u * (uint32_t)SLOT_INDEX[s]);
    if (g_log) {
        fprintf(stderr, "[f%llu] transmog: object %d (%s), lr %08x, items", (unsigned long long)g_frame, index, (const char*)name, lr);
        for (int i = 0; i < N_LOOK; i++) fprintf(stderr, " %d", (int16_t)mem_read16(out + 2u * (uint32_t)i));
        fprintf(stderr, "%s\n", transmog_active() ? "" : " (transmog off)");
    }
}

/* func_020deef4(cache, item data, ?, item list, [sp] count): fills a party
 * member's item cache from a look list (when a save is loaded, and when a
 * model is made again: after an equipment change, or after Reload
 * transmog.ini, see wrap_party_frame). Afterwards, while the item data is at
 * hand, the game is asked once more with the chosen looks not known yet, each
 * in its own slot, to learn what they look like; then the cache is filled
 * again from the real list, so it ends exactly as the game left it (the
 * game's numbers come from it). */
void wrap_fill_items_orig(Cpu* c);
static void probe(Cpu* c, const Cpu* in, uint32_t base, uint32_t list, const int16_t* ids) {
    for (int k = 0; k < N_LOOK; k++) mem_write16(list + 2u * (uint32_t)k, (uint16_t)ids[k]);
    *c = *in; wrap_fill_items_orig(c);
    for (int k = 0; k < N_LOOK; k++)
        if (ids[k] >= 0 && (int16_t)mem_read16(base + (uint32_t)(k * ENTRY) + 0x18) == ids[k])
            learn(ids[k], mem_read32(base + (uint32_t)(k * ENTRY) + 0x10));
}
void wrap_fill_items(Cpu* c) {
    Cpu in = *c;
    wrap_fill_items_orig(c);
    if (!transmog_active()) return;
    uint32_t base = in.r[0], list = in.r[3];
    if ((int)mem_read32(in.r[13]) != N_LOOK) return;
    uint32_t d = chardata_at(base, CACHE);
    if (!d) return;
    uint8_t name[NAME_LEN + 1];
    read_name(d, name);
    Entry* e = find(name, 0);
    if (!e) return;
    int16_t real[N_LOOK], ids[N_LOOK];
    int need = 0;
    for (int k = 0; k < N_LOOK; k++) ids[k] = real[k] = (int16_t)mem_read16(list + 2u * (uint32_t)k);
    for (int k = 0; k < N_LOOK; k++) if ((int16_t)mem_read16(base + (uint32_t)(k * ENTRY) + 0x18) == real[k] && real[k] == empty_item(k, d))
        learn_empty(real[k], base + (uint32_t)(k * ENTRY));     /* a slot really empty: the game's own entry */
    int probe_empty[N_LOOK] = { 0 };
    for (int sl = 0; sl < TM_SLOTS; sl++) {
        uint32_t dummy; int item = e->look[sl], k = SLOT_INDEX[sl];
        if (item == TM_EMPTY) {
            int t = empty_item(k, d);
            if (!known_empty(t)) { ids[k] = (int16_t)t; probe_empty[k] = 1; need = 1; }
        } else if (usable(sl, item, real[k]) && !known_look(item, &dummy)) { ids[k] = (int16_t)item; need = 1; }
    }
    if (!need) return;
    Cpu out = *c;
    probe(c, &in, base, list, ids);
    for (int k = 0; k < N_LOOK; k++)
        if (probe_empty[k] && (int16_t)mem_read16(base + (uint32_t)(k * ENTRY) + 0x18) == ids[k]) learn_empty(ids[k], base + (uint32_t)(k * ENTRY));
    for (int k = 0; k < N_LOOK; k++) mem_write16(list + 2u * (uint32_t)k, (uint16_t)real[k]);
    *c = in; wrap_fill_items_orig(c);                          /* the real items again */
    *c = out;
    if (g_log) fprintf(stderr, "[f%llu] transmog: learnt the looks for %s (%d known)\n", (unsigned long long)g_frame, (const char*)name, g_nknown);
}

/* func_02072e94(out name, object index, look-list index, cache entry,
 * [sp] the cache): makes the file name of one part of a character's model
 * (p_<part><number>) from the cache. Only while it runs, the cache entries
 * (and the equipment numbers it checks, like "no arm armour: the torso's
 * sleeves") say the chosen looks; then everything is put back. */
void wrap_part_name_orig(Cpu* c);
void wrap_part_name(Cpu* c) {
    test_reload();
    g_last_part = g_frame; g_asked = 0;                        /* models are being made (wrap_party_frame waits) */
    if (!transmog_active()) { wrap_part_name_orig(c); return; }
    uint32_t base = mem_read32(c->r[13]);
    uint32_t d = object_chardata((int)c->r[1]);
    /* In battle, a whip's or a bow's second model (p_w<n>a: the whip
     * uncoiled as it strikes, the bow drawn) is named with no cache, only
     * the weapon's own entry (func_ov025_021e63b0, through func_02054000):
     * that entry is in the same cache. */
    int no_cache = 0;
    if (d && !base && c->r[2] < N_LOOK && c->r[3] == d + CACHE + c->r[2] * ENTRY) { base = d + CACHE; no_cache = 1; }
    Entry* e = NULL;
    if (d && d + CACHE == base) { uint8_t name[NAME_LEN + 1]; read_name(d, name); e = find(name, 0); }
    if (!e) { wrap_part_name_orig(c); return; }
    uint32_t saved[N_LOOK][EMPTY_WORDS]; uint16_t saved_eq[N_LOOK]; int patched[N_LOOK] = { 0 };
    for (int s = 0; s < TM_SLOTS; s++) {
        uint32_t look; int item = e->look[s], k = SLOT_INDEX[s];
        uint32_t ent = base + (uint32_t)(k * ENTRY), eq = d + CHAR_EQUIPMENT + 2u * (uint32_t)equip_index(k);
        const EmptyEntry* empty = NULL;
        if (item == TM_EMPTY) {
            empty = known_empty(empty_item(k, d));
            /* (the whip or bow a battle names alone always has a model) */
            if (!empty || (no_cache && empty->item < 0)) continue;
        } else if (!usable(s, item, (int16_t)mem_read16(ent + 0x18)) || !known_look(item, &look)) continue;
        for (int j = 0; j < EMPTY_WORDS; j++) saved[k][j] = mem_read32(ent + 8 + 4u * (uint32_t)j);
        saved_eq[k] = mem_read16(eq);
        if (empty) {
            for (int j = 0; j < EMPTY_WORDS; j++) mem_write32(ent + 8 + 4u * (uint32_t)j, empty->w[j]);
            mem_write16(eq, 0xFFFF);
        } else { mem_write32(ent + 0x10, look); mem_write16(ent + 0x18, (uint16_t)item); mem_write16(eq, (uint16_t)item); }
        patched[k] = empty ? 2 : 1;
    }
    wrap_part_name_orig(c);
    for (int k = 0; k < N_LOOK; k++) if (patched[k]) {
        uint32_t ent = base + (uint32_t)(k * ENTRY), eq = d + CHAR_EQUIPMENT + 2u * (uint32_t)equip_index(k);
        if (patched[k] == 2) for (int j = 0; j < EMPTY_WORDS; j++) mem_write32(ent + 8 + 4u * (uint32_t)j, saved[k][j]);
        else { mem_write32(ent + 0x10, saved[k][2]); mem_write16(ent + 0x18, (uint16_t)saved[k][4]); }   /* (+0x10, +0x18) */
        mem_write16(eq, saved_eq[k]);
    }
}

/* func_ov017_0218f5a4(field, object, 0, 0, [sp] force): makes a party
 * member's model again if the items it should be made of aren't the ones it
 * is made of (or when forced). The game calls it when a save is loaded and
 * when the equipment changes; it is noted here which field it was called
 * with. */
void wrap_model_check_orig(Cpu* c);
void wrap_model_check(Cpu* c) {
    g_field = c->r[0];
    wrap_model_check_orig(c);
}

/* func_ov017_0218f064's neighbour func_ov017_0218e8c8(field): the party on
 * the field, once a frame. After a change of looks (Reload transmog.ini,
 * Mods > Transmog), at its end the models of the party are made again: the
 * check above, forced, for each member, on the field the game checks on
 * itself; the game does the rest, as when the equipment changes. Not while
 * playing together or in replays (transmog_active). */
void wrap_party_frame_orig(Cpu* c);
void wrap_party_frame(Cpu* c) {
    uint32_t field = c->r[0];
    wrap_party_frame_orig(c);
    test_reload();
    if (!g_refresh || g_blocked || (getenv("DQIX_INPUT_FILE") && !getenv("DQIX_TRANSMOG_IN_REPLAYS"))) return;
    if (!g_field || field != g_field) return;                  /* not a field the game has checked models on */
    /* One at a time: the members' models are made one after another (about
     * 1.5 s for the party); asked again meanwhile, the game drops it for a
     * member it is making, which would keep the look before. So wait until
     * no part has been made for half a second (or, if nothing was made after
     * the last time it was asked, for 5 s). */
    if (g_asked ? g_frame - g_asked_at < 300 : g_frame - g_last_part < 30) return;
    g_refresh = 0;
    g_asked = 1; g_asked_at = g_frame;
    Cpu saved = *c;
    for (int i = 0; i < 4; i++) {
        if (!object_chardata(i)) continue;
        *c = saved;
        c->r[13] = saved.r[13] - 8;                            /* the 5th argument, on the stack */
        mem_write32(c->r[13], 1);                              /* force */
        c->r[0] = field; c->r[1] = (uint32_t)i; c->r[2] = 0; c->r[3] = 0;
        wrap_model_check_orig(c);
        if (g_log) fprintf(stderr, "[f%llu] transmog: made party member %d's model again\n", (unsigned long long)g_frame, i);
    }
    *c = saved;
}
