#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MS __attribute__((ms_abi))
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
u8 g_main_ram[4 << 20];
u32 g_prof_addr[65536], g_prof_cnt[65536]; int g_prof_state = -1; u64 g_prof_from, g_prof_to;
void* g_win = (void*)0x1234; void* g_bar = (void*)0x99;
char g_notice[160]; u64 g_notice_until, g_perf_freq = 1000;
static int nmenu, nitems, nlearn, verbose;
static MS void* s_popup(void) { return (void*)(long)(0x1000 + nmenu++); }
static MS int s_append(void* m, u32 f, u64 id, const u16* t) {
    char b[200]; int i = 0; if (t) for (; t[i] && i < 199; i++) b[i] = (char)t[i]; b[i] = 0;
    nitems++; if (f == 0 && id >= 2000 && id < 2400) nlearn++;
    if (verbose || (id >= 2000 && id < 2400 && f == 0 && (nlearn <= 3 || (id >= 2288 && id <= 2290))) || (f == 0x10 && strstr(b, "bilit")))
        printf("  menu %lx %s id=%llu '%s'\n", (long)m, f == 0x10 ? "SUB" : f == 0x800 ? "SEP" : "ITEM", id, b);
    return 1;
}
static MS void* s_fopen(const char* n, const char* m) { return fopen(n, m); }
static MS int s_fclose(void* f) { return fclose(f); }
static MS char* s_fgets(char* b, int n, void* f) { return fgets(b, n, f); }
static MS u64 s_fwrite(const void* p, u64 a, u64 b, void* f) { return fwrite(p, a, b, f); }
static MS int s_free(void* p) { free(p); return 0; }
static MS u64 s_perf(void) { return 5; }
static MS int s_msg(void* h, const u16* a, const u16* b, u32 c) { return 0; }
void *IMP_CreatePopupMenu = s_popup, *IMP_AppendMenuW = s_append, *IMP_DrawMenuBar = 0, *IMP_fopen = s_fopen, *IMP_fclose = s_fclose,
     *IMP_fgets = s_fgets, *IMP_fwrite = s_fwrite, *IMP_free = s_free, *IMP_SDL_GetPerformanceCounter = s_perf, *IMP_MessageBoxW = s_msg;
static int checks[4000];
MS void menu_check(int id, int on) { checks[id] = on; }
static const char* answer = "500";
MS char* ask_text(void* w, const char* t, const char* p) { printf("  ask '%s': %s\n", t, p); return strdup(answer); }
static int orig_called; static int in_btl;
MS u64 overlays_loaded_mask(void) { return (u64)in_btl; }
MS void orig_first_strike(u32* cpu) { orig_called++; cpu[0] = 0; }
u32 g_arm9[41], g_cyc; extern int cheat_in_call; u8 g_dtcm[0x4000]; u32 g_dtcm_base = 0x027E0000;
static int recalcs, recalc_bad, buff_calls[8], buff_bad;
static void buff_stub(u32* c, int i) { buff_calls[i]++; if (c != g_arm9 || c[1] != 4 || c[0] != 0x020F38E0 + 0 * 0x964 && c[0] != 0x020F38E0 + 2 * 0x964 && c[0] != 0x020F38E0 + 3 * 0x964) buff_bad++; }
MS void fn_main_020877c0(u32* c) { buff_stub(c, 0); } MS void fn_main_020878b4(u32* c) { buff_stub(c, 1); } MS void fn_main_020879a8(u32* c) { buff_stub(c, 2); }
MS void fn_main_02087a9c(u32* c) { buff_stub(c, 3); } MS void fn_main_02087b90(u32* c) { buff_stub(c, 4); } MS void fn_main_02087c84(u32* c) { buff_stub(c, 5); }
MS void fn_main_02087d78(u32* c) { buff_stub(c, 6); } MS void fn_main_02087e6c(u32* c) { buff_stub(c, 7); }
static int recalcs_dummy;
MS void fn_main_02083e28(u32* c) {      /* RecalcStats: check how it's called, and push max HP down to test the clamp */
    recalcs++;
    if (c != g_arm9 || !cheat_in_call || c[1] != 0 || c[13] >= 0x027E3F00 || (c[13] & 7)) recalc_bad++;
    if (c[13] >= 0x027E0000) memset(g_dtcm + (c[13] - 0x027E0000) - 0x300, 0xEE, 0x300);   /* its stack frames */
    for (int k = 0; k < 4; k++) if (c[0] == 0x020F384C + k * 0x964) { u32 cur = 0x020F38E0 + k * 0x964; memcpy(g_main_ram + cur + 4 - 0x02000000, &(u16){ 60 }, 2); return; }
    recalc_bad++;
}

