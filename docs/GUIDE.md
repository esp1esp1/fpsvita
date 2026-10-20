# fpsvita user guide: vsync, caps, latency options and the overlay

This is the long version of the README. It assumes nothing about how a console draws frames.

## 1. Vsync

The Vita's screen refreshes 60 times a second. Between two refreshes there is a short gap
called the **vblank**. A game that wants smooth, tear-free motion finishes drawing a frame,
hands it to the display, and then **waits for the next vblank** before starting the next one.
That wait is vsync. On the Vita it is a function call (`sceDisplayWaitVblankStart` and its
variants), and the game chooses how many vblanks to wait: one for 60 fps, two for 30 fps,
three for 20 fps.

Three things follow from this:

- **The wait sets the frame rate.** A game that waits two vblanks per frame runs at 30 fps no
  matter how fast it could draw. fpsvita works by changing that number.
- **Many games tie their speed to the wait.** Older engines and many ports advance the world by
  a fixed step per frame instead of by measured time. Change the wait from two vblanks to one
  and the game runs at 60 fps **and** twice as fast: enemies, music cues, timers, physics.
  Lower the wait instead and the same game plays in slow motion. On those games only Default is
  right. There is no way to tell from outside; the `(og:..)` value in the menu tells you what
  the game asked for, and the overlay tells you what it got. The
  [compatibility list](compatibility-list.md) has the games known to be fine at a given limit
  and the ones reported speed-tied. If a game feels wrong at a new limit, put it back.
- **Waiting is also where input latency hides.** The game reads the controller, simulates,
  draws, submits, then waits. Everything it read at the start of the frame reaches the screen
  one or more frames later. The latency options below all move the read closer to the screen.

### Games that do not wait

Some titles never call a vblank wait. They submit a frame and immediately start the next one,
relying on the display driver to hold them back when its queue is full. For those, changing the
wait count does nothing. fpsvita's **Vsync** option (inject) adds a wait of its own around the
present. With
auto-detect on (default) it engages when no wait has been seen for 30 presents **and** the game
is presenting at least 10 percent faster than the cap would allow, and releases after a long
run of paced presents. The second condition stops it from stacking a wait on a game that
something else is already pacing, for example another plugin's frame-rate patch.

## 2. Frame-rate limit

| Mode | What the helper does | Use it for |
|------|----------------------|------------|
| Default | Nothing. Every call chains straight through. | The default. Anything that works. |
| 60fps | Every display wait becomes a one-vblank wait. | Games that run at 30 by choice but draw fast enough for 60, and do not tie speed to the frame. |
| 30fps | Two vblanks. | A 60 fps game that drops frames unevenly and keeps its own time; a steady 30 feels better than 45 to 60. |
| 20fps | Three vblanks. | Heavy ports that cannot hold 30. |
| Uncapped | Display waits return at once. The panel still shows one complete frame per refresh, the newest one. | Measuring what the game can draw, and games with their own timers that benefit from more frames. Never for speed-tied games. |
| Tearing | Uncapped, and every flip takes effect immediately, mid-refresh. | The lowest latency the hardware allows, at the cost of visible seams. Needs the kernel module. |

"Default" rather than "Off" because on PC and the Steam Deck "limit off" means uncapped.

**Save** writes the whole row set for the running title into the state file as its profile;
the helper applies it at every later launch, and restores the globals when the game exits.

A capped wait is issued through the helper's own copy of the wait function rather than by
changing the count on the game's own call. On one homebrew port (Doom64EX) the latter returned
at once and removed all pacing; the former holds.

## 3. The latency options

These live on the quick menu's Latency row and in `ur0:data/fpsvita/config.txt` (defaults) or
on the title's line in `state.txt`. With a game running, a change applies to that title and is
kept by **Save**; with no game running it sets the global default. Anti-lag and the Limiter act
on the next frame. Lowlat is applied once when the game initialises graphics, so a change made
mid-game waits for the next launch, and the overlay says so: `RESTART GAME` (Mini: `LL?`)
after turning it on, `ON UNTIL RESTART` after turning it off. A game that already runs a
one-deep queue by itself shows `ON (GAME'S OWN)`; the option changes nothing there.

### Lowlat (`lowlat=1`): one frame less in the display queue

