# Changelog

## v0.3.1 (2026-10-24)

### Added

- `experimental=1` in `config.txt` turns on panel refresh from the quick menu: with a game
  running, the FPS row gains `50Hz`, `45Hz` and `40Hz` after `60fps` (a 60fps limit at that
  refresh), applied at once and saved per game as `panel=`. At a 60fps limit the overlay's
  NOT HELD tip suggests a refresh rate the game runs 15% above (`NOT HELD, TRY 45HZ`), except
  for games the compatibility list marks speed-tied. The entries are not offered on a PS TV.
- `tips=0` in `config.txt` turns tips off: no TIP row on the overlay and no `(!)` in the
  quick menu.

### Changed

- Panel refresh is now opt-in: without `experimental=1` a hand-written `panel=` is ignored.
  If you added one since v0.2.1, add `experimental=1` to `config.txt` and reboot.
- The Slim (PCH-2000) accepted 40 Hz in two user reports with no flicker; whether its screen
  refreshes at 40 Hz is not confirmed yet.
- README screenshots retaken on the current overlay: Dead or Alive 5 Plus with Lowlat and
  Antilag off and on, and The Swapper at Default and at a 30 limit.
- Antilag is now simply on or off, and on by default. On is the fresh poll plus the
  whole-vblank sleep, which only runs at a 30 or 20 limit and backs off by itself after a
  missed frame. The separate `auto` mode (fresh poll only) is gone: it was a third state
  hidden behind one checkbox. An `antilag=auto` in `config.txt` or on a game's line reads as
  on; the menu and Save now write `0` or `1`.
- Overlay: Mini shows `AL` when Antilag is on; the ANTI-LAG row reads `OFF`, `ON / n` or the
  sleep in force; the tip is `TRY ANTI-LAG` and only shows while Antilag is off.

### Fixed

- The overlay's INPUT read `-` and QUEUE stayed stale on games whose flips name their
  framebuffer (The Swapper, Minecraft, Dragon Quest Builders): the matched frame was skipped
  instead of measured. The `log=1` rate lines had the same fault (`fscan`, `flag`).

## v0.3.0 (2026-10-23)

### Added

- `log=1` in `config.txt`: the helper writes a `helper: rate` line (frame rate, CPU time,
  input age, options in force) to `helper.log` every 3000 frames. Off by default.
- `tools/ingest_playlog.py`: pulls those lines off the console over FTP into a TSV for a game
  report. See "Reporting a game" in the guide and README.

### Changed

- Quick menu: Save moves to the bottom right, the Limiter to the FPS row, the Vsync checkbox
  (shown only with `vsync=0` or `1`) next to Save.
