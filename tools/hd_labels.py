"""HD text: the database of the labels the game has as pictures (fonts/labels.bin).

    python3 tools/hd_labels.py spec.txt fonts/labels.bin

The game shows some text as pictures, not with its fonts ("Back", "HP", "Lv.",
the buttons of Stella's menu, "Menu" by the X button on the field...). With HD
text on, runtime/text_hd.c recognises their 4-bit tiles (by a 64-bit hash: the
file holds nothing of the game's graphics), takes their letters away and draws
the label's text again with the TrueType font.

A spec line describes one label, from a layer dump of a screen showing it
(a build with -DDQIX_PIXDBG, run with DQIX_LAYERDUMP=dir,frame):

    <ld.bin> <top|bottom> x0 y0 x1 y1 <font: 0 small, 1 main> [options] text

  x0 y0 x1 y1   the box around the letters (screen pixels, x1/y1 exclusive);
                keep button icons and box borders out of it
  options       layer=N   the layer (BG 0-3, 4 sprites; default: found)
                ink=e,f   the colour indexes of the letters and their outlines
                          (hex; default: the colours not common on the box's edge)
                main=f    the letters' own colour (default: the one least next
                          to the background)
                body=1,f  colours measured for the letters' size (default: main)
                style=bold,outline  keep the original's look: thicker letters,
                          and its outline drawn too (in the outline's colour)
                outline=2 the outline's colour index (when the letters have shades too)

One 4-bit tile (a sheet of digit sprites, say), shown wherever it is used:

    tile <ld.bin> <tile's offset in VRAM, hex> 8 8 x0 y0 x1 y1 <font> [options] text

A label in a 16-colour 3D texture:

    tex <ld.bin> <texture's offset in VRAM, hex> <w> <h> x0 y0 x1 y1 <font> [options] text

(x0..y1 in texels; DQIX_POLYAT=frame,x,y prints the texture under a pixel).
"""
import sys, struct, numpy as np
LD = np.dtype([('tile', '<u4'), ('tp', 'u1'), ('idx', 'u1'), ('flags', 'u1'), ('pad', 'u1'), ('pal', '<u2'), ('rgb', '<u2')])
cache = {}
def load(p):
    if p not in cache:
        raw = open(p, 'rb').read(); n = 2 * 5 * 256 * 192
        top = int(np.frombuffer(raw[:4], '<i4')[0])
        L = np.frombuffer(raw[4:4 + n * 12], LD).reshape(2, 5, 192, 256)
        vram = raw[4 + n * 12: 4 + n * 12 + 0xA4000]
        out = np.frombuffer(raw[4 + n * 12 + 0xA4000: 4 + n * 12 + 0xA4000 + 2 * 256 * 192 * 4], '<u4').reshape(2, 192, 256)
        cache[p] = (top, L, vram, out)
    return cache[p]
labels = []; keys = []; keyset = set(); texkeys = []
def fnv64(b):
    h = 14695981039346656037
    for x in b: h ^= x; h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h
