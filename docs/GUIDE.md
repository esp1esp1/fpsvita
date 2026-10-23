# fpsvita user guide

For players who have installed fpsvita (see the [README](../README.md)): what each option
does, what the overlay says, and how to report a game.

- [How a frame reaches the screen](#how-a-frame-reaches-the-screen)
- [The quick menu](#the-quick-menu)
- [Options](#options)
- [What to try first](#what-to-try-first)
- [Overlay reference](#overlay-reference)
- [Troubleshooting](#troubleshooting)
- [Settings files](#settings-files)
- [Lite mode](#lite-mode)
- [Reporting a game](#reporting-a-game)

## How a frame reaches the screen

### Refresh and vblank

The Vita's screen redraws itself about 60 times a second (59.94 Hz), once every 16.7 ms. Each
redraw is a **refresh**. Between two refreshes there is a short pause, the **vertical blank**
or **vblank**.

A game draws each frame into memory, then hands it to the display. Showing the new frame is
called a **flip**. Flips normally happen at a vblank, so the screen never shows the top of one
frame and the bottom of the next. A flip in the middle of a refresh shows exactly that: a seam
across the picture, called **tearing**.

### The wait sets the frame rate

A game paces itself by waiting for vblanks. If it waits for every vblank, it runs at 60 fps.
If it waits for every second one, 30 fps. Every third, 20 fps. The rate a game asks for is its
**own rate**; the quick menu shows it as `og:30` or `og:60`.

fpsvita's frame limits change how long that wait lasts. At a limit of 30, every wait the game
makes lasts two vblanks. At Uncapped, the wait returns at once. Because a limit works through
vblanks, it comes in steps: 60, 30 or 20. The [Limiter](#limiter) reaches other rates with a
timer instead.

### Why some games break at another frame rate

Many games move their world forward by a fixed step each frame: a car travels a set distance
per frame, a timer counts frames. A game built this way for 30 fps runs at double speed at 60
and in slow motion at 20. Other games measure the time between frames and stay at the right
speed at any rate.

You cannot tell which kind a game is from outside it. The FPS counter looks right either way:
a speed-tied game happily reaches 60 fps while everything moves twice as fast. Only your eyes
tell you. Nothing outside the game can fix this, so fpsvita cannot either.

`Default` leaves the game's own pacing alone and is always safe. The
[compatibility list](compatibility-list.md) collects known results.

### Where input latency hides

Input latency is the time from pressing a button to seeing the result. A frame passes
through five steps on its way to the screen, and each adds some:

1. **Read.** The controller is sampled once per refresh. The game reads the newest sample.
   Many games read right after a refresh begins, a moment before the new sample lands, so the
   sample they get is already a whole refresh (16.7 ms) old.
2. **Simulate and render.** The CPU runs the game logic and prepares the frame. The overlay
   shows this as CPU time.
3. **Submit.** The game hands the finished frame to the display queue.
4. **Queue.** The frame waits for its turn. The Vita's usual queue holds two frames, which
   adds 16 to 33 ms.
5. **Flip.** At the next vblank the frame is shown, and the panel draws it top to bottom.

A typical game at a 30 limit shows input that is 25 to 55 ms old by the time the panel starts
drawing it. The overlay's INPUT row measures this from the sample to the flip.

Antilag works on step 1, Lowlat on step 4 and Tearing on step 5.

### Games that never wait

Some games never make the vblank wait. They render as fast as they can and flip, or keep time
with a timer of their own. A limit that changes the wait does nothing to such a game, so
fpsvita can add a wait after each frame instead: that is [Vsync](#vsync).

## The quick menu

Hold the PS button to open the quick menu. fpsvita adds three rows:

```
[icon] FPS:  < 30fps (og:60) (!) >           Limiter:  < off >
       HUD:  < Full >                         Corner:  < BL >
   Latency:  [x] Lowlat  [x] Antilag        Vsync [x]   [Save]
```

| Control | Values | Takes effect |
|---|---|---|
| FPS | Default, 60fps, 30fps, 20fps, Uncapped, Tearing | At once |
| `(!)` | Shown when the [compatibility list](compatibility-list.md#probably-incompatible) names a problem for this game at the selected limit. A limit equal to the game's own rate counts as safe. | |
| Limiter | off, 20, 24, 25, 30, 40, 45, 50, 60 | At once, only at Uncapped |
| HUD | Off, Mini, Full | At once |
| Corner | TL, TR, BL, BR (overlay position) | At once |
| Lowlat | Ticked or not | At the game's next launch |
| Antilag | Ticked or not | At once |
| Vsync | Ticked or not. Shown only when Vsync is forced in `config.txt` | At once, saved at once |
| Save | Shown only while a game runs | |

### With a game running

FPS, Limiter, Lowlat and Antilag change this game only. The changes last until the game
exits. Press **Save** to keep them: Save writes this game's profile, which loads at every
launch of the game.

Save stores the FPS row (including Tearing), Lowlat, Antilag and Limiter. It does not store
HUD, Corner or Vsync, which are global.

### With no game running

Every change is the global default and is saved at once. The Save button is hidden. Global
defaults apply to every game that has no profile of its own, and to any option a profile
leaves out.

HUD, Corner and Vsync are always global and always saved at once.

## Options

Each option below says what it does, when it helps, what it costs and when it does nothing.
The measured numbers come from [latency measurements](latency-measurements.md).

### Frame limits

**What it does.** `60fps`, `30fps` and `20fps` make every wait last one, two or three
vblanks. `Uncapped` makes waits return at once. `Default` leaves the game alone, and
`Tearing` is Uncapped with immediate flips (see [Tearing](#tearing)).

**When it helps.** A game that cannot hold its own rate can run steadily at a lower limit,
and a game with spare power can run faster. Both only if its speed does not follow the frame
rate. Setting the limit to a game's own rate (`30fps` for a game showing `og:30`) does not
change its speed, and lets full [Antilag](#antilag) do more.

**What it costs.** A speed-tied game runs too fast or too slow
([why](#why-some-games-break-at-another-frame-rate)). A limit the game cannot reach drops
frames instead; the overlay's TIP row then says `30 NOT HELD, TRY 20`.

**When it does nothing.** A game that keeps time without the vblank wait ignores the limit
(Persona 4 Golden stays at 30 under a 60 limit). Vsync can add a wait for games that never
wait.

### Vsync

**What it does.** Adds a wait of the limit's length after each frame, for games that never
wait on their own.

- `auto` (the default): on when the game has not waited for 30 frames and runs at least 10%
  faster than the limit; off again after 60 frames in a row that follow a wait of its own.
- `vsync=1`: always on at a 60, 30 or 20 limit. `vsync=0`: never.

**When it helps.** Games that run uncapped by design and need a limit, such as Dragon Quest
Builders, Minecraft and The Amazing Spider-Man at 30 (see the
[compatibility list](compatibility-list.md#from-patch-lists)).

**What it costs.** In auto, nothing for a game that paces itself. Forced on, a game that
already waits gets a second wait and can fall to a lower rate.

**When it does nothing.** At Default and Uncapped, where the checkbox is greyed out. The
checkbox only appears when `vsync=0` or `vsync=1` is set; Mini shows `VSYNC` while the wait is
active.

### Antilag

Antilag has two forms: **auto** (the default) is the fresh poll only; **full** adds a sleep
at 30 and 20 limits. The checkbox is ticked for either. Unticking turns Antilag off, and
ticking turns on full. Auto comes back only through `antilag=auto` in
[config.txt](#configtxt) or the game's [state.txt](#statetxt) line.

**What it does.**

- *Fresh poll.* When the sample the game just read is nearly a refresh old, the next one
  lands within about 2 ms. fpsvita reads again for up to 2.5 ms and hands the game the new
  sample.
- *Sleep (full only).* At a 30 or 20 limit, after the game submits a frame, fpsvita holds the
  game's thread for whole refreshes before it continues. The game's next read then happens
  later, and the fresh poll catches a newer sample. The sleep only spends time the frame would
  otherwise wait in the queue, so the flip does not move. It only runs on games that read
  input right after submitting. A frame that misses its slot turns the sleep off for 60
  frames, after which it learns again.

**When it helps.** Games that read input right after a refresh begins. The fresh poll gained
5 to 14 ms where it applies, 11 to 14 ms on most retail titles tested. Full Antilag gained
17 to 24 ms on Metal Gear Solid 2 HD at 30.

**What it costs.** The fresh poll spins 0.2 to 0.5 ms per read. A game that reads twice per
frame pays about 3 ms of CPU per frame (Galaxy on Fire 2, no frames lost). The full sleep can
cost an occasional missed frame, which switches it off for 60 frames; the overlay's ANTI-LAG
count climbs when that keeps happening, which means the game cannot afford the sleep.

**When it does nothing.**

- A game that reads input mid-frame (The Simpsons: Hit & Run), or one that already gets fresh
  samples.
- The sleep: at Default, 60fps and Uncapped, while Vsync's wait is active, and when the frame
  has no whole refresh to spare.
- A game that reads several samples at once with a blocking read skips the fresh poll, unless
  its profile has [`read=peek`](#statetxt).

### Lowlat

**What it does.** Makes the game's display queue one frame deep instead of two, at the moment
the game starts its graphics.

**When it helps.** When frames sit in the queue: the overlay's QUEUE close to the frame time,
and TIP says `TRY LOW LAT`. Dead or Alive 5 Plus at 60 went from 38 to 20 ms with Antilag on.

**What it costs.** Less slack for a slow frame, so a game near its limit can lose frames.
Watch FPS and the LAST row.

**When it does nothing.**

- A game that already uses a one-frame queue. The overlay shows `ON (GAME'S OWN)`.
- A game that sets up its graphics outside the call fpsvita hooks, such as Doom64EX. The
  overlay keeps showing `RESTART GAME`.
- Where frames do not wait in the queue: on Metal Gear Solid 2 HD and The Wolf Among Us INPUT
  barely moved.

**Applying it.** The change takes effect at the game's next launch. Tick Lowlat, press Save,
then quit and restart the game. Without Save, the relaunch uses the old setting and the LAST
row reads `LOW LAT: NOT SAVED`.

### Limiter

**What it does.** At the Uncapped limit, holds the game to a fixed rate with a timer: 20, 24,
25, 30, 40, 45, 50 or 60 fps (any value from 10 to 120 through `config.txt`). The average
rate is exact.

**When it helps.**

- Rates between the vblank steps, such as 40 or 45 for a game that cannot hold 60 but runs
  well above 30.
- Games that a normal limit cannot hold at all, because they never wait and Vsync does not
  catch them: set FPS to Uncapped and the Limiter to the rate you want (Doom64EX at 30 was the
  test case). The game's input read is then not tied to the vblank, which on some games
  lowers latency too.

The speed caveat applies as for any limit ([why](#why-some-games-break-at-another-frame-rate)).

**What it costs.** The timer is not tied to the refresh. At 40 fps, frames stay on screen for
one or two refreshes in turn, so motion is less even than at 30 or 60. With Tearing they show
at once, with seams.

**When it does nothing.** At any limit other than Uncapped (the overlay shows
`40 FPS (NEEDS UNCAPPED)`), and when the game cannot reach the rate.

### Tearing

**What it does.** Uncapped, plus every flip takes effect at once instead of at the next
vblank. The newest frame reaches the screen without waiting.

**When it helps.** Games running above 60 fps, or at Limiter rates, where waiting for the
vblank delays each frame. On Doom64EX's intro, the game's wait for a frame to be shown
completed 563 times a second with Tearing, against 43 with normal flips.

**What it costs.** Visible seams. The speed caveat of Uncapped applies.

**When it does nothing.** In [lite mode](#lite-mode), which has no kernel module. When the
immediate flips fail, fpsvita falls back to normal flips and the overlay shows
`TEARING (NO KERNEL)`.

### Panel refresh (experimental)

**What it does.** Runs the screen at a lower refresh rate, from 24 to 59 Hz, while one game
runs. Set it per game with `,panel=<hz>` on the game's [state.txt](#statetxt) line; there is
no quick-menu control. The limits then count in the new refreshes: at `panel=40` the `60fps`
limit is an even 40 fps, one frame per refresh. The normal 59.94 Hz returns when the game
exits or another one starts.

**When it helps.** A game that cannot hold 60 but runs well above 30, where the Limiter's
uneven frame times bother you. Unlike the Limiter, every frame stays on screen for exactly one
refresh. It suits best:

- Games that keep their own time and run somewhere between 35 and 55 fps with headroom to
  spare. Absolute Drift at 40 Hz held a steady 40 fps with 12 ms of CPU per frame and looked
  very smooth.
- Slower-paced games (exploration, adventure, driving, third-person action) where smooth
  motion matters more than the last few milliseconds of input.

**Pick a rate the game holds in its heaviest scenes**, then check the overlay: FPS should read
the panel rate, not just under it. A game that misses a refresh holds that frame for two, and
at 40 Hz that alternates 25 and 50 ms frames, which looks choppier than a steady 30. Doom64EX
at 40 Hz averaged 37 to 38 fps and looked worse than 30. If FPS sits under the panel rate,
lower `panel=` a few Hz.

It suits poorly:

- Speed-tied games: a lower refresh is permanent slow motion.
- Fast, timing-critical games (fighting, rhythm, competitive shooters): input latency rises.
- Games that already hold 60, or that hold 30 by themselves (at Default a game that waits two
  refreshes gets half the panel rate, 20 fps at 40 Hz).

**What it costs.** The controller is sampled once per refresh, so input latency rises:
Doom64EX at 40 Hz measured about 61 ms from input to screen, against about 34 ms at 60.

**Limits.** Tested on the OLED Vita (PCH-1000), firmware 3.65, including sleep and resume
while a game runs at a panel rate. The Slim (PCH-2000) drives its LCD with the same timing, so
fpsvita applies it there too, but it is untested: if the screen flickers or goes black, quit
the game (the normal rate returns) or reboot, and please [report](#reporting-a-game) what you
saw. A PS TV has no panel, and fpsvita leaves any display it does not recognise alone. Lite
mode cannot use it (no kernel module). The overlay shows `40HZ` (Mini) or `@40HZ` on the LIMIT row (Full) when it is in force,
and `helper.log` logs `helper: panel want=40 got=40` at launch; `got=0` means it was refused.

## What to try first

1. Set HUD to `Full` and play. Note INPUT and watch the TIP row.
2. Leave Antilag on auto. If TIP says `TRY FULL ANTI-LAG`, untick and tick Antilag to turn on
   full Antilag. For a game whose own rate is 30 or 20 (`og:30`), set the FPS row to that
   rate first: speed stays the same, and the full sleep can work.
3. If TIP says `TRY LOW LAT`, tick Lowlat, Save and relaunch the game.
4. Read LAST after each change. Keep what helped and press Save.
5. Try a frame limit only after checking the [compatibility list](compatibility-list.md), and
   judge the game's speed by eye.
6. Treat Limiter and Tearing as experiments.

## Overlay reference

The overlay appears over a running game when HUD is `Mini` or `Full`, and hides when no game
runs. Its numbers are smoothed averages, updated about four times a second.

### Mini

Two lines:

```
30 FPS  LIMIT 30 / OG:60
LL  AL
```

The first line is the frame rate and the limit: `LIMIT 30`, `DEFAULT`, `UNCAPPED`, `TEARING`
or `TEARING (NO KERNEL)`, followed by `/ OG:60` once the game's own rate is known.

The second line lists what is active:

| Shown | Meaning |
|---|---|
| `VSYNC` | Vsync's wait is active |
| `LL` | A one-frame queue is in effect, from Lowlat or from the game itself |
| `LL?` | Lowlat is ticked and applies at the next launch |
| `AL` | Antilag auto |
| `AL+` | Full Antilag |
| `LIM 40` | The Limiter is holding 40 fps |
| `40HZ` | [Panel refresh](#panel-refresh-experimental) at 40 Hz |

### Full

| Row | Shows | Examples |
|---|---|---|
| FPS | Frames per second / time per frame | `30.0 / 33.3 ms` |
| LIMIT | The limit / the game's own rate | `30 / OG:60`, `30 FPS`, `DEFAULT / OG:60`, `UNCAPPED`, `TEARING`, `TEARING (NO KERNEL)`; with panel refresh `40 @40HZ` |
| CPU | CPU time per frame, without time spent waiting for the queue | `11.1 ms` |
| INPUT | Age of the newest controller sample the frame had, at that frame's flip | `22.1 ms` |
| QUEUE | Time from submitting the frame to its flip | `8.7 ms` |
| ANTI-LAG | Antilag state | see below |
| LOW LAT | Lowlat state | see below |
| LIMITER | `40 FPS`, `40 FPS (NEEDS UNCAPPED)` or `OFF` | |
| TIP | Advice from the readings | see [TIP](#tip) |
| LAST | Your last change, before and after | see [LAST](#last) |
| TITLE | The game's title ID | `PCSE00078` |

A row shows `-` until it has a measurement. CPU stays `-` on a game that does not submit
frames through the call fpsvita hooks. INPUT does not include the panel's own response time.

ANTI-LAG reads `OFF`, `AUTO`, `ON / 0` (full, not sleeping now) or `16.6 ms / 0` (full,
sleeping that long each frame). The number counts frames that missed their slot while
sleeping.

LOW LAT reads `ON`, `OFF`, `ON (GAME'S OWN)` (the game uses a one-frame queue itself),
`RESTART GAME` (ticked, applies at the next launch) or `ON UNTIL RESTART` (unticked, the queue
stays short until the game restarts).

### TIP

TIP suggests one thing to try. It shows `...` for 3 seconds after a launch or a change, while
the averages settle. A new tip must hold for 5 seconds before it replaces the one shown. The
first rule that matches wins:

| Order | Tip | When |
|---|---|---|
| 1 | `30 NOT HELD, TRY 20` | The game runs under 90% of the limit. At 20 it reads `20 NOT HELD`. |
| 2 | `TRY FULL ANTI-LAG` | Antilag is off and the sample is 10 ms old or more, or full Antilag is off at a 30 or 20 limit with a refresh of queue time and room for the sleep. |
| 3 | `TRY LOW LAT` | Lowlat is off and the frame spends 80% of its time in the queue. |
| 4 | A known problem | The [compatibility list](compatibility-list.md#probably-incompatible) names one for this game at this limit, and the limit is not the game's own rate. |
| 5 | `NONE` | Nothing to suggest. |

The known problems read: `NO INPUT ABOVE 59`, `FREEZES, USE DEFAULT`, `SHIP SHAKES WITH CAP`,
`VIDEOS BREAK AT 60`, `MENUS RUN FAST`, `SLOW MOTION AT 20` and `SPEED TIED TO FPS`.

### LAST

LAST compares INPUT before and after your last change to the limit, Lowlat, Antilag, the
Limiter or Tearing. It is how you find out whether a change helped on your game.

| Shown | Meaning |
|---|---|
| `-` | No change yet |
| `ANTI-LAG ...` | Changed, waiting 3 seconds for the averages to settle |
| `ANTI-LAG 38>27 MS` | INPUT went from 38 to 27 ms |
| `LIMIT 30>22 -3FPS` | INPUT before and after, and the frame rate fell by 3 fps. Shown instead of `MS` when it fell by more than 5% |
| `LIMIT 30>60 FPS` | No INPUT measurement, so the frame rate before and after |
| `LOW LAT: RESTART` | Lowlat changed; relaunch the game to measure it |
| `LOW LAT: NOT SAVED` | The game relaunched without the change, because Save was not pressed |

The names are `LIMIT`, `LOW LAT`, `ANTI-LAG`, `LIMITER` and `TEARING`.

A change counts only while the game presents frames before and after it, and changes in the
first second after a launch (when the profile loads) are ignored.

## Troubleshooting

**No fpsvita rows in the quick menu.**
The quick-menu plugin quits when the `.rco` file or the kernel module is missing. Check
`ur0:data/fpsvita/fpsvita_plugin.rco`, the `*KERNEL` line, and that you edited the taiHEN
config taiHEN actually reads (`ux0:tai/config.txt` wins when it exists).
`ur0:data/fpsvita/boot.log` names what was missing.

**The Vita's shell fails at boot.**
See [recovery](../README.md#conflicts-and-recovery) in the README.

**The game runs too fast or in slow motion.**
Its speed follows the frame rate. Set FPS to `Default` and press Save. Report it so it can go
on the compatibility list.

**A limit has no effect.**
The game keeps time without the vblank wait. In auto, Vsync only steps in when the game runs
at least 10% faster than the limit. Try `vsync=1` in `config.txt`, or Uncapped with the
[Limiter](#limiter). Some games pace themselves in ways nothing outside the game can change.

**FPS stays below the limit, TIP says `NOT HELD`.**
The game cannot render that fast. Use the lower limit TIP suggests, or `Default`.

**Stutter after turning on full Antilag.**
The game's frame times are spiky. The sleep backs off by itself after a missed frame; if the
stutter stays, untick Antilag for that game (or set `antilag=auto` on its line) and Save.

**Frames drop after turning on Lowlat.**
The game needed the second queue slot. Turn Lowlat off, or use a lower limit.

**LOW LAT still shows `RESTART GAME` after a restart.**
If LAST reads `LOW LAT: NOT SAVED`, press Save and relaunch. Otherwise the game sets up its
graphics in a way Lowlat cannot reach.

**No input at 60fps or Uncapped (Killzone: Mercenary).**
Add `,read=peek` to the game's line in [state.txt](#statetxt).

**The game freezes at 60 (Jak II).**
Use `Default` for that game.

**The overlay does not appear.**
HUD is `Off`, or no game is running.

**A change to `config.txt` was lost.**
The quick-menu plugin reads `config.txt` only at boot and rewrites the whole file whenever you
change a global setting. Reboot after editing it.

**Save does nothing.**
`state.txt` is over 4 KB. Save refuses to write a file it cannot read whole, so no profile is
ever cut off. Remove lines for games you no longer play.

## Settings files

All files live in `ur0:data/fpsvita/`. Edit them with VitaShell's text editor.

| File | Written by | Read by | An edit takes effect |
|---|---|---|---|
| `config.txt` | The quick-menu plugin: at first boot, and whole on every global change | The quick-menu plugin at boot, the helper at every game launch | After a reboot |
| `state.txt` | Save: replaces or adds this game's line, keeps every other line as it is | The helper at every game launch | At that game's next launch |
| `helper.log` | The helper, inside each game | You, and `tools/ingest_playlog.py` | |
| `boot.log` | The quick-menu plugin | You | |

### config.txt

The global settings, one `key=value` per line. A fresh install writes every key with its
default:

| Key | Values | Default |
|---|---|---|
| `limit` | `-1` Default, `1` 60fps, `2` 30fps, `3` 20fps, `0` Uncapped | `-1` |
| `vsync` | `auto`, `1` forced on, `0` forced off | `auto` |
| `hud` | `0` Off, `1` Mini, `2` Full | `0` |
| `hudpos` | `0` TL, `1` TR, `2` BL, `3` BR | `0` |
| `lowlat` | `0`, `1` | `0` |
| `antilag` | `auto`, `1` full, `0` off | `auto` |
| `limiter` | `0` off, or 10 to 120 fps | `0` |
| `tearing` | `0`, `1` (with `limit=0`) | `0` |
| `log` | `0`, `1`: write rate lines to `helper.log` (see [collecting play logs](#collecting-play-logs)) | `0` |

### state.txt

One line per game, written by Save:

```
TITLEID=<limit>,lowlat=<0|1>,antilag=<0|1|auto>,limiter=<fps>,tearing=<0|1>
```

For example, Metal Gear Solid 2 HD at its own 30 with full Antilag:

```
PCSE00078=2,lowlat=0,antilag=1,limiter=0,tearing=0
```

The limit uses the same numbers as `limit=` in `config.txt`. A key missing from the line
falls back to the global setting.

`read=peek` has no quick-menu control. Add it by hand to the end of a game's line. It turns
the game's blocking controller reads into non-blocking ones, which Killzone: Mercenary needs
above 59 fps. Save keeps it.

```
PCSA00107=1,lowlat=0,antilag=auto,limiter=0,tearing=0,read=peek
```

`panel=<hz>` (24 to 59) is also added by hand and kept by Save; see
[Panel refresh](#panel-refresh-experimental).

`state.txt` can hold up to 4 KB.

## Lite mode

`fpsvita-helper-lite.suprx` is the helper alone: no kernel module, quick menu, overlay,
Tearing or panel refresh. Copy it to `ur0:tai/` and add it under `*ALL`, without the other
fpsvita lines or the full helper. Settings come from the same two files, written by hand in
`ur0:data/fpsvita/`; it reads `limit`, `vsync`, `lowlat`, `antilag`, `limiter` and `log`, plus
each game's line, at every launch, so an edit needs no reboot. With no files it uses the
defaults.

## Reporting a game

Open one issue per game at
[github.com/esp1esp1/fpsvita/issues](https://github.com/esp1esp1/fpsvita/issues). A game that
works at a limit is as useful as one that breaks. Include:

- The game's name and title ID (the overlay's TITLE row).
- Your Vita model and firmware, and other plugins you run, especially frame-rate patches and
  overclocking.
- Your settings: the FPS and Latency rows, or the game's `state.txt` line.
- What you saw. Say whether the game's **speed looked right by eye**: an FPS reading cannot
  show a speed-tied game.
- The overlay's FPS, CPU, INPUT, QUEUE, TIP and LAST readings. A photo works.
- `ur0:data/fpsvita/helper.log`, and `boot.log` for quick-menu or boot problems.

A report enters the [compatibility list](compatibility-list.md) with `user` confidence.

### Collecting play logs

With `log=1`, the helper adds a rate line to `helper.log` every 3000 frames, the first at
frame 900. Each line holds the frame rate, frame times and latency figures under the
settings in force. To collect them:

1. Change `log=0` to `log=1` in `config.txt` and reboot.
2. Play normally.
3. In VitaShell, press Select to start its FTP server.
4. On a computer with a copy of the fpsvita source, run:

   ```
   python3 -I tools/ingest_playlog.py --ip VITA_IP
   ```

   `VITA_IP` is the address VitaShell shows. The script appends one row per rate line to
   `play.tsv` and deletes the log on the Vita (`--keep` leaves it; `--out` picks another
   file).
5. Attach the TSV file to your issue.
6. Set `log=0` and reboot when you are done. Each rate line is a few-millisecond write to the
   memory card from the game's display thread.

`helper.log` also gets a few lines at every game launch and grows until something deletes it.
Delete it whenever you like.
