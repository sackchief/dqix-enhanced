/* Transmog: the list of item numbers (Mods > Item numbers), written from the
 * player's own ROM, in the game's language. Nothing about items is kept in
 * this project: the names come from the game's own file,
 * data/prm/itemname.gp2, the same one the game shows its item names from.
 *
 * itemname.gp2 is a Level-5 GPC archive ("GPC2"): a header (lengths in words),
 * a compressed table of the inner files (CRC32 of the name, offset, size), and
 * the files, each compressed: a word giving the method (bits 0-2: 0 none,
 * 1 LZ10, 2 Huffman 4-bit, 3 Huffman 8-bit, 4 RLE) and the size (bits 3-31).
 * It holds itemname_en.nat, _fr, _de, _it and _es: a word (bits 0-11: the
 * number of items), then 16 bytes an item (the offsets of its name and its
 * plural in the texts that follow, a word of grammar, the item number), then
 * the texts: lower case, with <'e> for an accent and the like, and in German
 * [adjm] and the like for grammar (an adjective's ending, case endings). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "transmog.h"

/* ---------------------------------------------------------------- the ROM's files */
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* A file of the ROM's file system by its path, or NULL. */
static const uint8_t* rom_file(const char* path, uint32_t* len) {
    if (!g_rom || g_rom_size < 0x200) return NULL;
    uint32_t fnt = rd32(g_rom + 0x40), fnt_len = rd32(g_rom + 0x44), fat = rd32(g_rom + 0x48), fat_len = rd32(g_rom + 0x4c);
    if (fnt + fnt_len > g_rom_size || fat + fat_len > g_rom_size || fnt_len < 8) return NULL;
    uint32_t dir = 0xF000;                                     /* the root */
    for (;;) {
        const char* slash = strchr(path, '/');
        size_t want = slash ? (size_t)(slash - path) : strlen(path);
        uint32_t d = dir & 0xFFF;
        if (8 * d + 8 > fnt_len) return NULL;
        uint32_t sub = fnt + rd32(g_rom + fnt + 8 * d), id = rd16(g_rom + fnt + 8 * d + 4), found = 0, is_dir = 0, next = 0;
        while (sub < fnt + fnt_len) {
            uint8_t t = g_rom[sub++];
            if (!t) break;
            uint32_t n = t & 0x7F;
            if (sub + n > fnt + fnt_len) return NULL;
            int match = n == want && !memcmp(g_rom + sub, path, want);
            sub += n;
            if (t & 0x80) {                                    /* a folder: its number follows */
                if (match) { found = 1; is_dir = 1; next = rd16(g_rom + sub); break; }
                sub += 2;
            } else {
                if (match) { found = 1; next = id; break; }
                id++;
            }
        }
        if (!found) return NULL;
        if (!slash) {
            if (is_dir || 8 * next + 8 > fat_len) return NULL;
            uint32_t a = rd32(g_rom + fat + 8 * next), b = rd32(g_rom + fat + 8 * next + 4);
            if (b < a || b > g_rom_size) return NULL;
            *len = b - a;
            return g_rom + a;
        }
        if (!is_dir) return NULL;
        dir = next;
        path = slash + 1;
    }
}

/* ---------------------------------------------------------------- Level-5 compression */
/* Unpacks one compressed block; a new buffer (free it), or NULL. */
static uint8_t* l5_unpack(const uint8_t* src, uint32_t srclen, uint32_t* outlen) {
    if (srclen < 4) return NULL;
    uint32_t h = rd32(src), method = h & 7, size = h >> 3, i = 4, o = 0;
    if (size > (16u << 20)) return NULL;
    uint8_t* out = malloc(size ? size : 1);
    if (!out) return NULL;
    if (method == 0) {
        if (srclen - 4 < size) goto bad;
        memcpy(out, src + 4, size);
    } else if (method == 1) {                                  /* LZ10 */
        while (o < size) {
            if (i >= srclen) goto bad;
            uint8_t flags = src[i++];
            for (int b = 0; b < 8 && o < size; b++) {
                if (flags & (0x80 >> b)) {
                    if (i + 2 > srclen) goto bad;
                    uint32_t x = (uint32_t)src[i] << 8 | src[i + 1]; i += 2;
                    uint32_t n = (x >> 12) + 3, disp = (x & 0xFFF) + 1;
                    if (disp > o) goto bad;
                    for (uint32_t k = 0; k < n && o < size; k++, o++) out[o] = out[o - disp];
                } else {
                    if (i >= srclen) goto bad;
                    out[o++] = src[i++];
                }
            }
        }
    } else if (method == 2 || method == 3) {                   /* Huffman, 4 or 8 bits a leaf */
        int bits = method == 2 ? 4 : 8;
        if (i >= srclen) goto bad;
        uint32_t tree = i, tree_len = ((uint32_t)src[i] + 1) * 2, pos = i + tree_len, cur = 1, val = 0, vbits = 0;
        if (pos > srclen) goto bad;
        while (o < size) {
            if (pos + 4 > srclen) goto bad;
            uint32_t word = rd32(src + pos); pos += 4;
            for (int b = 0; b < 32 && o < size; b++) {
                uint32_t bit = word >> (31 - b) & 1, n = src[tree + cur];
                uint32_t child = (cur & ~1u) + (n & 0x3F) * 2 + 2 + bit;
                if (child >= tree_len) goto bad;
                if (n & (0x80 >> bit)) {
                    val |= (uint32_t)src[tree + child] << vbits; vbits += (uint32_t)bits;
                    if (vbits == 8) { out[o++] = (uint8_t)val; val = 0; vbits = 0; }
                    cur = 1;
                } else cur = child;
            }
        }
    } else if (method == 4) {                                  /* RLE */
        while (o < size) {
            if (i >= srclen) goto bad;
            uint8_t f = src[i++];
            if (f & 0x80) {
                if (i >= srclen) goto bad;
                for (uint32_t k = 0, n = (f & 0x7Fu) + 3; k < n && o < size; k++) out[o++] = src[i];
                i++;
            } else {
                for (uint32_t k = 0, n = f + 1u; k < n && o < size; k++) { if (i >= srclen) goto bad; out[o++] = src[i++]; }
            }
        }
    } else goto bad;
    *outlen = size;
    return out;
bad:
    free(out);
    return NULL;
}

