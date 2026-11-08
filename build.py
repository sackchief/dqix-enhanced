import subprocess, struct, hashlib, pickle, sys, os, re
HERE=os.path.dirname(os.path.abspath(__file__))
SRC=os.path.join(HERE,'..','orig.exe'); DST=sys.argv[1]
d=bytearray(open(SRC,'rb').read())
assert hashlib.sha256(d).hexdigest()=='e77dacc5d253d3c1a6c815a34e6d94580676b106e721286e03d7bc9fa832c52a'
S=pickle.load(open(os.path.join(HERE,'..','syms.pkl'),'rb'))
pe=struct.unpack('<I',d[0x3c:0x40])[0]; opt=pe+24
nsec=struct.unpack('<H',d[pe+6:pe+8])[0]; optsz=struct.unpack('<H',d[pe+20:pe+22])[0]
base=struct.unpack('<Q',d[opt+24:opt+32])[0]; assert base==0x140000000
soi=struct.unpack('<I',d[opt+56:opt+60])[0]; hdrs=struct.unpack('<I',d[opt+60:opt+64])[0]
sh=opt+optsz; newh=sh+nsec*40
assert newh+40<=hdrs and d[newh:newh+40]==b'\0'*40, 'no room for a section header'
SECVA=base+soi
SYM=dict(
  g_main_ram=S['g_main_ram'], g_win=S['g_win'], g_bar=0x1443f5998, g_notice=S['g_notice'],
  g_notice_until=S['g_notice_until'], g_perf_freq=S['g_perf_freq'],
  IMP_CreatePopupMenu=S['__imp_CreatePopupMenu'], IMP_AppendMenuW=S['__imp_AppendMenuW'], IMP_DrawMenuBar=S['__imp_DrawMenuBar'],
  IMP_fopen=S['__imp_fopen'], IMP_fclose=S['__imp_fclose'], IMP_fgets=S['__imp_fgets'], IMP_fwrite=S['__imp_fwrite'],
  IMP_free=S['__imp_free'], IMP_SDL_GetPerformanceCounter=S['__imp_SDL_GetPerformanceCounter'], IMP_MessageBoxW=S['__imp_MessageBoxW'],
  menu_check=S['menu_check'], ask_text=S['ask_text'], orig_first_strike=S['fn_ov017_021970a0'],
  DO_ACTION_RESUME=S['do_action']+6, FRAME_RESUME=S['frontend_frame']+6, VOCSPELL_RESUME=S['fn_main_0209ac28']+5)
assert S['fn_main_0209ac28']==0x1402d63f3
SYM.update(overlays_loaded_mask=S['overlays_loaded_mask'])
SYM.update(g_prof_addr=S['g_prof_addr'], g_prof_cnt=S['g_prof_cnt'], g_prof_state=S['g_prof_state'],
           g_prof_from=S['g_prof_from'], g_prof_to=S['g_prof_to'])
SYM.update(SPELLLIST_LEARNED=0x14056786b, SPELLLIST_VOCATION=0x14056776f,     # ov002_02157174's je, taken / not taken
           SPELLLIST26_LEARNED=0x140b3e0b6, SPELLLIST26_VOCATION=0x140b3df12,  # ov026_021dc8fc's
           ABILITY_USABLE=0x1404daf52, ABILITY_VOCATION=0x1404daeba,          # ov000_0217c32c's vocation read
           FORGET_RESUME=0x14026ae02)                                         # func_02083b28 + 7