- The compatibility warning (`(!)` and the overlay's known-problem tip) no longer shows when
  the limit equals the game's own rate, where a speed-tied game runs at its normal speed.
- Panel refresh (`panel=`) is documented in the guide, still experimental: tested on the OLED
  model (now including sleep and resume); the Slim should accept it but is untested.
- Documentation rewritten: README, guide, developer guide, compatibility list and latency
  measurements.

### Fixed

- Panel refresh: putting the Vita to sleep while a game ran at a panel rate left the home
  screen's top layers flickering after the game exited, until the next sleep or a reboot.
  The kernel now restores the stock timing before sleep and re-applies the rate after resume.
- The Vsync checkbox is saved to `config.txt` at once; before, a toggle was lost at reboot
  unless another global setting changed after it.

## v0.2.1 (2026-10-20, experimental)

### Added

- Experimental per-game panel refresh: `,panel=<hz>` (24 to 59) on a title's `state.txt` line
  runs the screen at that rate while the title runs, and the limits count in its refreshes
  (the 60 limit at `panel=40` is an even 40 fps). OLED Vita (PCH-1000) only, tested on
  firmware 3.65; anything else is left alone. No quick-menu control: the overlay shows `40HZ`
  (Mini) or `@40HZ` (Full), and `helper.log` logs `helper: panel want=40 got=40`. 59.94 Hz
  returns when the title exits. Input lag rises with the lower refresh; see the release notes.
- `Save` keeps `panel=` on the title's line, like `read=peek`.

## v0.2.0 (2026-10-20)

### Added

- `antilag=auto`: the fresh poll is on by default for fresh installs, while the whole-vblank
  sleep stays off until `antilag=1` is set. The overlay shows `AUTO` (Mini: `AL`); the quick-menu
  checkbox is checked under auto, unticking it turns Antilag off and ticking it again selects
  full Antilag. Back to auto is `antilag=auto` in `config.txt` or on the title's `state.txt`
  line, by hand. The overlay's `TIP` says `TRY FULL ANTI-LAG` when only the whole-vblank sleep
  would pay. A `config.txt` with no `antilag=` line means auto; existing files keep their
  explicit `antilag=0` or `antilag=1`. The lite helper honours auto too.
- `helper: env` at game start: the title's VitaGrafix section (`ENABLED`, `IB`, `FB`, `FPS`)
  and the current ARM/GPU/bus clocks, in `helper.log`.
- Known problems from the compatibility list's incompatible table, at the limit they apply
  to: the quick menu shows `(!)` next to the FPS row, and the overlay's `TIP` row shows the
  problem (`NO INPUT ABOVE 59`, `FREEZES, USE DEFAULT`, `SHIP SHAKES WITH CAP`,
  `VIDEOS BREAK AT 60`, `MENUS RUN FAST`, `SLOW MOTION AT 20`, `SPEED TIED TO FPS`) when no
  measured tip applies (`NOT HELD`, `TRY FULL ANTI-LAG` and `TRY LOW LAT` win). The table is
  generated from `docs/compatibility-list.md` by `tools/gen_compat.py`; `tests/run.sh` fails
  if `shell/compat_data.h` is stale.
- Per-game `read=peek`: converts a title's blocking `sceCtrlRead*` to the matching
  `sceCtrlPeek*`, a fix for titles that lose input when they run above their own rate.
  `Save` preserves the key, and the `(!)` and `NO INPUT ABOVE 59` warning go away once it is
  set. Anti-lag's fresh poll re-polls through
  the Peek on these titles.

### Changed

- Mini's second line uses short forms (`VSYNC`, `LL`, `LL?`, `AL`, `AL+`, `LIM 40`), so it fits
  the box in the right-hand corners, where the full words ran off the screen.

- The Limiter now holds the submit thread to a fixed schedule: each slot is one period after
  the previous one, so the average rate is the cap exactly. A frame late by less than a period
  gets a shorter next slot and the schedule catches up; a frame later than that restarts the
  schedule instead of bursting.
- The fresh poll is skipped on a blocking `sceCtrlRead*` that asks for more than one sample,
  where a re-poll would drop samples.
- The native-rate suffix is now `(og:60)` in the quick menu and overlay, replacing
  `(game 60)`. The shorter form fits the FPS row and names the original rate plainly.

## v0.1.1 (2026-10-19)

### Fixed

- The Vita hung on the boot logo unless kubridge v0.3 or later was installed: the menu plugin
  called kubridge's exception API for its crash log without checking that it exists, and
  kubridge v0.1/v0.2 or no kubridge at all crashed SceShell at startup. kubridge is now
  optional ([#2](https://github.com/esp1esp1/fpsvita/issues/2)).

### Changed

- Docs: a compatibility list (games known fine at a given limit, games with good latency
  measurements, games reported speed-tied), and the README and guide now say plainly that a
  limit other than Default only suits games that keep their own time. The guide's advice to
  use 30 on a game that runs double speed at 60 was wrong for 60 fps games; it is Default now.

## v0.1.0 (2026-10-17)

First release. Changes since v0.1.0-beta.1, which was a code review and a documentation pass
over the beta.

### Fixed

- Forced vsync (`vsync=1`) was cleared whenever the quick menu opened on a title whose profile
  is Default or Uncapped, and the next HUD or Latency click then wrote `vsync=0` to
  `config.txt`. The menu now only greys the checkbox out.
- Titles whose main module imports only the controller (HELP NO BRAKE, DriftR) could not be
  limited at all: the helper stopped looking once it had any hook. It now waits for the display
  imports, which on these titles arrive with a module loaded later, and the saved profile
  applies.
- Forced vsync falls back to the present call when a game stops going through the GXM display
  queue (video playback, a 2D menu) instead of pacing nothing while the overlay said it was.
- The Vsync label and checkbox were drawn off screen whenever `vsync=0` or `vsync=1` was set.
- The overlay reports a game that already runs a one-deep display queue as `ON (GAME'S OWN)`
  instead of `ON UNTIL RESTART`.
- After a game was killed without exiting, the next title inherited its latency flags as the
  values to restore; the globals from `config.txt` are restored now.
- No more periodic flash writes from a game's display thread: the per-frame log line was a
  development instrument and is no longer in release builds. `helper.log` is written at game
  start only.
- One `config.txt` write per FPS-row press instead of two when cycling through Tearing.
- The kernel module accepts the Tearing flip and the overlay numbers only from the running
  game's own process.
- Shell: an overlay page is no longer leaked when its template fails to open; the crash
  handlers are released when the module unloads; a `config.txt` too large to read is left
  alone instead of being overwritten with defaults.
- Two statistics races that could poison the overlay's INPUT number.

### Changed

- Quick menu: an fpsvita icon to the left of the FPS row, the overlay corner and the Limiter
  as labelled `Corner:` and `Limiter:` pairs on the right edge, and a `Latency:` label, so the
  rows read like the system's own.
- README: step-by-step install, the guide and the Framecapper warning up front, a changelog.
- Guide and measurements: anti-lag numbers match the measurement table (11 to 24 ms on most
  titles, nothing on games that read the controller mid-frame); the Lowlat gain on Dead or
  Alive 5 Plus is listed; forced vsync is documented as a global setting; hand edits to
  `config.txt` are documented as needing a reboot; auto-detect and `helper.log` are described
  as implemented; the homebrew port listed as "Road Rage 2" is The Simpsons: Hit & Run.

## v0.1.0-beta.1 (2026-10-17)

First public beta: frame limits from the quick menu (Default, 60, 30, 20, Uncapped, Tearing),
per-game profiles, anti-lag, low latency, a Limiter, the overlay, and a lite single-plugin
variant.
