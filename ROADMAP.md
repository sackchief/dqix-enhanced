# Roadmap

What's planned for DQIX Enhanced, roughly in order. Done items move to the
bottom with the version they arrived in.

## Next: v34

- [ ] **An in-game menu** drawn by the game itself (Esc, or a controller
      button): every option in a list over the game, easy with a controller.
      The menu bar only exists on Windows, so this is what opens up Linux,
      the Steam Deck and handhelds, and it gives controller players on
      Windows a menu too.
- [ ] **A first-launch choice**: "Like the DS" or "Enhanced (sharper 3D,
      Basic Widescreen)", asked once on a new install, so new players find
      what the project can do. Existing players keep their settings.
- [ ] **Steering the boat with the stick** in any direction, like walking.
- [ ] **Screenshots (F12) as you see them**: the whole window as a PNG in a
      `screenshots` folder, named by date and time (now: the DS's two
      screens as BMPs, numbered from 0 again every launch).
- [ ] **Dynamic Widescreen polish**: the Zoom menu's smear (#16), a flicker
      after leaving the attribute points menu, the paler sky at the far
      sides in battles, and whatever play testing turns up.
- [ ] The window light in the Upover inn flickers (#19; not on a DS).
- [ ] **The turnstile in Gortress's prison**: drawn away from where it
      is (its shadow is right), with one arm stretched far across the
      floor. Also in Classic mode, so it's in the core, not widescreen;
      to be compared with a reference emulator.
- [ ] **Bug reports that still replay after an update**: the report keeps
      the build's widescreen settings that change what the game counts as
      in view, and the replay uses them.
- [ ] Letters cut short in the battle panels ("Lv '1"; one player, maybe
      the European version with HD text).

## Then

- [ ] **Cheats** (Mods > Cheats): Action Replay codes, then simple switches
      like double experience or gold. Off while playing with friends.

## Linux, Steam Deck and handhelds

The game already runs on Linux (every automated test is a Linux build);
what's missing is everything around it.

- [ ] (In v34, above.) **An in-game menu**: needed on the Steam Deck,
      Linux, Android and handhelds like the Thor; also gives Mac a menu.
- [ ] **Installing on Linux**: rakmahefa's AppImage builder (v32) builds
      the game from your ROM; still to come: a desktop shortcut for the
      game and the updater without `.bat` files.
- [ ] **Steam Deck**: building without a compiler on SteamOS (a bundled
      toolchain for desktop mode, or build on any PC and copy over), adding
      it to Steam as a non-Steam game with a controller layout (the menu on
      a button), basic widescreen for the 16:10 screen. Until then, players
      can try the Windows build through Proton (untested).
- [ ] **Android**: the game built on a PC (phones can't run the build
      tools) and copied to the phone; the touchscreen as the DS's bottom
      screen; OpenGL ES versions of the shaders.
- [ ] **macOS**: Felix Bronnhuber's native Mac port, with a Mac tester.

## Multiplayer polish

- [ ] **Lag with four players** (reported after v32): find where the time
      goes (the relay's extra trips through the host, its 30 ms bundling at
      each end, the slowest player holding everyone back) and cut it.
- [ ] **The relay running out of its daily allowance**: on Cloudflare's free
      plan the relay hit its 100,000-request daily cap on 2026-10-30, and
      then nobody can join with a code until midnight UTC. Fewer messages
      (the host sending one bundle for all friends, not one per friend),
      direct connections first (hole punching, below), and the paid plan
      if it's still needed.
- [ ] **Lower input delay over long distances**: base it on the one-way trip,
      bundle less at the relay, and log every wait so lag can be traced.
- [ ] **The host window on Windows**: check the Copy button and the player
      list with 3-4 players.

## Modern options

Optional ways to play the DS never had, all off by default (Modern menu).

- [ ] **Widescreen, finishing touches**: in battles the sky picture is
      only as wide as the DS's screen, so the far sides show the game's
      paler backup sky. Pop-in at the sides while playing with friends.
- [ ] **A free camera**: turning further than the game allows in each place,
      and maybe tilt and zoom (the right stick already turns the camera as
      far as the game allows).
- [ ] Check the analog stick in battles and every kind of menu.

## Multiplayer

The original game never had these, because its wireless only reached DSes in
the same room.

- [ ] **Join a session that has already started.**
- [ ] **Hole punching**: with a code, try a direct connection first and use
      the relay only when that fails (a little less delay).
- [ ] **Check NAT-PMP, PCP and IPv6 on real routers** (tested so far against
      a stand-in router only).
- [ ] **Lower input delay when the DSes aren't talking wirelessly**, so walking
      around during a session feels as responsive as playing alone.

## Content

- [ ] **Hand-written weekly DQVC news**: one message per sale, written by hand
      in Sellma's voice, replacing the single surviving message from April
      2010. The game turns the text into the news download.
- [ ] **Optional: the downloadable quests one per week**, in the order Nintendo
      released them, instead of all at once.

## Look and feel

- [ ] **HD-2D and ray-traced lighting** (Felix Bronnhuber): depth of field,
      bloom, colour grading, ray-traced shadows and occlusion, as Enhanced
      options with an effects window. Shadows that follow the time of day
      (the sun's angle from the game's clock) and moonlight at night.
- [ ] **An HD battle HUD**: our own panels from the game's numbers (names,
      levels, HP and MP bars) in the HD fonts.