SYM.update(g_dtcm=S['g_dtcm'], g_dtcm_base=S['g_dtcm_base'], CRIT_RESUME=S['fn_ov000_02156cc4']+6)
assert S['fn_ov000_02156cc4']==0x140447b0c
SYM.update(mem_read8=S['mem_read8'], SETUP_RESUME=S['fn_ov000_02165490']+6)
assert S['fn_ov000_02165490']==0x14048c6a5
SYM.update(FLEE_RESUME=S['fn_ov024_021da670']+6); assert S['fn_ov024_021da670']==0x140a3fc4d
for _a in ('020877c0','020878b4','020879a8','02087a9c','02087b90','02087c84','02087d78','02087e6c'): SYM['fn_main_'+_a]=S['fn_main_'+_a]
SYM.update(g_arm9=S['g_arm9'], g_cyc=S['g_cyc'], fn_main_02083e28=S['fn_main_02083e28'], ARM9SYNC_RESUME=S['arm9_sync']+5)
assert S['arm9_sync']==0x140d78160 and S['fn_main_02083e28']==0x14026bb7f and S['g_cyc']==0x1410d7fcc and S['g_arm9']==0x1410c7ee0
assert S['do_action']==0x140dff980 and S['frontend_frame']==0x140e08380 and S['menu_check']==0x140e165f0
CF=['clang','--target=x86_64-unknown-linux-gnu','-O2','-ffreestanding','-fno-builtin','-fpie','-fvisibility=hidden',
    '-mno-red-zone','-fno-stack-protector','-fno-asynchronous-unwind-tables','-fno-exceptions','-Wall','-Wno-misleading-indentation','-c']
subprocess.check_call([sys.executable,'-I',os.path.join(HERE,'gen_skills.py'),os.path.join(HERE,'skills.h')])
subprocess.check_call(CF+[os.path.join(HERE,'cheats.c'),'-o',os.path.join(HERE,'cheats.o')])
subprocess.check_call(['clang','--target=x86_64-unknown-linux-gnu','-c',os.path.join(HERE,'hooks.S'),'-o',os.path.join(HERE,'hooks.o')])
# no absolute relocations: the section must work wherever Windows loads the exe
for o in ('cheats.o','hooks.o'):
    r=subprocess.run(['readelf','-rW',os.path.join(HERE,o)],capture_output=True,text=True).stdout
    bad=[l for l in r.splitlines() if 'R_X86_64_64' in l or 'R_X86_64_32 ' in l or 'R_X86_64_32S' in l or 'GOT' in l]
    assert not bad, bad   # a GOT entry is an absolute address too, and nothing rebases it
open(os.path.join(HERE,'link.ld'),'w').write('''SECTIONS {
  . = 0x%x;
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .got : { *(.got*) }
  .data : { *(.data*) *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
''' % SECVA)
LD=['ld.lld','-T',os.path.join(HERE,'link.ld'),'--no-pie','-o',os.path.join(HERE,'cheats.elf'),os.path.join(HERE,'cheats.o'),os.path.join(HERE,'hooks.o')]
for k,v in SYM.items(): LD+=['--defsym','%s=0x%x'%(k,v)]
subprocess.check_call(LD)
secs=subprocess.run(['readelf','-SW',os.path.join(HERE,'cheats.elf')],capture_output=True,text=True).stdout
assert not re.search(r'\]\s+\.got', secs), 'linker made a GOT: absolute addresses would break under ASLR'
# the section must also cover .bss (objcopy leaves it out of the binary)
mem_end=0
for l in secs.splitlines():
    m=re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]{16})\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S+\s+(\S*A\S*)',l)
    if m and int(m.group(2),16): mem_end=max(mem_end,int(m.group(2),16)+int(m.group(3),16))
subprocess.check_call(['llvm-objcopy','-O','binary',os.path.join(HERE,'cheats.elf'),os.path.join(HERE,'cheats.bin')])
blob=open(os.path.join(HERE,'cheats.bin'),'rb').read()
syms={}
for l in subprocess.run(['nm',os.path.join(HERE,'cheats.elf')],capture_output=True,text=True).stdout.splitlines():
    p=l.split()
    if len(p)==3: syms[p[2]]=int(p[0],16)
first=min(v for k,v in syms.items() if k in ('hook_do_action','cheat_frame','build_cheats_menu'))
assert syms['hook_do_action']>=SECVA
# ---- section
FA=0x200
raw=bytearray(blob); raw+=b'\0'*((-len(raw))%FA)
memsz=max(len(blob),mem_end-SECVA)
vsize=(memsz+0xfff)&~0xfff
fo=(len(d)+FA-1)&~(FA-1)
d+=b'\0'*(fo-len(d)); d+=raw
d[newh:newh+40]=b'.cheats\0'+struct.pack('<IIIIIIHHI',memsz,SECVA-base,len(raw),fo,0,0,0,0,0xE0000060)
d[pe+6:pe+8]=struct.pack('<H',nsec+1)
d[opt+56:opt+60]=struct.pack('<I',soi+vsize)
# ---- hooks
def off(va):
    for i in range(nsec):
        h=sh+i*40; vs,vaa,rs,ro=struct.unpack('<4I',d[h+8:h+24])
        if vaa<=va-base<vaa+rs: return va-base-vaa+ro
    raise Exception(hex(va))