The Vita's graphics library keeps a queue of finished frames waiting to be shown. Sony's
samples and most games allow two in flight. That second slot is a full frame of latency
(16.7 ms at 60, 33 ms under a 30 limit) bought purely as tolerance for a slow frame. fpsvita
sets the queue depth to one when the game initialises graphics.

- **Gain:** up to one frame, on titles that actually keep the second slot full. Dead or Alive 5
  Plus at 60 gained 18 ms; on MGS2 HD and The Wolf Among Us the `INPUT` number barely moved.
- **Cost:** a frame that takes longer than usual now stalls the game instead of being absorbed
  by the queue, so a title that was just holding its rate may start dropping frames.
- **Not every title can take it.** Some homebrew ports initialise graphics in a way the helper
  cannot intercept; on those the overlay keeps saying `RESTART GAME` after a relaunch, and
  the option simply has no effect.

### Anti-lag (`antilag=auto` or `antilag=1`): just-in-time input

The controller is sampled once per vblank, and the new sample becomes visible 0.5 to 1.5 ms
after it. Most games poll input right after the previous frame was submitted, which is right
after the vblank: a hair before the new sample lands. They lose that race every frame and play
on input a whole frame old. Anti-lag does two things:

`auto` (the fresh-install default) enables only the fresh poll. `1` enables the fresh poll
and the whole-vblank sleep; the sleep is the part that can cost frames, so it stays explicit.
Under auto the quick-menu checkbox shows ticked: untick it for off, tick it again for full.
Getting back to auto means setting `antilag=auto` in `config.txt`, or on the title's line in
`state.txt`, by hand.

- **Fresh poll.** When a read returns a sample that is nearly a vblank old, the helper re-polls
  until the new one is in, 2.5 ms at most. Measured: 0.2 to 0.5 ms of spin, never a timeout,
  and 5 to 14 ms less input-to-screen (11 to 14 on most retail titles measured) on every
  title where the game polls that way.
- **Whole-vblank sleep.** Under a 30 or 20 limit, if the frame would otherwise sit in the
  display queue for a vblank or more, the helper holds the submitting thread that long first, so
  the poll lands on a later sample. It measures the queue time itself and only sleeps time that
  was idle anyway; if flips start landing later it backs off for 60 frames and relearns. On
  MGS2 HD at 30 the two together cut 17 to 24 ms. At a 60 limit it never sleeps.

- **Self-retreating:** a frame that misses its deadline while the sleep is on gates it for 60
  frames and restarts the warm-up. A game that cannot hold its limit gets no sleep at all.
- **Cost:** a sub-millisecond spin per controller read on games that poll on a stale sample;
  a vitaGL port that polls twice a frame pays about 3 ms, which it only notices if it is CPU
  bound. The sleep ends with 0.3 ms of busy-waiting for precision.
- **Does nothing for** games that poll mid-frame (The Simpsons: Hit & Run): no sample is ever about to land
  when they read.

### Limiter (`limiter=<fps>`): a time-based cap without vsync

In Uncapped mode the display waits return at once. The Limiter holds the submitting thread to
a fixed schedule: each slot is `1/fps` after the previous slot, so the average rate is the cap
exactly. A frame late by less than a period gets a shorter next slot and the schedule catches
up; a frame later than that restarts the schedule instead of bursting. The display shows
whatever is newest at each refresh.

- **Gain:** a stable rate, and the game's input read is not tied to the vblank, which on some
  titles is lower latency than the real limit.
- **Cost:** frames are not aligned to refreshes, so motion can judder slightly even at a
  steady 30. The schedule keeps the rate, not a phase lock to the refresh.
- **Use it for** titles that cannot be vblank-paced at all (Doom64EX at 30 was the test case)
  or when you want 40, 45 or 50 rather than 30 or 60. Only active in Uncapped (or Tearing) mode.

### Tearing (`tearing=1` with Uncapped)

Sony's user-mode display call refuses immediate flips; the kernel module's does not. In Tearing
mode every flip takes effect at once, so the panel starts showing the new frame mid-refresh
with a visible seam, as on a PC with vsync off. It is the only way to get a frame on screen
in less than a refresh. Pair it with the Limiter if the game's uncapped rate is wild.

### Which to try first

1. Limit the game to the rate it can actually hold. Uneven frame times are the biggest latency
   source and nothing below fixes them.
