# Latency measurements

For readers who want the evidence behind fpsvita's latency options and the reasons they work
the way they do. It covers how input latency was measured, how the Vita samples the
controller, the results per option, the results that came out negative, and ideas that were
examined and not built. For what each option does in practice, see the
[user guide](GUIDE.md#options).

## Method

All numbers come from one retail Vita (enso, firmware 3.65) in October 2026.

Each option was tested by playing the same part of a game twice, option off and then on, and
comparing the first full measurement window of each run. Frame rates were unchanged in every
pair.

**Input to screen** is the age of the newest controller sample the game had when it submitted
a frame, measured when that frame flips. It is the overlay's INPUT row. It does not include
the panel's own response time.

## How the Vita samples the controller

The controller is sampled once per refresh, every 16.7 ms. A new sample becomes visible 0.5
to 1.5 ms after the refresh begins.

Most games read the controller right after they submit a frame, which is right after a
refresh begins. They read just before the new sample lands, so they get the previous one.
The Wolf Among Us reads 0.2 ms after the refresh and gets a sample 16.2 ms old, every frame.

## Where a frame's latency comes from

Follow one frame of a typical game at a 30 limit:

| Step | Adds |
|---|---|
| The sample is already old when read | up to one refresh, 16.7 ms |
| Simulate and render | the game's CPU time |
| Wait in the display queue (two frames deep by default) | 16 to 33 ms |
| Flip at the next vblank | until that vblank |

Together, a typical game at a 30 limit shows input 25 to 55 ms old before the panel starts
drawing it. The first and third rows are where fpsvita can act from outside the game.

## Antilag

Antilag (input to screen, off then on):

| Title (limit) | Off | On | What did it |
|---|---|---|---|
| Metal Gear Solid 2 HD (30), menu | 40 ms | 16 ms | Fresh poll, and the sleep on most frames |
| Metal Gear Solid 2 HD (30), in-engine cutscene | 32 ms | 16 ms | Same |
| Oddworld: Stranger's Wrath HD (30) | 23 ms | 9 ms | Fresh poll; the sleep correctly never started (short queue) |
| The Wolf Among Us (30) | 56 ms | 42 ms | Fresh poll; no room for the sleep at 18 fps |
| The Swapper (30) | 49 ms | 35 ms | Fresh poll; the game is GPU bound |
| Dead or Alive 5 Plus (60), menu | 49 ms | 38 ms | Fresh poll |
| Dead or Alive 5 Plus (60), gameplay | 35 ms | 33 ms | The game already reads a fresh sample here |
| Galaxy on Fire 2 (own rate) | 21 ms | 16 ms | Fresh poll, two reads a frame |
| Doom64EX (own rate), intro | 34 ms | 27 ms | Fresh poll |
| The Simpsons: Hit & Run (30) | 32 ms | 32 ms | Nothing: the game reads mid-frame, no sample is about to land |
| Grim Fandango Remastered (30) | | | No clean pair: the two runs did not cover the same content |

### Fresh poll

A game that reads just before a new sample lands is a refresh behind for want of about a
millisecond. So when the sample a game just read is nearly a refresh old, fpsvita reads again
for up to 2.5 ms and hands over the new sample as soon as it lands.

The check is on the sample's age, not on timing, so it needs no knowledge of the game's loop
and fires at most once per sample. It gained 5 to 14 ms where it applies, 11 to 14 ms on most
retail titles.

The cost was small. The spin took 0.2 to 0.5 ms and never timed out. Galaxy on Fire 2, which
reads twice per frame, paid about 3 ms of CPU per frame and lost no frames.

### Whole-refresh sleep

At a 30 limit, a frame often waits in the queue for a refresh or more. If the game's thread
sleeps that time away right after submitting, its next input read happens later and the
fresh poll catches a newer sample, while the flip stays where it was. Fresh poll plus sleep
on Metal Gear Solid 2 HD at 30 gained 17 to 24 ms.

The design follows from three findings:

- **Whole refreshes only, never at 60.** Samples land just after each refresh. A sleep sized
  to most of the frame's spare time, rather than whole refreshes, moved the read away from
  that landing and made input older at a 60 limit. At 60 there is no whole refresh to spare.
- **Only queue time is spent.** The sleep is limited to the time the frame would have waited
  in the queue, measured while not sleeping. Metal Gear Solid 2 HD at 30 queues 27 ms, so a
  refresh of sleep moves only the read. Oddworld queues 7 ms, which leaves no whole refresh.
- **A guard backs it off.** Without a back-off guard, the sleep pushed Oddworld's frames a
  refresh later, which made latency worse. With it, flips arriving later than before the
  sleep turn the sleep off and it learns again. A missed frame does the same.

The sleep also runs only on games whose controller reads cluster right after they submit a
frame. That is the only loop shape where a later read is a fresher read.

## Lowlat

| Title | Input to screen |
|---|---|
| Dead or Alive 5 Plus (60), menu | 38 ms with Antilag, 20 ms with Antilag and Lowlat |
| Dead or Alive 5 Plus (60), gameplay | 33 ms with neither, 21 ms with Antilag and Lowlat |
| Metal Gear Solid 2 HD (60), menu | 26 ms with or without Lowlat |
| The Wolf Among Us (30) | 11 ms with Antilag, 8 ms with both; within the noise |

The Metal Gear Solid 2 HD and The Wolf Among Us rows used an older measure that aged the
sample at the next flip instead of at that frame's own flip. It understates a two-deep queue
by up to a frame, so these two rows compare with each other but not with other INPUT figures.

A two-deep queue is a frame of latency kept as insurance against slow frames. Lowlat gives
that frame back where frames actually wait in the queue (Dead or Alive 5 Plus gained 18 ms in
its menu and about 12 ms in gameplay, where Antilag alone gives 2)
and does little where they do not. The queue depth is fixed when the game starts its
graphics, so Lowlat applies at the next launch. Doom64EX starts its graphics outside the call
fpsvita hooks, so Lowlat cannot apply there.

## Tearing

The display call available to games refuses an immediate flip on every title tested (error
INVALID_UPDATETIMING), and the screen stops updating. The kernel's display call accepts it.
So Tearing sends each flip through fpsvita's kernel module, which allows it only for the
running game, and falls back to a normal flip when that fails.

Timing the game's wait for each frame to be shown, Doom64EX's intro ran it 43 times a second
with normal flips and 563 times a second with kernel flips. Seams were visible on a
slow-motion phone recording.

## Limiter

The Limiter holds the game to a fixed schedule: each slot is one period after the previous
slot, so the average rate is exact. A frame late by less than a period shortens the next
slot so the schedule catches up. A later frame restarts the schedule rather than letting
frames burst. The game's own timing still reads real time, so nothing drifts. It was tested on
Doom64EX at 30.

## Vsync auto

Auto mode must not switch its wait on and off every few frames. A plain pair of thresholds
does exactly that: once the wait engages, the game's next own wait resets the count and
releases it again. So engaging needs 30 frames without a wait, while releasing needs 60
paced frames in a row, which a game that waits only now and then never reaches. A game that
really resumes waiting reaches it in about a second.

Engaging also needs the game to present at least 10% faster than the limit. A game already
held at or below the limit by a wait fpsvita cannot see, or by its GPU, gains nothing from a
second wait and would only be slowed to the next step.

## Negative and inconclusive results

- **The Simpsons: Hit & Run**: no gain. It reads input mid-frame, when no sample is about to
  land.
- **Dead or Alive 5 Plus gameplay**: 35 to 33 ms. The game already reads a fresh sample.
- **Grim Fandango Remastered**: no clean pair, because the two runs did not cover the same
  content.
- **Metal Gear Solid 2 HD at 60**: Lowlat changed nothing (26 ms either way, older measure).
- **The Wolf Among Us**: Lowlat within the noise (older measure).

## Panel refresh (experimental)

Lowering the OLED's refresh by stretching its vertical timing measured 50.000, 45.10 and
40.000 Hz, with no visible artefacts anywhere from 24 to 50 Hz. The controller follows the
refresh: a blocking read returned 60.3 samples a second at 60 Hz and 40.1 at 40 Hz.

- **Doom64EX at 40 Hz, Default limit**: a steady 40.0 fps in light scenes, 37 to 38 in heavy
  ones. Input to screen rose to about 61 ms, against about 34 ms at 60 Hz: fewer samples, and
  the game queues about two frames, which Lowlat cannot reach on this title.
- **Uncharted: Golden Abyss at a 60 limit**: 35 to 36 fps at 60 Hz with 40% of vblanks
  missed, so frame times alternated. At a 36 Hz panel it showed one frame per refresh.
- **Absolute Drift at 40 Hz, 60 limit**: a steady 40 fps in gameplay with 12 ms of CPU per
  frame (14 ms worst), 5 missed refreshes in 600 frames, about 47 ms input to screen. It
  looked very smooth.
- **Doom64EX, by eye**: at 37 to 38 fps on a 40 Hz panel its frames alternated between one
  and two refreshes, and it looked choppier than a steady 30. A panel rate the game cannot
  hold is worse than none.

- **Slim (PCH-2000), two user reports**: Minecraft: PlayStation Vita Edition and Dragon Quest
  Builders at `panel=40` with a 60 limit, firmware 3.65. The kernel granted 40 Hz in both,
  with no artefacts and the games at normal speed. Both ran under 40 fps most of the time
  (Minecraft 34 to 39, Dragon Quest Builders 19 to 39), so 40 Hz made them play worse.
  Whether the Slim's screen itself refreshed at 40 Hz is not confirmed: one Minecraft sample
  at the 60 limit read 57 frames a second, which a 40 Hz screen cannot show. The reports'
  input figures predate a measurement fix (v0.3.1) for these two games and are not used.

So panel refresh is for games that cannot hold 60 but beat 30 and keep their own time; it
trades latency for even frame pacing.

Sleep: resuming while the timing was stretched latched part of the display setup. The game
was fine, but after it exited the home screen's top layers flickered half-drawn until the
next sleep or a reboot. fpsvita now restores the stock timing before sleep and re-applies the
rate after resume; with that, the rate came back at 40 Hz and the home screen stayed clean.

## Ideas examined and not built

- **Adaptive vsync** (flip without waiting when a frame is late): few frames miss by a small
  margin. On hold.
- **The firmware's own 48 and 50 Hz output modes**: switching mode reached about 47.8 Hz
  only after a sleep and resume, and one mode blanked the panel until a reboot. The timing
  patch above replaced this.
- **Kernel scanline sync** and **a fresher button read from the system controller chip**:
  not measured.
- **A time-based limiter locked to the refresh phase**: the Limiter keeps an exact average
  rate but is not phase-locked.

## Further reading

- The [henkaku wiki](https://wiki.henkaku.xyz/) pages on SceCtrl, SceDisplay and SceSyscon.
- PC latency work that shaped the options: NVIDIA Reflex, AMD Anti-Lag 2, Intel XeLL,
  LatencyFleX, RTSS, Special K, RetroArch's frame delay and Android Swappy.
- [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) lists every project whose source
  informed the design.
