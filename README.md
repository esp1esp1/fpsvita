# fpsvita - Frame-rate limits, anti-lag and tearing for PS Vita games, controlled live from the quick menu.

**What it does**

- **Frame limit on the fly.** Default, 60, 30, 20, Uncapped or Tearing, switched while the game
  runs. Cap a stuttery 60 fps game to a solid 30, or let a 30 fps game run at 60 if it can.
- **Per-game profiles.** Save stores the whole row for that title; it is applied at every later
  launch and the global defaults come back when the game exits.
- **Anti-lag.** Most games read the controller a hair before the new sample arrives and play on
  input a whole frame old. Anti-lag fixes that: 11 to 25 ms less input-to-screen latency on
  every capped title measured, with no change in frame rate.
- **Low latency, Limiter, Tearing.** One frame less in the display queue; a limiter for any
  rate (40, 45, 50) without vsync; and PC-style vsync-off tearing for the lowest latency the
  hardware allows.
- **Overlay.** Frame rate, frametime, CPU time, and the number that matters: input-to-screen
  latency, per frame. Mini (two lines) or Full (a box), in any corner.
- **Lite mode.** One plugin and a text file, if you don't want the menu.

## Install

Grab the release zip and copy:

```
ur0:tai/fpsvita-kernel.skprx
ur0:tai/fpsvita-ui.suprx
ur0:tai/fpsvita-helper.suprx
ur0:data/fpsvita/fpsvita_plugin.rco
```

Add to `ur0:tai/config.txt`:

```ini
*KERNEL
ur0:tai/fpsvita-kernel.skprx

*main
ur0:tai/fpsvita-ui.suprx

*ALL
ur0:tai/fpsvita-helper.suprx
```

Put `fpsvita-ui.suprx` before `PSVshellPlus_Shell.suprx` under `*main` if you have it, so the
fpsvita rows come first (or after, if you prefer PSVshellPlus on top). Reboot. When updating,
replace all four files together.

## Using it

Open the quick menu (hold PS) with a game running and scroll down:

```
┌─────────────────────────────────────────────────────────────┐
│  FPS:     [<]  30fps (game 60)  [>]                  [Save] │
│  HUD:     [<]  Mini             [>]   [<]  TL  [>]          │
│  Latency  [x] Lowlat   [x] Antilag   [<] off [>] Limiter    │
└─────────────────────────────────────────────────────────────┘
```

**FPS row.** Left and right cycle the limit. `(game 60)` is the rate the game asked for by
itself, so you can see what you are changing.

| Limit | What you get |
|-------|--------------|
| Default | The game's own rate, untouched. |
| 60fps, 30fps, 20fps | The game waits one, two or three screen refreshes per frame. A steady 30 often feels better than a game that can't reach 60. |
| Uncapped | The game runs as fast as it renders; the screen shows the newest complete frame each refresh, no tearing. |
| Tearing | Uncapped, and each new frame goes to the screen immediately, mid-refresh. Lowest latency, visible seams. |

A game that speeds up at 60 ties its game speed to the frame rate. Put it back on its own rate
and Save; there is no way to fix that from outside.

**Save** stores the row as that title's profile. With no game running, the rows set the
defaults for everything.

**HUD row.** Overlay off, Mini or Full, and which corner.

**Latency row.** Antilag is the one to try first on everything. Lowlat helps titles that keep
their display queue full (the overlay's QUEUE row near a full frame says so) and applies from
the game's next launch. Limiter holds Uncapped games to a chosen rate without vsync, for 40 or
45 fps or for games that cannot be limited the normal way. The reasoning behind each, with
numbers, is in [docs/GUIDE.md](docs/GUIDE.md).

**Vsync checkbox.** Appears only when `vsync=auto` is turned off in `config.txt`. With auto on
(the default) forced vsync engages by itself on the few games that never wait for the screen.

## Overlay

Mini is two green lines: `30 FPS  LIMIT 30 / GAME 60`, then whatever is active: `VSYNC`,
`LOW LAT`, `ANTI-LAG 16.7 ms`, `LIMITER 40`. Full is a box with a row each for FPS and
frametime, LIMIT, CPU, INPUT (the frame's newest controller sample to its flip, which is
input-to-screen minus the panel itself), QUEUE (submit to flip), ANTI-LAG, LOW LAT, LIMITER and
TITLE. It costs nothing while off. Every row is explained in the guide.

## Lite mode

`fpsvita-helper-lite.suprx` is the whole helper with no menu and no overlay. Install only that
file under `*ALL`, write `ur0:data/fpsvita/config.txt` and `state.txt` by hand (keys in the
guide), and every option except Tearing works; changes apply at the game's next launch. Do not
install it together with the full set.

## Good to know

- **Files.** `ur0:data/fpsvita/config.txt` holds the defaults, `state.txt` one profile line per
  title. Plain text, written by the menu, safe to edit.
- **Other plugins.** Do not run Framecapper alongside fpsvita, both hook the same display
  waits. VitaGrafix coexists; its FPS patches show up as the `(game ..)` value. Other
  combinations are untested.
- **Beta.** Developed and tested on one retail Vita with ensō on 3.65; 3.60 should work. If a
  title misbehaves, please report its id and the lines from `ur0:data/fpsvita/helper.log`.
- **If the quick menu rows disappear after a boot**, hold L while powering on to skip plugins,
  then comment the `fpsvita-ui` line out of `config.txt` and send `ur0:data/fpsvita/boot.log`.
- **Building and how it works:** [docs/HACKING.md](docs/HACKING.md). The measurements behind the
  options: [docs/latency-measurements.md](docs/latency-measurements.md).

## Credits

- Vblank hook approach from [Framecapper](https://github.com/Rinnegatamante/Framecapper) by Rinnegatamante.
- Quick menu and HUD integration pattern from [PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus) by GrapheneCt.
- ScePaf names and export lists from [ScePaf-RE](https://github.com/VitaArchive/ScePaf-RE).
- RCO built with [acdc](https://github.com/isage/acdc) by isage; crash traces through [kubridge](https://github.com/bythos14/kubridge) by bythos14.

## License

GPL-3.0, see `LICENSE`. Third-party components and their licences are in
`THIRD_PARTY_NOTICES.md`.
