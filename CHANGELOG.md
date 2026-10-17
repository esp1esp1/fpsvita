# Changelog

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