def rel(at,n,tgt,op,old):
    o=off(at); assert bytes(d[o:o+n])==bytes.fromhex(old),(hex(at),d[o:o+n].hex())
    d[o:o+n]=bytes([op])+struct.pack('<i',tgt-(at+5))+b'\x90'*(n-5)
rel(0x140dff980,6,syms['hook_do_action'],0xE9,'415741564155')          # do_action: push r15/r14/r13
rel(0x140e08380,6,syms['hook_frame'],0xE9,'415641545557')              # frontend_frame: push r14/r12/rbp/rdi
rel(0x140e15b4d,7,syms['hook_help'],0xE8,'488b3d44fe5d03')             # menu_init: mov rdi,[g_bar] before &Help
rel(0x1402d63f3,5,syms['hook_voc_spell'],0xE9,'4155415455')            # func_0209ac28: push r13/r12/rbp
rel(0x140567769,6,syms['hook_spell_list'],0xE9,'0f84fc000000')          # ov002_02157174: je (vocation 0 -> every learned spell)
rel(0x140b3df0c,6,syms['hook_spell_list26'],0xE9,'0f84a4010000')        # ov026_021dc8fc: the same
rel(0x1404daeb4,6,syms['hook_ability_weapon'],0xE9,'8d8850090000')      # ov000_0217c32c: lea ecx,[rax+0x950]
rel(0x14026adfb,7,syms['hook_forget_ability'],0xE9,'5756534883ec20')    # func_02083b28 ForgetAbility entry
rel(0x140d78160,5,syms['hook_sync'],0xE9,'534883ec20')                  # arm9_sync: push rbx / sub rsp,0x20
rel(0x140447b0c,6,syms['hook_crit'],0xE9,'56534883ec28')                # ov000_02156cc4 (critical?): push rsi/rbx, sub rsp,0x28
rel(0x14048c6a5,6,syms['hook_battle_setup'],0xE9,'415741564155')     # ov000_02165490 battle setup: push r15/r14/r13
rel(0x140a3fc4d,6,syms['hook_flee'],0xE9,'56534883ec28')                # ov024_021da670 (Flee and other self actions)
rel(0x140a560a8,5,syms['hook_combo'],0xE8,'e8'+struct.pack('<i',S['mem_read8']-(0x140a560a8+5)).hex())  # ov024_021e6a90: mem_read8(combo byte)
o=off(0x140870dc1); assert d[o]==0xE8 and struct.unpack('<i',d[o+1:o+5])[0]==S['fn_ov017_021970a0']-(0x140870dc1+5)
d[o+1:o+5]=struct.pack('<i',syms['first_strike']-(0x140870dc1+5))       # first strike decision
# relocations must not touch the patched bytes
rva=struct.unpack('<I',d[opt+112+5*8:opt+112+5*8+4])[0]; rsz=struct.unpack('<I',d[opt+112+5*8+4:opt+112+5*8+8])[0]
ro=off(base+rva); p=ro; R=[]
while p<ro+rsz:
    pg,bs=struct.unpack('<II',d[p:p+8])
    if bs==0: break
    for k in range((bs-8)//2):
        e=struct.unpack('<H',d[p+8+2*k:p+10+2*k])[0]
        if e>>12: R.append(base+pg+(e&0xfff))
    p+=bs
for at,n in ((0x140dff980,6),(0x140e08380,6),(0x140e15b4d,7),(0x140870dc1,5),(0x1402d63f3,5),(0x140567769,6),(0x140b3df0c,6),(0x1404daeb4,6),(0x14026adfb,7),(0x140d78160,5),(0x140447b0c,6),(0x140a560a8,5),(0x14048c6a5,6),(0x140a3fc4d,6)):
    assert not [r for r in R if at-7<r<at+n], hex(at)
open(DST,'wb').write(d)
print('section %x size %d, hooks: do_action %x frame %x help %x first %x' % (SECVA,len(blob),syms['hook_do_action'],syms['hook_frame'],syms['hook_help'],syms['first_strike']))