static uint32_t crc32_of(const char* s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* An inner file of a GPC archive, unpacked (free it), or NULL. */
static uint8_t* gpc_file(const uint8_t* g, uint32_t glen, const char* name, uint32_t* outlen) {
    if (glen < 20 || memcmp(g, "GPC2", 4)) return NULL;
    uint32_t count = rd16(g + 4) & 0xFFF, hl = rd16(g + 6) * 4u, il = rd16(g + 8) * 4u, first = rd16(g + 10) * 4u;
    if (hl + il > glen) return NULL;
    uint32_t infolen;
    uint8_t* info = l5_unpack(g + hl, il, &infolen);
    if (!info) return NULL;
    uint32_t want = crc32_of(name);
    uint8_t* out = NULL;
    for (uint32_t k = 0; k < count && 12 * k + 12 <= infolen; k++) {
        if (rd32(info + 12 * k) != want) continue;
        uint32_t off = (rd32(info + 12 * k + 4) & 0xFFFFFF) * 4 + first, size = rd32(info + 12 * k + 8) & 0xFFFFFF;
        if (off < glen && size <= glen - off) out = l5_unpack(g + off, size, outlen);
        break;
    }
    free(info);
    return out;
}

/* ---------------------------------------------------------------- the names */
/* One of the game's texts as UTF-8: <'e> é and the like, <1> an apostrophe,
 * [grammar] left out, the first letter a capital. */
static void decode_name(const char* s, char* out, size_t cap) {
    static const struct { const char* tag; const char* lo; const char* up; } T[] = {
        { "'a", "\xc3\xa1", "\xc3\x81" }, { "'e", "\xc3\xa9", "\xc3\x89" }, { "'i", "\xc3\xad", "\xc3\x8d" },
        { "'o", "\xc3\xb3", "\xc3\x93" }, { "'u", "\xc3\xba", "\xc3\x9a" },
        { "'A", "\xc3\x81", "\xc3\x81" }, { "'E", "\xc3\x89", "\xc3\x89" }, { "'O", "\xc3\x93", "\xc3\x93" },
        { "`a", "\xc3\xa0", "\xc3\x80" }, { "`e", "\xc3\xa8", "\xc3\x88" }, { "`u", "\xc3\xb9", "\xc3\x99" },
        { "^a", "\xc3\xa2", "\xc3\x82" }, { "^e", "\xc3\xaa", "\xc3\x8a" }, { "^i", "\xc3\xae", "\xc3\x8e" },
        { "^o", "\xc3\xb4", "\xc3\x94" }, { "^u", "\xc3\xbb", "\xc3\x9b" },
        { ":a", "\xc3\xa4", "\xc3\x84" }, { ":e", "\xc3\xab", "\xc3\x8b" }, { ":i", "\xc3\xaf", "\xc3\x8f" },
        { ":o", "\xc3\xb6", "\xc3\x96" }, { ":u", "\xc3\xbc", "\xc3\x9c" },
        { ":A", "\xc3\x84", "\xc3\x84" }, { ":U", "\xc3\x9c", "\xc3\x9c" },
        { "~n", "\xc3\xb1", "\xc3\x91" }, { "ss", "\xc3\x9f", "\xc3\x9f" }, { "oe", "\xc5\x93", "\xc5\x92" },
        { ",c", "\xc3\xa7", "\xc3\x87" }, { ",", ",", "," },
        { "1", "'", "'" }, { "6", "'", "'" }, { "9", "'", "'" }, { "66", "\"", "\"" }, { "99", "\"", "\"" },
    };
    size_t o = 0;
    int first = 1;
    while (*s && o + 4 < cap) {
        if (*s == '[') {                                       /* German grammar: the name alone, with no article */
            const char* e = strchr(s, ']');
            if (e) {
                if (!strncmp(s, "[adjm]", 6)) out[o++] = 'r';  /* Gro<ss>e[adjm] Helm: Grosser Helm */
                else if (!strncmp(s, "[adjn]", 6)) out[o++] = 's';   /* Kesse[adjn] Kopftuch: Kesses Kopftuch */
                s = e + 1;                                     /* [adjf], [adjp], [-] (a place to break), case endings */
                continue;
            }
        }
        if (*s == '<') {
            const char* e = strchr(s, '>');
            if (e && e - s - 1 <= 3) {
                size_t n = (size_t)(e - s - 1);
                const char* rep = NULL;
                for (size_t k = 0; k < sizeof T / sizeof T[0]; k++)
                    if (strlen(T[k].tag) == n && !memcmp(T[k].tag, s + 1, n)) { rep = first ? T[k].up : T[k].lo; break; }
                if (rep) {
                    size_t rl = strlen(rep);
                    if (o + rl + 1 >= cap) break;
                    memcpy(out + o, rep, rl); o += rl;
                    s = e + 1; first = 0;
                    continue;
                }
            }
        }
        char ch = *s++;
        if (first && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (ch != ' ' || o) out[o++] = ch;
        first = 0;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static const int LIST_LO[TM_SLOTS] = { 13000, 16000, 14000, 17000, 12000, 19000, 21000, 18000 };
static const int LIST_HI[TM_SLOTS] = { 13999, 16999, 15999, 17999, 12999, 20999, 21999, 18999 };

typedef struct { int id; const char* text; } Item;
static int by_id(const void* a, const void* b) { return ((const Item*)a)->id - ((const Item*)b)->id; }

int transmog_write_items(const char* path, int language) {
    static const char* const CODE[] = { "en", "en", "fr", "de", "it", "es" };
    static const char* const NAME[] = { "English", "English", "French", "German", "Italian", "Spanish" };
    if (language < 1 || language > 5) language = 1;
    if (g_rom_size >= 16 && g_rom[15] == 'E' && (language == 3 || language == 4)) language = 1;   /* the USA game: English, French, Spanish */
    uint32_t glen;
    const uint8_t* g = rom_file("data/prm/itemname.gp2", &glen);
    if (!g) return -1;
    char file[32];
    snprintf(file, sizeof file, "itemname_%s.nat", CODE[language]);
    uint32_t len;
    uint8_t* d = gpc_file(g, glen, file, &len);
    if (!d && language != 1) { language = 1; d = gpc_file(g, glen, "itemname_en.nat", &len); }
    if (!d) return -1;
    uint32_t count = len >= 4 ? rd32(d) & 0xFFF : 0, pool = 4 + 16 * count;
    Item* items = pool <= len ? malloc(sizeof(Item) * (count ? count : 1)) : NULL;
    FILE* f = items ? fopen(path, "w") : NULL;
    if (!f) { free(items); free(d); return -1; }
    fprintf(f, "# DQIX item numbers for transmog.ini (Mods > Transmog), from your game, in %s.\n"
               "# In transmog.ini, under a character's name: slot = number, e.g. head = 12170,\n"
               "# then Mods > Reload transmog.ini. Each slot takes only the numbers listed under it.\n"
               "# A weapon look is used only on a weapon of the same kind (the same hundreds, e.g. 203xx whips).\n"
               "# \"empty\" (in transmog.ini) shows a slot as if nothing were worn there.\n", NAME[language]);
    static const int ORDER[TM_SLOTS] = { 4, 0, 2, 1, 3, 5, 6, 7 };   /* head, torso, arms, legs, feet, weapon, shield, accessory */
    int total = 0;
    for (int oi = 0; oi < TM_SLOTS; oi++) {
        int s = ORDER[oi];
        fprintf(f, "\n[%s]\n", g_tm_slot_names[s]);
        if (s == 2) fprintf(f, "# 14xxx (not listed): the sleeves of torso 13xxx, as worn without gloves (14621: 13621's)\n");
        int n = 0;
        for (uint32_t k = 0; k < count; k++) {
            const uint8_t* r = d + 4 + 16 * k;
            int id = (int)rd32(r + 12);
            uint32_t a = rd32(r);
            if (id < LIST_LO[s] || id > LIST_HI[s] || pool + a >= len || !memchr(d + pool + a, 0, len - pool - a)) continue;
            items[n].id = id; items[n].text = (const char*)d + pool + a; n++;
        }
        qsort(items, (size_t)n, sizeof items[0], by_id);
        for (int k = 0; k < n; k++) {
            char name[160];
            decode_name(items[k].text, name, sizeof name);
            if (!name[0]) continue;
            fprintf(f, "%5d  %s\n", items[k].id, name);
            total++;
        }
    }
    fclose(f);
    free(items);
    free(d);
    return total;
}
