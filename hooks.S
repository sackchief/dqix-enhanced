.intel_syntax noprefix
.text
.globl hook_do_action, hook_frame, hook_help, hook_voc_spell, hook_spell_list, hook_spell_list26, hook_ability_weapon, hook_forget_ability, hook_sync, hook_crit, hook_combo, hook_battle_setup, hook_flee
/* ov002_02157174 picks where a member's spell list comes from: vocation 0 (the debug class) lists every
 * learned spell (func_020839dc), any other vocation only its own spells (func_0209ab7c). This replaces
 * its je (6 bytes, ZF from "test eax,eax" on the vocation): spell_list_all can send other vocations to
 * the every-learned-spell side too. rbx = Cpu*; every path after reloads from it, so only the flags
 * and callee-saved registers are live; the volatile ones are kept anyway. */
hook_spell_list:                    /* ov002_02157174: the object is r5 */
    je 2f
    push rdx
    mov edx, 5
    call spell_list_ask
    pop rdx
    jnz 2f
    jmp SPELLLIST_VOCATION
2:  jmp SPELLLIST_LEARNED

/* ov026_021dc8fc has the same choice (vocation 0: func_020839dc, otherwise func_0209ab7c); r4 = object */
hook_spell_list26:
    je 2f
    push rdx
    mov edx, 4
    call spell_list_ask
    pop rdx
    jnz 2f
    jmp SPELLLIST26_VOCATION
2:  jmp SPELLLIST26_LEARNED

/* ZF=0 out if spell_list_all(rbx, edx) says use every learned spell. Keeps every volatile register
 * but rdx (the callers keep that one). Entered with rsp = 0 mod 16 (the body is aligned; push + call), so
 * six pushes and 32 keep it aligned for the call. pop, lea and ret leave the flags alone. */
spell_list_ask:
    push rax
    push rcx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 32
    mov rcx, rbx
    call spell_list_all
    test eax, eax
    lea rsp, [rsp + 32]             /* lea and pop leave the flags alone */
    pop r11
    pop r10
    pop r9
    pop r8
    pop rcx
    pop rax
    ret
/* func_0209ac28 (is the spell on the vocation's list?): voc_spell_ok says yes first, or the game asks.
 * Replaces push r13 / push r12 / push rbp (5 bytes) at the entry. rcx = Cpu*, r0 at [rcx]. */
hook_voc_spell:
    push rcx
    sub rsp, 32
    call voc_spell_ok
    add rsp, 32
    pop rcx
    test eax, eax
    jz 1f
    mov dword ptr [rcx], 1
    ret
1:  push r13
    push r12
    push rbp
    jmp VOCSPELL_RESUME
/* do_action(int id): our ids go to cheat_action, the rest carry on
 * (the exe's own ids stop at 0x162 and at 0x8a + the number of skins) */
hook_do_action:
    cmp ecx, 401
    jb 1f
    cmp ecx, 459
    jbe 2f
    cmp ecx, 2000                   /* the spell and ability editor: 2000..3639 */
    jb 1f
    cmp ecx, 3719                   /* ...3640.. the battle toggles, 3650.. vocations and gear */
    ja 1f
2:  jmp cheat_action
1:  push r15
    push r14
    push r13
    jmp DO_ACTION_RESUME
/* frontend_frame(): the per-frame cheats first */
hook_frame:
    sub rsp, 40
    call cheat_frame
    add rsp, 40
    push r14
    push r12
    push rbp
    push rdi
    jmp FRAME_RESUME
/* menu_init, just before the Help menu: the Cheats menu (replaces mov rdi,[g_bar]) */
hook_help:
    sub rsp, 40
    mov rcx, [rip + g_bar]
    call build_cheats_menu
    add rsp, 40
    mov rdi, [rip + g_bar]
    ret

/* ov000_0217c32c builds the battle Abilities list. For each ability it reads the member's vocation word:
 * vocation 0 (the debug class) makes it usable outright; any other vocation compares the ability's weapon
 * type (low 5 bits of entry+4, sword 1 .. fisticuffs 14) with the equipped weapon. This replaces
 * "lea ecx,[rax+0x950]" (6 bytes) at that read: if ability_any says so, go to the vocation-0 result
 * (r1 = 1 at ABILITY_USABLE) with the ARM flags "cmp r1,#0" leaves for 0; else read the vocation as usual.
 * rbx = Cpu*, rsi = the character (kept: callee-saved); the body is 16-aligned. */