2. Anti-lag. `auto` is the fresh-install default (fresh poll only); `1` adds the
   whole-vblank sleep. Cheap, and on most games it is worth a frame of input latency outright.
3. Lowlat. Check the `INPUT` number before and after; on many titles it changes nothing.
4. The Limiter and Tearing are tools for games that cannot be limited the normal way, or for
   when latency matters more than a clean image.

## 4. Reading the overlay

Mini is two green lines without a box; Full is a box in PSVshellPlus's style with a label
and a value per row.

```
30 FPS  LIMIT 30 / OG:60            FPS:        29.9 / 33.4 ms
VSYNC  LL  AL+                      LIMIT:          30 / OG:60
                                    CPU:                 4.8 ms
                                    INPUT:              15.5 ms
                                    QUEUE:              11.8 ms
                                    ANTI-LAG:       16.7 ms / 2
                                    LOW LAT:                 ON
                                    LIMITER:                OFF
                                    TIP:                   NONE
                                    LAST:        ANTI-LAG 40>16 MS
                                    TITLE:            PCSE00078
```

| Item | Meaning |
|------|---------|
| `FPS` | Presents per second the game is achieving, and the frametime, averaged over the last second or so. Under a 30 limit it should read 29.9 to 30.0. A value well above the limit means the limit is not taking (no display wait to hook, vsync not engaged). |
| `LIMIT` | The mode in force for this title after its profile, then the rate the game asked for by itself (`30 / OG:60`: limited to 30, original 60). The original part is absent when the game never waited; on a title with another plugin's frame-rate patch it is that patch's number. `TEARING (NO KERNEL)` means the mode is set but the kernel module is not there to honour it. |
| Mini line 2 | What is active, in short form: `VSYNC` the helper adds its own vblank wait around the present (manual or auto-detected); `LL` Lowlat on, `LL?` requested for the next launch; `AL` Anti-lag auto, `AL+` full; `LIM 40` the Limiter. |
| `CPU` | Time the game spends per frame on its submitting thread: poll input, simulate, build the frame, hand it to the GPU. The idle time waiting for the display is excluded. Compare with the frame budget (16.7 ms at 60, 33.4 ms at 30). Shows `-` on the few titles where the submit call is not hooked. |
| `INPUT` | The number that matters: from the newest controller sample the game had when it submitted a frame to that frame's flip. Everything but the panel's own response. Anti-lag should lower it; compare with and without. |
| `QUEUE` | The same frame's submit-to-flip. Around one period means the display queue is holding a frame; that is the time anti-lag's sleep can reclaim and what Lowlat is meant to cut. |
| `ANTI-LAG` | `OFF`; `AUTO` when only the fresh poll is enabled; `ON / 2` when full Anti-lag is on but no sleep is in force; `16.7 ms / 2` when the whole-vblank sleep is on. The count is frames that ran over budget while the sleep was on; each one gates it for 60 frames, so a climbing count means the game cannot afford it. |
| `LOW LAT` | `ON`: the display queue was clamped to one frame at startup. `RESTART GAME`: requested, applies at the next launch. `ON UNTIL RESTART`: turned off, still clamped until the next launch. `ON (GAME'S OWN)`: the title runs a one-deep queue by itself. |
| `LIMITER` | Uncapped with a time-based limit active; `(NEEDS UNCAPPED)` when set under a vsync limit. |
| `TIP` | What the readings say is worth trying next, one suggestion at a time: `60 NOT HELD, TRY 30` (or `30 NOT HELD, TRY 20`, `20 NOT HELD`) when the game runs a tenth under its limit; `TRY FULL ANTI-LAG` when the game plays on a sample at least 10 ms old or, at a 30 or 20 limit, lets a frame sit a vblank in the queue while its CPU work leaves room for a vblank of sleep; `TRY LOW LAT` when the queue still holds most of a frame. When none of those applies and the [compatibility list](compatibility-list.md) names a problem for this title at the selected limit, the tip is that problem: `NO INPUT ABOVE 59` (60 or Uncapped, until the title has `read=peek`), `FREEZES, USE DEFAULT`, `SHIP SHAKES WITH CAP`, `VIDEOS BREAK AT 60`, `MENUS RUN FAST`, `SLOW MOTION AT 20` or `SPEED TIED TO FPS`; the quick menu marks the same case with `(!)` next to the FPS row. `NONE` when nothing measurable is left; `...` for three seconds after a launch or a change while the averages settle. A different tip has to hold for five seconds before it replaces the one shown, so a scene change does not flip it. No tip predicts a number: toggle it and read `LAST`. |
| `LAST` | Before and after of the last thing you toggled, once it has settled: `ANTI-LAG 40>16 MS` is `INPUT` before and after. When the change cost more than 5 percent of the frame rate the loss replaces the unit (`ANTI-LAG 40>16 -3FPS`). A Lowlat toggle shows `LOW LAT: RESTART` until the same title has been relaunched, and `LOW LAT: NOT SAVED` if it came back without the toggle (press Save before quitting; a toggle with a game running is not stored otherwise). |
| `TITLE` | The title id the numbers belong to. |