MS int voc_spell_ok(u32*); MS int ability_any(u32*); MS int keep_ability(u32*); MS int spell_list_all(u32*, int); MS void build_cheats_menu(void*); MS void cheat_action(int); MS void cheat_frame(void); MS void first_strike(u32*);
static u8* R(u32 a) { return g_main_ram + (a - 0x02000000); }
static void W16(u32 a, u16 v) { memcpy(R(a), &v, 2); } static void W32(u32 a, u32 v) { memcpy(R(a), &v, 4); }
static u16 G16(u32 a) { u16 v; memcpy(&v, R(a), 2); return v; } static u32 G32(u32 a) { u32 v; memcpy(&v, R(a), 4); return v; }
static void prim(u32 p, int hp, int mp, int mhp, int mmp) { W16(p, hp); W16(p+2, mp); W16(p+4, mhp); W16(p+6, mmp); W16(p+8, 50); W16(p+10, 60); W16(p+12, 70); W16(p+14, 80); W32(p+16, 10 | 20 << 10 | 30 << 20 | 3u << 30); }
static void show(const char* t, u32 p) { u32 b = G32(p+16); printf("%s HP %d/%d MP %d/%d atk %d def %d agi %d dex %d charm %d mm %d mend %d top %d\n", t, G16(p), G16(p+4), G16(p+2), G16(p+6), G16(p+8), G16(p+10), G16(p+12), G16(p+14), b & 1023, b >> 10 & 1023, b >> 20 & 1023, b >> 30); }
#define LB(k) R(0x020F3888 + (k) * 0x964 + 0x8B0)
static int bit(int k, int b) { return LB(k)[b >> 3] >> (b & 7) & 1; }
static void dump(const char* t, int k) { printf("%s:", t); for (int i = 0; i < 45; i++) printf("%02x", LB(k)[i]); printf("\n"); }
static void setup(void) {
    for (int i = 0; i < 6; i++) {
        u32 o = 0x02200000 + i * 0x200, base = i < 4 ? 0x020F3888 + i * 0x964 : 0x02210000 + i * 0x100, cur = 0x020F38E0 + i * 0x964;
        W32(0x020F33D8 + 8 + 4 * i, o); W16(o, i < 4 ? 0x980 : 0x080); W32(o + 0x150, i < 4 ? 0x020F384C + i * 0x964 : 0); W32(o + 0x134, base); W32(o + 0x138, cur);
    }
}
static int second(void) {      /* a new run: the edits come back from cheats.ini */
    setup();
    LB(1)[36] = 0x01; LB(0)[0] = 0;    /* the game says member 2 knows Frizz and member 1 knows nothing */
    build_cheats_menu(g_bar); cheat_frame();
    printf("reload: m2 frizz=%d (want 0)  m1 dragon slash=%d (want 1)  m1 void bit 0x23:80=%d (want 0)\n", bit(1, 288), bit(0, 0), bit(0, 0x23 * 8 + 7));
    return 0;
}
int main(int argc, char** argv) {
    if (argc > 1) return second();
    remove("cheats.ini");
    /* 4 party members at objects 0..3, 2 monsters at 4, 5 */
    for (int i = 0; i < 6; i++) {
        u32 o = 0x02200000 + i * 0x200, base = i < 4 ? 0x020F3888 + i * 0x964 : 0x02210000 + i * 0x100, cur = 0x020F38E0 + i * 0x964;
        W32(0x020F33D8 + 8 + 4 * i, o);
        W16(o, i < 4 ? 0x980 : 0x080); W32(o + 0x150, i < 4 ? 0x020F384C + i * 0x964 : 0);
        W32(o + 0x134, base); W32(o + 0x138, cur);
        prim(base + 0x2C, 40 + i, 10, 100 + i, 30); prim(cur, 40 + i, 10, 100 + i, 30);
    }
    W16(0x020F38E0 + 1 * 0x964, 0);  /* member 2 fallen */
    LB(1)[36] = 0x05;                       /* member 2 knows Frizz and Kafrizz */
    LB(0)[3] = 0x10; LB(0)[0x23] = 0x80;    /* member 1: Knife 58 and an unnamed bit */
    build_cheats_menu(g_bar);
    printf("menu: %d entries, %d spell/ability items for member 1\n", nitems, nlearn);
    printf("ticks FS0=%d INFO=%d\n", checks[410], checks[413]);
    cheat_frame(); for (int k = 0; k < 15; k++) cheat_frame();
    printf("ticks m2 frizz=%d frizzle=%d kafrizz=%d | m1 assassin's stab=%d\n", checks[2400 + 288], checks[2400 + 289], checks[2400 + 290], checks[2000 + 28]);
    cheat_action(2400 + 288); printf("toggle: %s | m2 frizz=%d tick=%d\n", g_notice, bit(1, 288), checks[2688]);
    LB(1)[36] |= 1; cheat_frame(); printf("game sets it again -> after a frame frizz=%d\n", bit(1, 288));
    cheat_action(2400 + 289); printf("toggle: %s | frizzle=%d\n", g_notice, bit(1, 289));
    cheat_action(2400 + 349); printf("unnamed bit 0x07:20 ignored: bit=%d\n", bit(1, 349));
    cheat_action(3600 + 0 * 10 + 2); dump("m1 all abilities", 0); printf("  %s\n", g_notice);
    cheat_action(3600 + 0 * 10 + 4); dump("m1 after undo  ", 0); printf("  %s\n", g_notice);
    cheat_action(3600 + 2 * 10 + 0); dump("m3 all spells   ", 2);
    cheat_action(3600 + 0 * 10 + 2);   /* member 1 learns every ability again, kept for the reload test */
    W16(0x02200000 + 3 * 0x200, 0); cheat_action(2000 + 3 * 400 + 0); printf("absent member: %s\n", g_notice); W16(0x02200000 + 3 * 0x200, 0x980);
    cheat_action(404); printf("heal: %s\n", g_notice);
    show("m1 cur", 0x020F38E0); show("m2 cur", 0x020F38E0 + 0x964);
    W16(0x020F38E0, 5); cheat_action(401); cheat_frame(); show("infHP m1", 0x020F38E0);
    cheat_action(403); cheat_frame(); show("max m1 cur", 0x020F38E0); show("max m1 base", 0x020F3888 + 0x2C); show("monster", 0x020F38E0 + 4 * 0x964);
    cheat_action(403);  /* off */
    answer = "321"; cheat_action(420 + 2 * 10 + 4); cheat_frame(); show("edit m3 agi", 0x020F38E0 + 2 * 0x964); printf("tick %d\n", checks[444]);
    answer = "abc"; cheat_action(420 + 2 * 10 + 4); cheat_frame(); show("bad input keeps", 0x020F38E0 + 2 * 0x964);
    answer = "1200"; cheat_action(420 + 0 * 10 + 0); cheat_frame(); show("m1 maxhp 1200->999", 0x020F38E0);
    cheat_action(405); cheat_action(406); cheat_action(407); printf("gold %u bank %u medals %u\n", G32(0x020F6D48), G32(0x020F6D44), G32(0x020F6D50));
    W16(0x020F5DE8, 0x55F0); R(0x020F5F18)[0] = 3; W16(0x020F5DEA, 0x55F1); R(0x020F5F19)[0] = 0; W16(0x020F6AC0, 0x4650); R(0x020F6B40)[0] = 1;
    cheat_action(408); printf("items: %d %d %d | %s\n", R(0x020F5F18)[0], R(0x020F5F19)[0], R(0x020F6B40)[0], g_notice);
    u32 cpu[16] = {7}; first_strike(cpu); printf("fs normal: r0=%u orig=%d\n", cpu[0], orig_called);
    cheat_action(411); first_strike(cpu); printf("fs party: r0=%u orig=%d tick=%d\n", cpu[0], orig_called, checks[411]);
    cheat_action(412); first_strike(cpu); printf("fs enemy: r0=%u\n", cpu[0]);
    cheat_action(413); for (int k = 0; k < 40; k++) cheat_frame(); printf("info: %s\n", g_notice);
    { u32 cpu[16] = {0x02000000, 0x02200000 + 1 * 0x200, 1};     /* member 2, Frizzle: turned on in the editor */
      u32 c2[16] = {0x02000000, 0x02200000 + 1 * 0x200, 0};     /* member 2, Frizz: forgotten */
      u32 c3[16] = {0x02000000, 0x02200000 + 4 * 0x200, 1};     /* a monster */
      printf("vocation check: m2 frizzle=%d frizz=%d monster=%d (want 1 0 0)\n", voc_spell_ok(cpu), voc_spell_ok(c2), voc_spell_ok(c3)); }
    { u32 a[16] = {0}, b[16] = {0}; a[5] = 0x02200000 + 1 * 0x200; b[5] = 0x02200000 + 0 * 0x200;
      printf("spell list: m2 (editor turned a spell on)=%d m1 (none)=%d (want 1 0)\n", spell_list_all(a, 5), spell_list_all(b, 5)); }
    /* everyone knows everything */
    memset(LB(3), 0, 45); LB(3)[36] = 0x04;              /* member 4: only Heal... well, bit 290 (Kafrizz) */
    cheat_action(419); printf("unnamed: %s\n", g_notice);
    cheat_action(416); { u32 c3[16] = {0, 0x02200000 + 4 * 0x200, 5}; printf("vocation check with use-all on: %d, spell list: %d, ability: %d\n", voc_spell_ok(c3), spell_list_all(c3, 4), ability_any(c3));
      u32 f1[16] = {0x020F384C + 3 * 0x964, 10}, f2[16] = {0x020F384C + 3 * 0x964, 76}, f3[16] = {0x020F384C + 3 * 0x964, 287};
      printf("keep item ability with use-all: gigagash=%d staff-0pt(unnamed)=%d id287=%d (want 1 %d 0)\n", keep_ability(f1), keep_ability(f2), keep_ability(f3), 1); }
    printf("use all: %s tick=%d | m4 frizz=%d kafrizz=%d gigaslash=%d void 0x07:20=%d\n", g_notice, checks[416], bit(3, 288), bit(3, 290), bit(3, 8), bit(3, 349));
    printf("  m2 frizz=%d (kept forgotten by the editor)\n", bit(1, 288));
    memset(LB(3), 0, 45); cheat_frame(); printf("  game rebuild clears member 4 -> next frame dragon slash=%d evac=%d\n", bit(3, 0), bit(3, 288 + 63));
    dump("m4 with unnamed", 3);
    cheat_action(419); dump("m4 unnamed off ", 3); printf("  %s\n", g_notice);
    R(0x020F3888 + 3 * 0x964 + 0x428 + 1)[0] = 13;     /* member 4 puts 13 points into Sword while it's on */
    cheat_action(416); printf("off: %s | m4 kafrizz=%d frizz=%d sword3=%d | m1 dragon slash=%d (editor kept)\n", g_notice, bit(3, 290), bit(3, 288), bit(3, 0), bit(0, 0));
    printf("  earned with 13 sword points: dragon slash(3)=%d attack+10(7)=%d metal slash(13)=%d crit(22)=%d (want 1 1 1 0)\n", bit(3, 0), bit(3, 1), bit(3, 2), bit(3, 3));
    printf("  m4 kafrizz is 0 because the rebuild cleared it while the cheat was on (the game puts it back on its next rebuild)\n");

    { u32 z[16] = {0}; printf("ability off: %d\n", ability_any(z)); u32 f[16] = {0x020F384C + 3 * 0x964, 10}; printf("keep off: %d\n", keep_ability(f)); }
    { u32 b1 = 0x020F384C + 0 * 0x964 + 0x94, b2 = 0x020F384C + 1 * 0x964 + 0x94;
      R(b1 + 0x17)[0] = 0x40; R(b1 + 0x3B)[0] = 0x05; R(b1 + 0x24)[0] = 1;
      cheat_action(3640); cheat_action(3641); printf("battle toggles: coup=%d tension=%d\n", checks[3640], checks[3641]);
      in_btl = 0; cheat_frame(); printf("field: tension %d status %02x coup %02x (want 1 40 05)\n", R(b1 + 0x24)[0], R(b1 + 0x17)[0], R(b1 + 0x3B)[0]);
      in_btl = 1; cheat_frame(); printf("battle: tension %d status %02x coup %02x (want 4 41 9d) | fallen m2: tension %d coup %02x (want 0 00)\n",
          R(b1 + 0x24)[0], R(b1 + 0x17)[0], R(b1 + 0x3B)[0], R(b2 + 0x24)[0], R(b2 + 0x3B)[0]);
      R(b1 + 0x24)[0] = 0; R(b1 + 0x3B)[0] &= 0x7F; cheat_frame(); printf("after using them: tension %d coup %02x (want 4 9d)\n", R(b1 + 0x24)[0], R(b1 + 0x3B)[0]); }
    /* vocations and gear: member 1 is a warrior with a copper sword; in the bag a soldier's sword, an oak staff
     * (a wand), a pot lid and a leather shield; a bandana on, a hairband and an iron helmet in the bag */
    { in_btl = 0;
      u32 c = 0x020F384C, cur = 0x020F38E0;
      g_arm9[13] = 0x027E3F00; g_arm9[21] = 0x1F; g_arm9[0] = 0xAAAA; g_cyc = 77;
      for (int i = 0x2F00; i < 0x3F00; i++) g_dtcm[i] = (u8)(i * 7);
      W32(c + 0x950, 1); W16(c + 0x954, 2); W16(c + 0x16C + 2, 15); W16(c + 0x16C + 6, 0);
      for (int sl = 0; sl < 10; sl++) W16(c + 0x488 + 2 * sl, 0xFFFF);
      W16(c + 0x488 + 14, 0x4E24); W16(c + 0x488 + 12, 0x307A);
      memset(R(0x020F5FB0), 0xFF, 272 * 2); memset(R(0x020F61D0), 0, 272); memset(R(0x020F62E0), 0xFF, 96); memset(R(0x020F6340), 0, 48);
      memset(R(0x020F66D0), 0xFF, 288); memset(R(0x020F67F0), 0, 144);
      W16(0x020F5FB0, 0x4E25); R(0x020F61D0)[0] = 1; W16(0x020F5FB2, 0x4FB3); R(0x020F61D1)[0] = 2;
      W16(0x020F62E0, 0x5330); R(0x020F6340)[0] = 1; W16(0x020F62E2, 0x532B); R(0x020F6341)[0] = 1;
      W16(0x020F66D0, 0x30D2); R(0x020F67F0)[0] = 1; W16(0x020F66D2, 0x2F9F); R(0x020F67F1)[0] = 1;
      W16(cur, 90); W16(cur + 4, 100);
      cheat_action(3650 + 13); printf("best (warrior): ticked warrior=%d | ", checks[3651]); cheat_frame();
      printf("%s\n  weapon %04x shield %04x head %04x | bag w %04x:%d %04x:%d s %04x:%d %04x:%d h %04x:%d %04x:%d | HP %d/%d recalcs %d bad %d r0 %x cyc %u\n", g_notice,
          G16(c + 0x488 + 14), G16(c + 0x488 + 16), G16(c + 0x488 + 12), G16(0x020F5FB0), R(0x020F61D0)[0], G16(0x020F5FB2), R(0x020F61D1)[0],
          G16(0x020F62E0), R(0x020F6340)[0], G16(0x020F62E2), R(0x020F6341)[0], G16(0x020F66D0), R(0x020F67F0)[0], G16(0x020F66D2), R(0x020F67F1)[0],
          G16(cur), G16(cur + 4), recalcs, recalc_bad, g_arm9[0], g_cyc);
      cheat_action(3650 + 3); cheat_frame();
      printf("to mage: %s | voc %u used %x lvl %d | weapon %04x shield %04x head %04x | bag w %04x:%d %04x:%d\n", g_notice, G32(c + 0x950), G16(c + 0x954), G16(c + 0x16C + 6),
          G16(c + 0x488 + 14), G16(c + 0x488 + 16), G16(c + 0x488 + 12), G16(0x020F5FB0), R(0x020F61D0)[0], G16(0x020F5FB2), R(0x020F61D1)[0]);
      cheat_frame(); cheat_frame(); for (int k = 0; k < 15; k++) cheat_frame();
      printf("  ticks: warrior %d mage %d auto %d\n", checks[3651], checks[3653], checks[3711]);
      cheat_action(3711); cheat_action(3650 + 4); cheat_frame();
      printf("auto + martial artist: %s | weapon %04x shield %04x head %04x | bag w %04x:%d %04x:%d s %04x:%d %04x:%d\n", g_notice,
          G16(c + 0x488 + 14), G16(c + 0x488 + 16), G16(c + 0x488 + 12), G16(0x020F5FB0), R(0x020F61D0)[0], G16(0x020F5FB2), R(0x020F61D1)[0],
          G16(0x020F62E0), R(0x020F6340)[0], G16(0x020F62E2), R(0x020F6341)[0]);
      cheat_action(3650 + 1); cheat_frame();
      printf("back to warrior: %s | weapon %04x shield %04x head %04x\n", g_notice, G16(c + 0x488 + 14), G16(c + 0x488 + 16), G16(c + 0x488 + 12));
      cheat_action(3650 + 0); cheat_frame();
      printf("to Guardian: %s | voc %u used %x lvl %d tick %d | weapon %04x shield %04x head %04x\n", g_notice, G32(c + 0x950), G16(c + 0x954), G16(c + 0x16C),
          checks[3650], G16(c + 0x488 + 14), G16(c + 0x488 + 16), G16(c + 0x488 + 12));
      cheat_action(3650 + 1); cheat_frame(); printf("and back: %s\n", g_notice);
      in_btl = 1; cheat_action(3650 + 2); printf("in battle: %s | voc %u\n", g_notice, G32(c + 0x950)); in_btl = 0;
      g_arm9[21] = 0x12; cheat_action(3710); cheat_frame(); printf("IRQ mode: recalcs %d (waits) | ", recalcs); g_arm9[21] = 0x1F; cheat_frame(); printf("then %d: %s\n", recalcs, g_notice);
      { int dirty = 0; for (int i = 0x2F00; i < 0x3F00; i++) dirty += g_dtcm[i] != (u8)(i * 7); printf("stack below sp restored: %s\n", dirty ? "NO" : "yes"); }
      printf("game_call left the Cpu alone: r0 %x sp %x cyc %u in_call %d bad %d\n", g_arm9[0], g_arm9[13], g_cyc, cheat_in_call, recalc_bad);
    }
    { u32 m1 = 0x020F38E0, e1 = 0x020F38E0 + 4 * 0x964;     /* member 1 and monster 1's current stats */
      W32(m1 + 0x14, 0x400 | 0x10000); R(m1 + 0x91)[0] = 3; R(m1 + 0x97)[0] = 1; R(m1 + 0x92)[0] = 2;   /* oomph, magic barrier on; defence count but no flag */
      W32(e1 + 0x14, 0x800 | 0x10); R(e1 + 0x92)[0] = 5; R(e1 + 0x93)[0] = 4;                             /* sapped; asleep (not a buff) */
      in_btl = 1; cheat_action(3642); cheat_frame();
      printf("permanent buffs (tick %d): m1 oomph %d barrier %d no-flag %d | enemy sap %d sleep-slot %d (want 6 6 2 6 4)\n", checks[3642],
          R(m1 + 0x91)[0], R(m1 + 0x97)[0], R(m1 + 0x92)[0], R(e1 + 0x92)[0], R(e1 + 0x93)[0]);
      R(m1 + 0x91)[0] = 0; cheat_frame(); printf("  countdown 0 left alone: %d\n", R(m1 + 0x91)[0]);
      in_btl = 0; R(m1 + 0x91)[0] = 2; cheat_frame(); printf("  out of battle untouched: %d\n", R(m1 + 0x91)[0]); }
    { MS int crit_force(u32*); u32 a[16] = {0, 2, 0x02300000}, e[16] = {0, 5}; W32(0x02300000 + 0x18, 2 << 12);
      printf("crit off: party %d monster %d | ", crit_force(a), crit_force(e));
      cheat_action(3643); printf("on (tick %d): party %d monster %d (want 1 0) | %s\n", checks[3643], crit_force(a), crit_force(e), g_notice); }
    { MS int combo_fix(u32*, int); u32 a[16] = {0}, e[16] = {0}; a[9] = 1; e[9] = 6;
      printf("combo off: party %d monster %d | ", combo_fix(a, 1), combo_fix(e, 1));
      cheat_action(3644); printf("on (tick %d): party %d monster %d (want 3 1) | %s\n", checks[3644], combo_fix(a, 1), combo_fix(e, 1), g_notice); }
    { in_btl = 0; cheat_action(3645); printf("buffs out of battle: %s | ", g_notice);
      in_btl = 1; cheat_action(3645); cheat_frame();
      printf("in battle: %s | calls %d %d %d %d %d %d %d %d bad %d (m2 is fallen)\n", g_notice, buff_calls[0], buff_calls[1], buff_calls[2], buff_calls[3],
          buff_calls[4], buff_calls[5], buff_calls[6], buff_calls[7], buff_bad); in_btl = 0; }
    { MS void pkj_battle(u32*); u32 ctx = 0x02320000, b = 0x02330000, cpu[16] = { ctx };
      W32(ctx + 0x29C, b); R(b + 0x81B1)[0] = 2 << 4; W16(b + 0x81B4, 1); W16(b + 0x81B4 + 24, 3); W16(b + 0x81B4 + 48, 7);
      pkj_battle(cpu); printf("pkj off: %d %d | ", G16(b + 0x81B4), G16(b + 0x81B4 + 24));
      cheat_action(3646); pkj_battle(cpu); printf("on (tick %d): %d %d third untouched %d | ", checks[3646], G16(b + 0x81B4), G16(b + 0x81B4 + 24), G16(b + 0x81B4 + 48));
      W16(b + 0x81B4, 1); W16(b + 0x81B4 + 24, 302); pkj_battle(cpu); printf("with Morag: %d %d (want 1 302) | %s\n", G16(b + 0x81B4), G16(b + 0x81B4 + 24), g_notice);
      W16(b + 0x81B4 + 24, 3); cheat_action(3648); pkj_battle(cpu); printf("  gem slimes (ticks pkj %d gem %d): %d %d | %s\n", checks[3646], checks[3648], G16(b + 0x81B4), G16(b + 0x81B4 + 24), g_notice);
      cheat_action(3648); W16(b + 0x81B4, 1); pkj_battle(cpu); printf("  gem off again: %d | %s\n", G16(b + 0x81B4), g_notice); cheat_action(3646); }
    { MS int flee_block(u32*); u32 act = 0x02331000, m[16] = {0, 5, 5, act}, pa[16] = {0, 1, 1, act};
      W32(act + 4, 917); printf("flee off: monster %d | ", flee_block(m));
      cheat_action(3647); printf("on (tick %d): monster %d party %d | ", checks[3647], flee_block(m), flee_block(pa));
      W32(act + 4, 0x2000 | 225); printf("225 %d | ", flee_block(m)); W32(act + 4, 336); printf("other action %d (want 1 0 1 0) | %s\n", flee_block(m), g_notice); }
    cheat_action(417);
    cheat_action(418); printf("rec: %s state=%d\n", g_notice, g_prof_state); g_prof_addr[5] = 0x020dd3cc; g_prof_cnt[5] = 3; g_prof_addr[9] = 0x80001234; g_prof_cnt[9] = 7;
    cheat_action(418); printf("rec: %s state=%d\n", g_notice, g_prof_state);
    FILE* f = fopen("cheats.ini", "r"); char l[200]; while (fgets(l, sizeof l, f)) printf("ini: %s", l); fclose(f);
    return 0;
}
