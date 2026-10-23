# HD text

F1 turns it on and off (on by default; `hd_text` in settings.ini).

The game's letters are drawn again at full resolution with TrueType fonts
(`fonts/`, copied into the game folder by the build). The game runs exactly
the same; only the picture changes.

How it works (runtime/text_hd.c, text_hd_glyphs.c, gpu2d.c, gl_render.c):

- **Tags.** The glyph routines are wrapped (`wrap.txt`, recomp `--wrap`):
  `func_0205b36c` (menus, text boxes: tiled canvases) and `func_02045864`
  (battle messages: plain 4-bit buffers that become 3D textures). For every
  nibble a glyph sets, a tag remembers the glyph instance, the pixel inside it
  and what was there before. Tags follow the pixels through DMA and the game's
  memcpy (`func_02001a40`); a CPU write forgets them (in main RAM only the
  nibbles it changes, unless the letter is older than 2 frames: a box border
  drawn into a letter's tile right after the text keeps the letter; in VRAM
  all of them, since textures are uploaded there with plain CPU copies).
- **2D.** The line renderer reads the tags of the pixels it shows: where each
  letter is on screen, its colour as shown, and the picture without the
  letters. A word the game copies to several places (the party's panels)
  gets one instance per place. Letters behind an alpha-blended layer count as
  visible. The GPU compositor draws the letters from an atlas (8 px per DS px,
  mipmapped) over that picture, before the 3D layer is blended in; letters
  over a bare 3D background are blended with it on their own.
- **3D.** A 16-colour texture with tagged texels is rebuilt 8x larger with
  its letters in HD (battle messages).
- **Shadows/outlines** of the letters are left out (`hd_text_shadows = 1` in
  settings.ini brings them back).
- **Instances.** One per glyph drawn (a ring; a slot whose letter is on
  screen is never reused). A word the game copies to several places keeps
  one copy per place from frame to frame.
- **Labels** the game has as pictures ("Back", "HP", "Lv.", Stella's menu,
  "Menu" by the X button...) come from `fonts/labels.bin`, made by
  `tools/hd_labels.py` (see the script for the spec format). It holds 64-bit
  hashes of the labels' tiles / textures, nothing of the game's graphics.
  Some are whole sheets: the digits of the HP/MP numbers (field and battle).
  A label can keep the original's look (`style=bold,outline`: thicker
  letters, and its outline drawn in the outline's colour): the active
  "Coup de Grâce", the battle digits.

Debugging (headless build, `-DDQIX_PIXDBG` for some): `DQIX_TXDUMP=dir,frames`
with `DQIX_TXHD=2 DQIX_TXFONTS=fonts` (the screens with HD text drawn on the
CPU, the picture without letters, the cells), `DQIX_LAYERDUMP=dir,frame`
(per layer and pixel: tile, colour index; input for hd_labels.py),
`DQIX_PIX=x,y` (what a pixel is made of), `DQIX_POLYAT=frame,x,y` (the 3D
polygon and texture under a pixel).