All values are exponential moving averages, so they take about a second to settle after a
scene change, and the whole overlay hides itself when no game is running.

## 5. Things that go wrong

- **Game runs at double speed at a higher limit.** Speed-tied engine. Use Default; Save it per
  game so it never happens again.
- **Game plays in slow motion at a lower limit.** The same thing the other way. Use Default.
- **Limit shows but fps reads far above it.** The title never waits and vsync has not engaged
  (fewer than 30 presents without a wait, or the game is not actually faster than the limit).
  Give it a few seconds; if it persists the title needs vsync forced: `vsync=1` in
  `config.txt`, or `vsync=0` to show the checkbox and tick it when needed. Forced vsync is a
  global setting; it is not stored in a title's profile.
- **Stutter after turning on anti-lag.** The game's frame time is spiky. The sleep should
  retreat on its own; if the stutter stays, the spikes are shorter than the retreat window.
  Turn anti-lag off for that title.
- **Frame drops after Lowlat.** The title was relying on the second queue slot. Turn it off,
  or limit lower.
- **Tearing mode looks like Uncapped.** The overlay says `TEARING (NO KERNEL)`: the lite helper
  or a missing kernel module. The full three-module install is required.
- **Two frame-rate plugins.** Framecapper and fpsvita must not run together. Frame-rate
  patches from other plugins coexist and show as the `(og:..)` value.
- **The quick menu row is missing after a boot.** Check `ur0:data/fpsvita/boot.log`; if SceShell
  crashed, hold L at power-on and comment the `fpsvita-ui` line out of `config.txt`.

## 6. The files

Both live in `ur0:data/fpsvita/`, are written by the quick menu and read by the helper when a
game starts. A hand edit to `state.txt` takes effect at that title's next launch. The quick
menu reads `config.txt` once at boot and rewrites the whole file on every change, so a hand
edit there needs a reboot, or the next menu change writes over it.

`config.txt`, the global defaults, regenerated whole on every change:

| Key | Values |
|-----|--------|
| `limit=` | -1 Default, 1 60fps, 2 30fps, 3 20fps, 0 Uncapped |
| `vsync=` | `auto` (default): forced vsync engages by itself on titles that never wait; `0` or `1`: forced off or on, and the Vsync checkbox is shown |
| `hud=` | Overlay: 0 Off, 1 Mini, 2 Full |
| `hudpos=` | Overlay corner: 0 TL, 1 TR, 2 BL, 3 BR |
| `lowlat=` | 0 or 1. Display queue depth forced to 1 at game start. |
| `antilag=` | `auto` (default on a fresh install): fresh poll only. `1`: fresh poll and the whole-vblank sleep at a 30/20 limit. `0`: off. |
| `limiter=` | fps, 0 off. Time-based cap in Uncapped mode. |
| `tearing=` | 0 or 1. With `limit=0`, every flip IMMEDIATE. |

`state.txt`, one line per title, written by Save:

```
PCSE00078=2,lowlat=0,antilag=auto,limiter=0,tearing=0
```

A plain `TITLEID=<limit>` line also works; the globals then apply to that title. A trailing
`,read=peek`, added by hand, turns the title's blocking `sceCtrlRead*` calls into the matching
`sceCtrlPeek*`, a fix for titles that lose input above their own rate; Save keeps it. Lines for
titles the quick menu does not know about are kept as they are.