for line in open(sys.argv[1]):
    line = line.strip()
    if not line or line.startswith('#'): continue
    tex = None
    tilekey = line.startswith('tile ')
    if tilekey: line = 'tex ' + line[5:]          # tile <ld.bin> <vram offset hex> 8 8 x0 y0 x1 y1 ...: one 4-bit tile (a sprite sheet's)
    if line.startswith('tex '):                   # tex <ld.bin> <vram offset hex> <w> <h> x0 y0 x1 y1 font [opts] text
        a = line.split(None, 10)
        path, toff, tw, th = a[1], int(a[2], 16), int(a[3]), int(a[4])
        x0, y0, x1, y1, font, text = *map(int, a[5:9]), int(a[9]), a[10]
        scr = 'tex'; tex = (toff, tw, th)
    else:
        a = line.split(None, 7)
        path, scr, x0, y0, x1, y1, font, text = a[0], a[1], *map(int, a[2:6]), int(a[6]), a[7]
    opt = {}                                      # options before the text: layer=N ink=e,f main=f bg=N
    while '=' in text.split(None, 1)[0]:
        k, v = text.split(None, 1)[0].split('=', 1); opt[k] = v; text = text.split(None, 1)[1]
    top, L, vram, out = load(path)
    eng = (0 if top else 1) if scr == 'top' else (1 if top else 0)
    if tex:                                       # a 16-colour texture: its texels as a layer
        toff, tw, th = tex
        tb = np.frombuffer(vram[toff: toff + tw * th // 2], 'u1')
        tidx = np.zeros((th, tw), 'u1'); tidx[:, 0::2] = (tb & 15).reshape(th, tw // 2); tidx[:, 1::2] = (tb >> 4).reshape(th, tw // 2)
        d = np.zeros((th, tw), LD); d['idx'] = tidx; d['tile'] = 1
        d = d[y0:y1, x0:x1]
        opt.setdefault('layer', '0')
    # the layer: the one most of whose drawn pixels there are what the screen shows
    best = None
    shown = out[eng, y0:y1, x0:x1].astype(int) if not tex else None
    if not tex: sr, sg, sb = (shown >> 16) & 255, (shown >> 8) & 255, shown & 255
    for layer in (range(5) if not tex else []):
        d = L[eng, layer, y0:y1, x0:x1]
        ok = (d['tile'] > 0) & ((d['flags'] & 2) == 0) & (d['idx'] > 0)
        if ok.sum() == 0: continue
        c = d['rgb'].astype(int)
        r, g, b = ((c & 31) * 255 + 15) // 31, (((c >> 5) & 31) * 255 + 15) // 31, (((c >> 10) & 31) * 255 + 15) // 31
        same = ok & (abs(r - sr) < 12) & (abs(g - sg) < 12) & (abs(b - sb) < 12)
        score = same.sum() * (len(np.unique(d['idx'][(d['tile'] > 0)])) >= 2)
        if score and (best is None or score > best[1]): best = (layer, score)
    if 'layer' in opt: best = (int(opt['layer']), 1)
    if best is None: print('NOT FOUND', line); continue
    layer = best[0]
    if not tex: d = L[eng, layer, y0:y1, x0:x1]
    else: d = np.zeros((th, tw), LD); d['idx'] = tidx; d['tile'] = 1; d = d[y0:y1, x0:x1]
    ok = (d['tile'] > 0) & ((d['flags'] & 2) == 0)
    idx = d['idx']
    # the background: the commonest colour on the box's edge (sprites: 0 where nothing is drawn)
    per = np.concatenate([idx[0], idx[-1], idx[:, 0], idx[:, -1]])
    pok = np.concatenate([ok[0], ok[-1], ok[:, 0], ok[:, -1]])
    per = np.where(pok, per, 0)
    vals, cnt = np.unique(per, return_counts=True)
    bg = int(vals[np.argmax(cnt)])
    if 'bg' in opt: bg = int(opt['bg'], 16)
    # the letters' colours: those not on the box's edge (the background's are)
    edge = set(int(v) for v, c in zip(vals, cnt) if c >= 0.1 * len(per))
    ink = ok & (idx != 0) & ~np.isin(idx, list(edge))
    if 'ink' in opt: ink = ok & np.isin(idx, [int(v, 16) for v in opt['ink'].split(',')])
    inkset = sorted(set(int(v) for v in idx[ink]))
    # the letters' own colour (the rest: outlines): the one least next to the background
    def touch(v):
        m = ink & (idx == v)
        nb = np.zeros_like(m)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            sh = np.roll(np.roll(~ink, dy, 0), dx, 1)
            nb |= sh
        return (m & nb).sum() / max(1, m.sum())
    main = min(inkset, key=lambda v: (touch(v), -int((ink & (idx == v)).sum())))
    if 'main' in opt: main = int(opt['main'], 16)
    ys, xs = np.nonzero(ink)
    tys, txs = ys, xs
    body = [int(v, 16) for v in opt['body'].split(',')] if 'body' in opt else [main]
    ys, xs = np.nonzero(ink & np.isin(idx, body))
    ix0, ix1, iy0, iy1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    # baseline: below the last row that is a good part of the letters' body (descenders are thin)
    rows = np.bincount(ys, minlength=y1 - y0)
    base = int(max(r for r in range(len(rows)) if rows[r] >= 0.25 * rows.max()) + 1)
    # the label's origin: the box's top left; its tiles
    lid = len(labels)
    reuse = None
    assert x1 - x0 <= 120 and y1 - y0 <= 18, 'label box too large (120 x 18)'
    lw, lh = x1 - x0, y1 - y0
    tiles = {}
    for yy, xx in zip(tys, txs) if not tex else []:
        t = d[yy, xx]
        tox, toy = x0 + xx - (t['tp'] % 8), y0 + yy - (t['tp'] // 8)
        tiles[int(t['tile'])] = (int(tox - x0), int(toy - y0))
    for tile, (dx, dy) in tiles.items():
        tb = bytes(vram[tile - 1: tile - 1 + 32])
        if (tb, lid, dx, dy) not in keyset: keyset.add((tb, lid, dx, dy)); keys.append((tb, lid, dx, dy))
    mask = sum(1 << v for v in inkset)
    # per row, what is behind the letters (gradients): the commonest other colour on it
    rowbg = []
    for r in range(lh):
        v = idx[r][ok[r] & ~ink[r]] if lh else []
        if len(v): u, c = np.unique(v, return_counts=True); rowbg.append(int(u[np.argmax(c)]))
        else: rowbg.append(bg)
    rowbg = tuple(rowbg + [bg] * (24 - lh))
    flags = sum({'bold': 1, 'outline': 2}[w] for w in opt.get('style', '').split(',') if w)
    if 'outline' in opt: flags |= int(opt['outline'], 16) << 4   # the outline's colour (others: shades, not measured)
    rec = (text, font, main, mask, bg, lw, lh, int(ix0), int(iy0), int(ix1), base, rowbg, flags)
    if tex and tilekey:                           # tiles (8 wide, 8 or 16 tall): shown wherever they are, the box's origin at (x0, y0)
        for k in range(tex[2] // 8):
            tb = bytes(vram[tex[0] + 32 * k: tex[0] + 32 * k + 32])
            key = (tb, lid, -x0, 8 * k - y0)
            if key not in keyset: keyset.add(key); keys.append(key)
    elif tex: texkeys.append((fnv64(bytes(vram[tex[0]: tex[0] + tex[1] * tex[2] // 2])), tex[1], tex[2], lid, x0, y0))
    if rec in labels:                          # the same label elsewhere: its tiles join the first's
        old = labels.index(rec)
        texkeys = [(a, b, c, old if l == lid else l, e, f) for a, b, c, l, e, f in texkeys]
        for k in range(len(keys)):
            if keys[k][1] == lid: keys[k] = (keys[k][0], old, keys[k][2], keys[k][3])
        keyset = set(keys); keys = list(dict.fromkeys(keys))
        print(f'  "{text}": same as label {old}')
        continue
    labels.append(rec)
    print(f'{lid}: "{text}" eng{eng} layer{layer} ink {inkset} main {main} bg {bg} box {lw}x{lh} ink x {ix0}-{ix1} top {iy0} base {base} tiles {len(tiles)}')
with open(sys.argv[2], 'wb') as f:
    f.write(b'DQL4'); f.write(struct.pack('<III', len(labels), len(keys), len(texkeys)))
    assert len(labels) <= 64, 'at most 64 labels (atlas cells)'
    for text, font, main, mask, bg, lw, lh, ix0, iy0, ix1, base, rowbg, flags in labels:
        tb = text.encode('latin1')[:47]
        f.write(struct.pack('<48sBBHBBBBBBBB', tb, font, main, mask, bg, lw, lh, ix0, iy0, ix1, base, flags))
        f.write(bytes(rowbg))
    for tb, lid, dx, dy in keys:
        f.write(struct.pack('<QHbb', fnv64(tb), lid, dx, dy))
    for h, w, hh, lid, bx, by in texkeys:
        f.write(struct.pack('<QHHHBB', h, w, hh, lid, bx, by))
print(len(labels), 'labels', len(keys), 'tiles')
