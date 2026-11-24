#!/usr/bin/env python3
"""Extract the code from a DS ROM for the recompiler (replaces `dsd rom extract`).

  extract_rom.py <rom.nds> <out dir>

Writes the same layout dsd does, as far as the recompiler and the runtime use it:
  arm9/arm9.bin  arm9.yaml      the ARM9 program, decompressed, without its autoload blocks
  arm9/itcm.bin  itcm.yaml      the autoload blocks (instruction / data TCM)
  arm9/dtcm.bin  dtcm.yaml
  arm7/arm7.bin  arm7.yaml      the ARM7 program
  arm9_overlays/ovNNN.bin       each ARM9 overlay, decompressed
  arm9_overlays/overlays.yaml
Only the code is taken; the game's other files stay in the ROM, which the
game reads itself while it runs.
"""
import os, struct, sys

def u32(b, o): return struct.unpack_from('<I', b, o)[0]

def blz_decompress(data):
    """Nintendo's backward LZ ("BLZ"): the footer gives the compressed length and
    how much bigger the result is; decompression runs from the end to the start."""
    data = bytearray(data)
    top_hdr = u32(data, len(data) - 8)
    extra = u32(data, len(data) - 4)
    if extra == 0:
        return bytes(data)
    comp_len = top_hdr & 0xFFFFFF
    hdr_len = top_hdr >> 24
    out = bytearray(len(data) + extra)
    out[:len(data)] = data
    src = len(data) - hdr_len            # reading backwards from here
    dst = len(out)
    stop = len(data) - comp_len           # the compressed region starts here
    while src > stop:
        src -= 1; flags = data[src]
        for _ in range(8):
            if src <= stop: break
            if flags & 0x80:
                src -= 2
                pair = data[src] | data[src + 1] << 8
                n = (pair >> 12) + 3
                disp = (pair & 0xFFF) + 3
                for _ in range(n):
                    dst -= 1
                    out[dst] = out[dst + disp]
            else:
                src -= 1; dst -= 1
                out[dst] = data[src]
            flags = (flags << 1) & 0xFF
            if dst <= stop: break
    return bytes(out)

NITROCODE = struct.pack('<II', 0xDEC00621, 0x2106C0DE)

def yaml_write(path, d):
    with open(path, 'w', newline='\n') as f:
        for k, v in d.items():
            if isinstance(v, bool): v = 'true' if v else 'false'
            f.write(f'{k}: {v}\n')

def main():
    rom_path, out = sys.argv[1], sys.argv[2]
    rom = open(rom_path, 'rb').read()
    a9_off, a9_entry, a9_load, a9_size = (u32(rom, 0x20 + 4 * i) for i in range(4))
    a7_off, a7_entry, a7_load, a7_size = (u32(rom, 0x30 + 4 * i) for i in range(4))
    fat_off = u32(rom, 0x48)
    ovt_off, ovt_size = u32(rom, 0x50), u32(rom, 0x54)
    for d in ('arm9', 'arm7', 'arm9_overlays'):
        os.makedirs(os.path.join(out, d), exist_ok=True)

    # ARM9: find the module parameters (they end with the "nitrocode" magic)
    arm9 = bytearray(rom[a9_off:a9_off + a9_size])
    m = arm9.find(NITROCODE)
    if m < 0: sys.exit('no ARM9 module parameters: not a DS ROM?')
    p = m - 0x1C
    al_start, al_end, al_data, bss_start, bss_end, comp_end, sdk = struct.unpack_from('<7I', arm9, p)
    compressed = comp_end != 0
    if compressed:
        n = comp_end - a9_load
        arm9 = bytearray(blz_decompress(arm9[:n]) + arm9[n:])
        struct.pack_into('<I', arm9, p + 0x14, 0)          # as decompressed by the game
    # autoload blocks (ITCM, DTCM) follow the static program
    blocks, pos = [], al_data - a9_load
    for e in range(al_start - a9_load, al_end - a9_load, 12):
        dest, size, bss = struct.unpack_from('<3I', arm9, e)
        blocks.append((dest, bytes(arm9[pos:pos + size]), bss)); pos += size
    static = bytes(arm9[:al_data - a9_load])
    open(os.path.join(out, 'arm9/arm9.bin'), 'wb').write(static)
    yaml_write(os.path.join(out, 'arm9/arm9.yaml'), {
        'base_address': a9_load, 'entry_function': a9_entry, 'build_info': p,
        'autoload_callback': u32(rom, 0x70), 'overlay_signatures': 0, 'encrypted': False,
        'compressed': compressed, 'bss_start': bss_start, 'bss_end': bss_end, 'sdk_version': sdk})
    # One file per TCM. A ROM hack can add a block of its own (code at the top
    # of the ITCM, say) beside the game's: the blocks of one TCM are merged,
    # in address order, the space between them zero (as the game's own
    # autoload leaves it: the earlier block's bss, then untouched memory).
    tcm = {}
    for (dest, data, bss) in blocks:
        tcm.setdefault('itcm' if dest < 0x02000000 else 'dtcm', []).append((dest, data, bss))
    for name, parts in tcm.items():
        parts.sort()
        base = parts[0][0]
        merged = bytearray()
        for dest, data, bss in parts:
            if dest - base < len(merged): sys.exit(f'{name}: autoload blocks overlap at {dest:08x}')
            merged += bytes(dest - base - len(merged)) + data
        bss = parts[-1][2]
        if len(parts) > 1:
            print(f'{name}: {len(parts)} autoload blocks merged ({base:08x}-{base + len(merged) + bss:08x})')
        open(os.path.join(out, f'arm9/{name}.bin'), 'wb').write(bytes(merged))
        yaml_write(os.path.join(out, f'arm9/{name}.yaml'), {
            'base_address': base, 'code_size': len(merged), 'bss_size': bss, 'kind': name.capitalize()})

    # ARM7
    open(os.path.join(out, 'arm7/arm7.bin'), 'wb').write(rom[a7_off:a7_off + a7_size])
    yaml_write(os.path.join(out, 'arm7/arm7.yaml'), {
        'base_address': a7_load, 'entry_function': a7_entry, 'build_info': 0, 'autoload_callback': u32(rom, 0x74)})

    # ARM9 overlays
    lines = ['table_signed: false', 'overlays:']
    for i in range(ovt_size // 32):
        oid, ram, size, bss, ctor_s, ctor_e, fid, flags = struct.unpack_from('<8I', rom, ovt_off + 32 * i)
        start, end = u32(rom, fat_off + 8 * fid), u32(rom, fat_off + 8 * fid + 4)
        data = rom[start:end]
        comp = bool(flags >> 24 & 1)
        if comp: data = blz_decompress(data[:flags & 0xFFFFFF])
        name = f'ov{oid:03d}.bin'
        open(os.path.join(out, 'arm9_overlays', name), 'wb').write(data[:size])
        lines += [f'- id: {oid}', f'  base_address: {ram}', f'  code_size: {size}', f'  bss_size: {bss}',
                  f'  ctor_start: {ctor_s}', f'  ctor_end: {ctor_e}', f'  file_id: {fid}',
                  f'  compressed: {"true" if comp else "false"}', f'  signed: {"true" if flags >> 25 & 1 else "false"}',
                  f'  file_name: {name}']
    open(os.path.join(out, 'arm9_overlays/overlays.yaml'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    print(f'extracted ARM9 ({len(static)} bytes, {len(blocks)} autoload blocks), ARM7 ({a7_size} bytes), {ovt_size // 32} overlays')

main()