- [ ] **Model packs**: more detailed models for items, monsters, people and
      the world (equipment last, it has the most combinations), following
      the game's own skeletons and animations; a model dump tool.
- [ ] **2D graphics packs and upscaling** (menus, the font, the HUD).
- [ ] **Recording**: a long press of the Share button saves the last 30
      seconds as a video.
- [ ] **Fine-tune the DS Lite screen filter** against photos of a real DS Lite
      running DQIX (colours per backlight level, pixel grid, motion blur).
- [ ] **Save states.**

## Reach

- [x] (In v32.) **The European version, with its five languages** (English, French,
      German, Italian, Spanish; players have asked for them): a map of the
      European game made with the decomp's `dsd` tool (only the USA and
      Japanese maps exist), the USA-specific fixes found in it (the leak
      patch, the anti-piracy functions, thread switching, the 3D queue, the
      stylus-arrow tags), European DQVC and download data, and a Language
      menu that sets the DS's language. Needs a European cartridge file to
      build and test with. European and USA players can't play together.
- [ ] **The Japanese version** (its map already exists in the decomp).

## Loose ends

- [ ] Find out why one test save never canvasses another in tag mode (every
      other pairing works; probably something in those two saves).
- [ ] Speed: 528 small jump stubs in the game's code still go through a
      slower path.

## Done

- [x] **v33 released as stable**: Dynamic Widescreen battles (submenus
      shown whole, the panels in the party's order, a party of one, long
      messages above the panels), dialogue boxes with a tag and Yes/No kept
      centred, the Krak Pot without the screen swap.
- [x] **v32 released as stable** (from v32-nightly1 to nightly3 and the
      fixes after): the European version and its Language menu
      (romusan495), a Linux AppImage builder (rakmahefa), HD text (Arthur
      Ferreira Soares), the Mods menu (texture packs on romusan495's work,
      xBR upscaling, music packs, song export), Dynamic Widescreen redone
      (windows move right, 4:3 menu screens, the battle panels bottom left),
      full-screen effects and movies in widescreen, the Alchemimicon's
      screens swapped, the inn's guests, Discord activity.
- [x] Installer that repairs old MSYS2s, ROMs in a zip, Controls fixed,
      fullscreen fixes, quitting asks first, the stick's walking, smooth
      outlines and shadows, black sides for DS-only places, Tab fast
      forward, saves in and out, self-attaching bug reports (v32-nightly1).
- [x] **v31 released as stable** (from v31-nightly1).
- [x] Basic widescreen (corners, sizes, opacity on the stick clicks),
      dynamic widescreen kept as experimental, single screen and the
      triggers, reset (Ctrl+R, Start+Select+L+R), volume, Controls remapping
      (v31-nightly1).
- [x] **v30 released as stable** (from v30-nightly1): widescreen without
      pop-in at the sides.
- [x] Widescreen: people, doors, items and monsters at the sides no longer
      vanish (the DS's box test answers for the wide view) (v30-nightly1).
- [x] **v29 released as stable** (from v29-nightly4): the sharper Enhanced
      mode and the new screen layouts.
- [x] Enhanced: up to 12x and Auto resolution, sharp bilinear upscaling;
      widescreen with the small screen in its own window; a Fullscreen bar
      on the second window (v29-nightly4).
- [x] Screens: swap them (F9), two windows for two monitors, widescreen
      battle commands in the top left corner, see-through small screen (O)
      (v29-nightly3).
- [x] Enhanced mode: the DS's rules for see-through effects, fog and soft
      outlines (clouds and Yggdrasil's glow) (v29-nightly1).
- [x] **v28 released as stable**: a real session across thousands of miles,
      both on apartment internet, through the relay (from v28-nightly2).
- [x] The updater: a real update to v27 and a rollback, tested on Windows.
- [x] Join with a code through a relay, on any internet (v28-nightly2).
- [x] **v27 released as stable** (from v27-nightly21).
- [x] A setup window (`Install Dragon Quest IX.bat`) with shortcuts and an
      uninstaller; NAT-PMP, PCP and IPv6 for multiplayer; Send a bug report (v27-nightly21).
- [x] The right stick turns the camera; double-click leaves fullscreen
      (v27-nightly18).
- [x] Widescreen: the world drawn wider, the top screen as a minimap; the
      stylus arrow hidden while walking with the stick (v27-nightly13).
- [x] The game's own icon for dqix.exe and its window, made from the ROM;
      dqix.exe starts the game when double-clicked (v27-nightly12).
- [x] Modern options: walk in any direction with the left stick; the top
      screen in the corner for widescreen monitors (v27-nightly11).
- [x] DQVC and the downloadable quests built into the game; no extra files
      needed (v27-nightly10).
- [x] In-game updates from GitHub, stable or nightly (v27-nightly9).
- [x] Every player gets their own daily DQVC stock; visitors can shop at the
      host's Sellma (v27-nightly8).
- [x] 3 and 4 players in co-op and tag mode (v27-nightly7, nightly8).
- [x] DQVC on the original calendar: sales start on Fridays at 08:00 UTC, new
      stock every day (v27-nightly6).
- [x] DQVC inside multiplayer sessions; the host window; the DS Lite photo as
      the default console (v27-nightly5).
- [x] Sessions stay in sync after a network hiccup; a session log for bug
      reports (v27-nightly4).
- [x] Playing with friends far away: tag mode and co-op over the internet
      (v27-nightly2, nightly3).