hook_ability_weapon:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 40
    mov rcx, rbx
    call ability_any
    add rsp, 40
    test eax, eax
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    jz 1f
    mov dword ptr [rbx + 0x40], 0        /* N */
    mov dword ptr [rbx + 0x44], 1        /* Z */
    mov dword ptr [rbx + 0x48], 1        /* C */
    mov dword ptr [rbx + 0x4c], 0        /* V */
    jmp ABILITY_USABLE
1:  lea ecx, [rax + 0x950]
    jmp ABILITY_VOCATION

/* func_02083b28 ForgetAbility(r0 char, r1 id). Its only callers (ov026_021dc8fc, building the battle lists,
 * and ov004_0215eb40) forget every item-given ability whose item isn't equipped, just before the
 * lists are read. keep_ability non-zero: leave the bit alone. Replaces push rdi/rsi/rbx + sub rsp,0x20
 * (7 bytes) at the entry; entered with rsp = 8 mod 16. */
hook_forget_ability:
    push rcx
    sub rsp, 32
    call keep_ability
    add rsp, 32
    pop rcx
    test eax, eax
    jz 1f
    ret
1:  push rdi
    push rsi
    push rbx
    sub rsp, 0x20
    jmp FORGET_RESUME

/* arm9_sync (the ARM9's cycles become emulated time, then interrupts): while the cheats are inside a call
 * to the game's own code (cheat_in_call, cheats.c game_call), drop the cycles and carry on, so no time
 * passes and nothing else runs in the middle. Replaces push rbx / sub rsp,0x20 (5 bytes) at the entry. */
hook_sync:
    cmp dword ptr [rip + cheat_in_call], 0
    jne 1f
    push rbx
    sub rsp, 0x20
    jmp ARM9SYNC_RESUME
1:  mov dword ptr [rip + g_cyc], 0
    ret

/* ov000_02156cc4 (is this hit critical?): crit_force says yes first (r0 = 1), or the game rolls.
 * Replaces push rsi / push rbx / sub rsp,0x28 (6 bytes) at the entry; rcx = Cpu*, entered with rsp = 8 mod 16. */
hook_crit:
    push rcx
    sub rsp, 32
    call crit_force
    add rsp, 32
    pop rcx
    test eax, eax
    jz 1f
    mov dword ptr [rcx], 1
    ret
1:  push rsi
    push rbx
    sub rsp, 0x28
    jmp CRIT_RESUME

/* ov024_021e6a90 (damage): replaces its call to mem_read8 for the combo byte (ecx = its address). rbx = the
 * Cpu* there (callee-saved); entered with rsp = 8 mod 16. The result goes back in eax like mem_read8's. */
hook_combo:
    push rcx
    sub rsp, 32
    call mem_read8
    movzx edx, al
    movsx edx, dl
    mov rcx, rbx
    call combo_fix
    add rsp, 32
    pop rcx
    ret

/* ov000_02165490 (battle setup, r0 = ctx): pkj_battle may change the enemy species first.
 * Replaces push r15 / push r14 / push r13 (6 bytes) at the entry; entered with rsp = 8 mod 16. */
hook_battle_setup:
    push rcx
    sub rsp, 32
    call pkj_battle
    add rsp, 32
    pop rcx
    push r15
    push r14
    push r13
    jmp SETUP_RESUME

/* ov024_021da670 (self-targeted actions, Flee among them): flee_block non-zero = r0 = 0 and return without
 * doing it. Replaces push rsi / push rbx / sub rsp,0x28 (6 bytes) at the entry; rsp = 8 mod 16 there. */
hook_flee:
    push rcx
    sub rsp, 32
    call flee_block
    add rsp, 32
    pop rcx
    test eax, eax
    jz 1f
    mov dword ptr [rcx], 0
    ret
1:  push rsi
    push rbx
    sub rsp, 0x28
    jmp FLEE_RESUME
