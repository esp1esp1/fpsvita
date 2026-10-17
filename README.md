# fpsvita

Live frame-rate control for the PS Vita, from the quick menu.

Three taiHEN plugins add a row to the quick menu that switches the running game between
**Default**, **60fps**, **30fps**, **20fps**, **Uncapped** and **Tearing** on the fly, saves a
per-game profile, cuts input latency with an anti-lag option, and can draw an overlay with the
measured rate, frametime and input-to-screen latency. No reboot, no recompile, no per-game
config files.

If you know the Steam Deck, RTSS or the NVIDIA control panel: the FPS row is the frame-rate
limit (Default means the game's own, not "no limit"), Vsync is "force vsync on", Low lat is
"low latency mode: on" (one pre-rendered frame), Anti-lag is "ultra" (just-in-time input),
Limiter is an RTSS-style limiter without vsync, Tearing is the Deck's "allow tearing".

Works alongside PSVshellPlus and Quick Menu Plus without modifying either.

## Quick menu

```
┌─────────────────────────────────────────────────────────────┐
│  FPS:     [<]  30fps (game 60)  [>]                  [Save] │
│  HUD:     [<]  Mini             [>]   [<]  TL  [>]          │
│  Latency  [x] Lowlat   [x] Antilag   [<] off [>] Limiter    │
├─────────────────────────────────────────────────────────────┤
│  (PSVshellPlus rows below)                                  │
└─────────────────────────────────────────────────────────────┘
```

- **FPS row.** Left/right cycles Default, 60fps, 30fps, 20fps, Uncapped, Tearing. `(game 60)`
  is the rate the game asked for itself. **Save** (only with a game running) stores the whole
  row set as that title's profile; the helper applies it at every later launch. With no game
  running the values are the global defaults.
- **Vsync checkbox.** Forces a vsync wait on titles that never wait themselves. Hidden while
  `vsync=auto` (the default) is set in `config.txt`, where it engages by itself.
- **HUD row.** Overlay Off, Mini or Full, and its corner: TL, TR, BL, BR. Global, immediate.
- **Latency row.** Lowlat, Antilag and the Limiter, explained in `docs/GUIDE.md`. With a game
  running they change that title live (Lowlat from its next launch) and are stored by Save;
  with no game running they edit the global defaults.

## Modes

| Mode | Effect |
|------|--------|
| Default | The game's own rate. The helper's hooks chain straight through. |
| 60fps | Every display wait becomes a one-vblank wait. |
| 30fps | Two vblanks. |
| 20fps | Three vblanks. |
| Uncapped | Display waits return at once; the game runs as fast as it renders, the panel shows the newest complete frame at each refresh (no tearing). |
| Tearing | Uncapped, and every flip takes effect at once instead of at the next refresh: the lowest latency the hardware allows, with visible tearing. Needs the kernel module (not in lite mode). |

The capped waits are issued through the helper's own SceDisplay import, which is what makes the
cap hold on titles where passing a changed count down the hook chain does not.

**Vsync (inject).** Some titles never call a display wait and pace only on the present. For
those the helper adds a vblank wait around the present itself. With `vsync=auto` (default)
this engages on its own when no display wait has been seen for 30 presents **and** the game is
presenting at least 10 percent faster than the cap, and releases after a sustained run of paced
presents. The rate gate keeps the plugin from stacking a second wait on a title that another
plugin (VitaGrafix's `withWait` patches, for instance) or a sub-module already paces.

**Anti-lag.** Controller samples land once per vblank, and most games poll right after the
vblank, a hair before the new sample lands, so they play on input a whole frame old. Anti-lag
re-polls until the fresh sample is in (a sub-millisecond spin), and at a 30 or 20 limit also
sleeps whole vblanks when the frame would otherwise just sit in the display queue. Measured
2026-10-17: 11 to 25 ms less input-to-screen on every capped title tested, frame rates
unchanged (`docs/latency-measurements.md`).

## Overlay

Mini is two green lines in the style of PSVshellPlus's micro view: `30 FPS  LIMIT 30 / GAME 60`,
then whatever is active right now: `VSYNC`, `LOW LAT`, `ANTI-LAG 16.7 ms`, `LIMITER 40`.
Full is a box like PSVshellPlus's, one label and value per row: FPS (with frametime), LIMIT,
CPU (frame time on the submitting thread), INPUT (the frame's newest controller sample to its
flip), QUEUE (its submit to flip), ANTI-LAG, LOW LAT, LIMITER, TITLE. The overlay is drawn by
the SceShell plugin over the game, like PSVshellPlus's HUD, from numbers the helper pushes four
times a second while the overlay is on. It costs nothing while off.

## Config and state files

Two files in `ur0:data/fpsvita/`, both written by the quick menu and read by the helper at
game start: `config.txt` holds the global defaults, `state.txt` one profile line per title.
Both are plain text and can be edited by hand; the keys are listed in `docs/GUIDE.md`.

## How it works

| Plugin | Section | Role |
|--------|---------|------|
| `fpsvita-kernel.skprx` | `*KERNEL` | Shared state in kernel memory: mode, inject, auto-detect, native rate, title id, overlay settings and the live stats block. Syscall accessors; the read is a single copy-out. |
| `fpsvita-ui.suprx` | `*main` | Runs in SceShell. Hooks the quick menu the way PSVshellPlus does, builds the two rows from `fpsvita_plugin.rco`, persists the state file, and draws the overlay. |
| `fpsvita-helper.suprx` | `*ALL` | Runs in every game process. Hooks the display waits, the present, GXM init, the display-queue submit and the controller reads of the main module by NID. If the main module imports none of them (some engine ports load their engine later) it hooks the module loaders and retries on each late module. Reads the kernel state on every hook call. |

Each hook costs one kernel call. The helper has no threads and reads the state file once at
game start; the only writes are two one-line reports per game process to
`ur0:data/fpsvita/helper.log`.

## Lite mode: one plugin, no menu

`fpsvita-helper-lite.suprx` is the helper built without the kernel module or the quick menu.
Install only that file under `*ALL`, write `config.txt` and `state.txt` by hand in the format
above, and every feature of the helper works except Tearing: limits, per-game lines, automatic
vsync, lowlat, antilag, the limiter, the rate lines in `helper.log`. There is no live control and no overlay, and a
change to the files takes effect at the title's next launch. Never install the lite helper
together with the full set.

## Installation

Copy:

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
ur0:tai/PSVshellPlus_Shell.suprx

*ALL
ur0:tai/fpsvita-helper.suprx
```

`fpsvita-ui.suprx` must come **before** `PSVshellPlus_Shell.suprx` in `*main`, otherwise the
rows appear below PSVshellPlus's. Reboot after editing. When updating, replace all four files
together: the three modules share one state layout.

If SceShell stops coming up after a change, hold L while powering on (ensō skips plugins),
start VitaShell's FTP and comment the `fpsvita-ui` line out. `ur0:data/fpsvita/boot.log`
records the shell plugin's milestones and, through kubridge, the pc, lr and owning module of a
SceShell fault.

## Building

See `docs/HACKING.md`: prerequisites, the build, the architecture and the host tests.

## Other plugins

Framecapper must not be installed alongside fpsvita: both hook the same display waits. VitaGrafix
coexists; its FPS patches change the rate a game asks for, which shows up as the `(game ..)`
value. PSVshellPlus and Quick Menu Plus share the quick menu and need only the ordering above.
Other combinations are untested.

## Compatibility

Developed and tested on one retail Vita with ensō on firmware 3.65. 3.60 should work; nothing
else has been tried. This is a beta: expect rough edges, and please report the title id and the
`ur0:data/fpsvita/helper.log` lines with any problem.

## Credits

- Vblank hook approach from [Framecapper](https://github.com/Rinnegatamante/Framecapper) by Rinnegatamante.
- Quick menu and HUD integration pattern from [PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus) by GrapheneCt.
- ScePaf names and export lists from [ScePaf-RE](https://github.com/VitaArchive/ScePaf-RE).
- RCO built with [acdc](https://github.com/isage/acdc) by isage; crash traces through [kubridge](https://github.com/bythos14/kubridge) by bythos14.
- Terminology borrowed from the Steam Deck, RTSS and the NVIDIA control panel so the options read the way PC players expect.

## License

GPL-3.0, see `LICENSE`. Third-party components and their licences are listed in
`THIRD_PARTY_NOTICES.md`.
