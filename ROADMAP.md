# Roadmap

What's planned for DQIX Enhanced, roughly in order. Done items move to the
bottom with the version they arrived in.

## Next: Linux and Steam Deck

The game already runs on Linux (every automated test is a Linux build);
what's missing is everything around it.

- [ ] **An in-game menu** drawn by the program (Esc or the controller's
      Home button), usable with mouse, keyboard or controller: every
      setting, bug reports, updates, hosting/joining, volume and controls
      without the Windows-only menu bar and windows. Also gives Mac a menu.
- [ ] **Installing on Linux**: a script that checks the compiler and
      libraries, builds the game from the cartridge file (build-game.sh)
      and adds a desktop shortcut; the updater without `.bat` files.
- [ ] **Steam Deck**: building without a compiler on SteamOS (a bundled
      toolchain for desktop mode, or build on any PC and copy over), adding
      it to Steam as a non-Steam game with a controller layout (the menu on
      a button), basic widescreen for the 16:10 screen. Until then, players
      can try the Windows build through Proton (untested).

## Multiplayer polish

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

- [ ] **Fine-tune the DS Lite screen filter** against photos of a real DS Lite
      running DQIX (colours per backlight level, pixel grid, motion blur).
- [ ] **Save states.**

## Reach

- [x] **European ROM** (YDQP): tools/region.py moves the USA symbol map's
      addresses; Language menu (English, French, German, Italian, Spanish).
- [ ] **Japanese ROM** in BUILD.bat.

## Loose ends

- [ ] Find out why one test save never canvasses another in tag mode (every
      other pairing works; probably something in those two saves).
- [ ] Speed: 528 small jump stubs in the game's code still go through a
      slower path.

## Done

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
