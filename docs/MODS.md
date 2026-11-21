# Mods: texture packs and music packs

Both live in folders next to the game (`textures` and `music`) and are
turned on and off in the **Mods** menu. Nothing in the game's files is
changed: take the folder away and the game is as it was.

## Texture packs (Enhanced mode)

PNG pictures anywhere in the `textures` folder (outside `textures\dump`)
are drawn instead of the game's textures. Each is named after the texture
it replaces.

To make one:
1. Turn on **Mods > Dump textures** and play. Every texture shown is saved
   in `textures\dump`.
2. Redraw or upscale those PNGs. Keep their names and their see-through
   parts. A bigger picture than the original is fine.
3. Put them in `textures` (any subfolder except `dump`) and choose
   **Mods > Reload the texture packs**.

Hold **`** (the key left of 1) to see the game's own textures and compare.
**Upscale the 3D textures** enlarges the game's own textures 4x with xBR;
a pack's pictures come first.

## Music packs

OGG (Vorbis), FLAC or WAV recordings in the `music` folder play instead of the
game's songs. Each is named after the song it replaces: `BG_012.ogg`,
`ME_003.flac`. A title after the name is fine (`BG_012 - Town.flac`), and
so is `BG_012.ogg.flac` (what renaming gives when Windows hides
extensions): the file's contents say whether it is OGG or FLAC.

- **Mods > Open the music folder** opens it and writes `songs.txt`, the
  list of every song (`BG_` songs, `ME_` jingles).
- **Mods > Show each song's name as it starts** puts the name in the
  window's title, so you can tell which one is playing.
- **Mods > Reload the music** after adding files: each takes over the next
  time its song starts.
- **Mods > Export the game's songs** writes every song as a WAV file to
  `music\export` (about 650 MB, a minute or two), made from the ROM's own
  sequences and instruments: close to the game's sound, not exact. Each
  file holds the intro and one pass of the loop, and `loops.txt` lists the
  loop points (as `LOOPSTART` / `LOOPLENGTH` in samples) to line a
  recording up with. A WAV with its loop (the "smpl" chunk, which sound
  editors and samplers keep) also works in the `music` folder, so an
  exported song can be edited and put back.

Looping: songs loop between the `LOOPSTART` and `LOOPSTART + LOOPLENGTH`
tags (or `LOOPEND`), in samples, the usual tags of game music rips.
Without them, songs loop the whole file and jingles play once.

The game's own song keeps playing silently (every note at volume 0), so
its timing, loops and anything the game waits for stay exactly the same;
the recording follows the game's fades, pauses and stops.
`music_pack_volume` in `settings.ini` (0 to 200, default 70) sets how loud
the recordings are next to the game's sound effects.

While playing together (Wireless), the game's own music plays: the
players' games must stay identical in memory, and a silenced song is a
change in memory. Replays of bug reports also leave music packs off.

Packs you share should contain only your own recordings or ones you have
the right to share.

## Transmog

A character can look as if they wore other equipment, while what they
really wear (and so every number: attack, defence, resistances) stays the
same. Turned on and off with **Mods > Transmog**.

1. Play until the characters you want to change have been on screen, then
   choose **Mods > Choose the looks (transmog.ini)**. It opens
   `transmog.ini` (next to the game) with a section for each character the
   game has shown, a line for each slot, and what is worn there now:

   ```
   [Riastea]
   torso     = worn    # wears 13619
   legs      = worn    # wears 16214
   head      = worn    # wears 12410
   ...
   ```

2. Replace `worn` with the number of the item that slot should look like
   (`torso = 13621`), and save the file. **Mods > Item numbers for
   transmog** writes and opens `transmog_items.txt`: every piece of
   equipment's number, by slot, with its name in your game's language (read
   from your ROM). The `# wears` numbers of your other characters work too. `worn` shows what is worn;
   `empty` shows the slot as if nothing were worn there (`head = empty`: no
   helmet, the hair shows). (`torso: 13621` works
   too; what follows a `#` is only a note.) Each slot takes its own kind of
   item: head 12xxx, torso 13xxx, arms 14xxx-15xxx, legs 16xxx, feet 17xxx,
   accessory 18xxx, weapon 19xxx-20xxx, shield 21xxx; a number of another
   kind is ignored (Reload says so), since the game would make a broken
   model of it.
3. Choose **Mods > Reload transmog.ini**. It says how many looks it found,
   and the party is dressed again at once.

Nothing in your save changes: take `transmog.ini` away (or turn Transmog
off) and everyone looks as they are. Characters are found by name.

How it works and what it covers:
- The game keeps a cache of each party member's equipment and adds up their
  numbers from it, so the cache always holds what is really worn. When the
  game fills it, it is asked once more with the chosen looks, to learn their
  model numbers, and then fills it again with the real equipment. Only while
  the file name of one model part is made does the cache say the chosen look.
  Reload (and turning Transmog on or off) makes the party's models again the
  way an equipment change does. A save made with and without transmog, also
  after a Reload while playing, is identical byte for byte.
- It changes the party's 3D models: on the field, in battles and in the
  cutscenes played with those models. Menus (the equipment preview) and
  cinematics that draw characters their own way still show what is worn.
- A weapon look is used only on a weapon of the same kind (the same
  hundreds: 200xx swords, 203xx whips, 209xx bows, ...). In battle a weapon
  is held, drawn and swung the way its real kind is (a whip is only drawn
  when it strikes, a bow is held in the other hand), so another kind would
  not match the moves; such a look is left out and the real weapon shown.
- A slot where nothing is worn shows nothing: a shield look needs a shield
  (any) in hand, a helmet look something on the head.
- `empty` uses what the game itself shows for an empty slot (func_02072afc):
  nothing for the head, weapon, shield and accessory (without a helmet the
  hair shows), and the game's stand-ins for the body (what is under the
  armour; for the arms, the torso's sleeves). Their cache entries are learnt
  from the game. In battle, a whip's or bow's strike still shows the real
  weapon for a moment when the weapon slot is `empty` (the game names that
  model on its own and needs one).
- While playing together (Wireless) and in replays of bug reports, everyone
  looks as they are: the games must stay identical in memory.
