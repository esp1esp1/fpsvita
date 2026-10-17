# fpsvita

Frame-rate limits, anti-lag and tearing for PS Vita games, controlled live from the quick menu.

**What it does**

- **Frame limit on the fly.** Default, 60, 30, 20, Uncapped or Tearing, switched while the game
  runs. Cap a stuttery 60 fps game to a solid 30, or let a 30 fps game run at 60 if it can.
- **Per-game profiles.** Save stores the whole row for that title; it is applied at every later
  launch and the global defaults come back when the game exits.
- **Anti-lag.** Most games read the controller a hair before the new sample arrives and play on
  input a whole frame old. Anti-lag fixes that: 11 to 24 ms less input-to-screen latency on
  most of the titles measured, with no change in frame rate. It cannot help a game that
  reads the controller mid-frame; the guide lists both kinds.
- **Low latency, Limiter, Tearing.** One frame less in the display queue; a limiter for any
  rate (40, 45, 50) without vsync; and PC-style vsync-off tearing for the lowest latency the
  hardware allows.
- **Overlay.** Frame rate, frametime, CPU time, and the number that matters: input-to-screen
  latency, per frame. Mini (two lines) or Full (a box), in any corner.
- **Lite mode.** One plugin and a text file, if you don't want the menu.

## Install

You need a Vita running HENkaku or ensō (so taiHEN is loading plugins) and a way to copy
files onto it, such as VitaShell's FTP or USB mode.

1. Download `fpsvita-v0.1.0.zip` from the
   [Releases](https://github.com/esp1esp1/fpsvita/releases) page and unzip it. Inside are a
   `ur0` folder, a `lite` folder and the docs.
2. Copy the **contents** of the `ur0` folder onto the Vita's `ur0:` partition, keeping the
   folder structure. Afterwards these four files must exist on the Vita:

   ```
   ur0:tai/fpsvita-kernel.skprx
   ur0:tai/fpsvita-ui.suprx
   ur0:tai/fpsvita-helper.suprx
   ur0:data/fpsvita/fpsvita_plugin.rco
   ```

   `ur0:data/fpsvita` does not exist yet on a fresh install; create it. Skip the `lite` folder,
   it is a separate variant (see [Lite mode](#lite-mode)), never install both.
3. Open your taiHEN config in a text editor (in VitaShell: highlight the file, Triangle, Open
   with Text Editor). It is `ur0:tai/config.txt`, **unless** `ux0:tai/config.txt` exists, in
   which case taiHEN uses that one and you must edit it instead. Add one line under each of
   these three section headers. The headers are probably already there; if a header is
   missing, add it:

   ```ini
   *KERNEL
   ur0:tai/fpsvita-kernel.skprx

   *main
   ur0:tai/fpsvita-ui.suprx

   *ALL
   ur0:tai/fpsvita-helper.suprx
   ```

   If you have PSVshellPlus, put the `fpsvita-ui.suprx` line **above** its `*main` line so the
   fpsvita rows appear above System Control (below it puts them underneath).
4. Reboot the Vita. Start any game, hold the PS button, and scroll down in the quick menu: the
   three fpsvita rows appear above System Control. If they do not, see
   [Good to know](#good-to-know).

**Updating:** replace all four files with the ones from the new zip, then reboot. Never mix
files from two versions. **Uninstalling:** remove the three lines from the config, reboot,
delete the files; `ur0:data/fpsvita/` also holds your settings, delete it if you do not want
them back later.

## Using it

**Read [docs/GUIDE.md](docs/GUIDE.md) before you change a game's rate.** It explains what each
mode and option does, what it costs, and what to do when a game misbehaves. The short version:
a game that speeds up at 60 fps ties its speed to the frame rate, and only its own rate fixes
that.

**Do not run Framecapper alongside fpsvita.** Both hook the same display waits, and the result
is undefined. VitaGrafix coexists; its FPS patches show up as the `(game ..)` value. Other plugin
combinations are untested.

Open the quick menu (hold PS) with a game running and scroll down:

```
┌─────────────────────────────────────────────────────────────┐
│ ▣ FPS:     [<]  30fps (game 60)  [>]                 [Save] │
│   HUD:     [<]  Mini  [>]                 Corner: [<] TL [>] │
│   Latency: [ ] Lowlat  [ ] Antilag       Limiter: [<] off [>] │
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
45 fps or for games that cannot be limited the normal way. The guide has the reasoning behind
each, with numbers.

**Vsync checkbox.** Appears only when `config.txt` has `vsync=0` or `vsync=1` instead of the
default `vsync=auto`. With auto, forced vsync engages by itself on the few games that never
wait for the screen. The checkbox is a global setting, not part of a title's profile.

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
  title. Plain text, written by the menu. `state.txt` is safe to edit at any time; the menu
  only reads `config.txt` at boot and rewrites it on every change, so reboot after editing
  that one.
- **Tested on** one retail Vita with ensō on 3.65; 3.60 should work. If a title misbehaves,
  please [open an issue](https://github.com/esp1esp1/fpsvita/issues) with its id and the lines
  from `ur0:data/fpsvita/helper.log`.
- **If the quick menu rows are missing**, first check the four files and three config lines
  above. If the rows were there and disappear after a boot, hold L while powering on to skip
  plugins, comment the `fpsvita-ui` line out of the config, and open an issue with
  `ur0:data/fpsvita/boot.log`.
- **Changes between versions:** [CHANGELOG.md](CHANGELOG.md).
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
