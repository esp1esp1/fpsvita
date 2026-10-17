# Latency measurements

The numbers behind fpsvita's latency options, measured on one retail PS Vita (ensō, firmware
3.65) in October 2026. This is the long answer to "does Antilag actually do anything" and
"why is Tearing a kernel feature".

## 1. What was measured

The helper (the plugin that runs inside each game) records, for every frame:

- **Input age at the read.** When the game asks for the controller state, how old is the newest
  sample it gets? The Vita stamps each sample with the time it was taken.
- **Input to screen.** The age of the newest controller sample the game had when it submitted
  a frame, measured at the moment that frame is flipped to the display. This is the whole
  chain except the panel's own response, and it is the INPUT row on the overlay.
- **Submit to flip.** How long a finished frame waits in the display queue before it is shown
  (the QUEUE row).
- Frame rate, CPU time per frame, and which display and controller calls the game makes.

Every option was tested by playing the same part of a game twice, with the option off and then
on, and comparing the first full measurement window. Titles: Metal Gear Solid 2 HD, The Wolf
Among Us, Oddworld: Stranger's Wrath HD, The Swapper, Dead or Alive 5 Plus, Grim Fandango
Remastered, Doom64EX, Galaxy on Fire 2 and Road Rage 2.

## 2. How the Vita samples the controller

- The system reads the controller once per screen refresh (every 16.7 ms). The new sample
  becomes visible to games 0.5 to 1.5 ms after the refresh begins.
- Most games read the controller right after they submit a frame, which happens right after
  a refresh begins. That is a hair *before* the new sample lands, so the game gets the previous
  one and plays on input that is a full refresh old. Measured on The Wolf Among Us: the read
  happens 0.2 ms after the refresh and the sample it gets is 16.2 ms old, every frame.
- With a two-deep display queue (the Vita default), a finished frame can wait a further 16 to
  33 ms before it is shown.

Put together, a typical game at a 30 fps limit shows input that is 25 to 55 ms old before the
panel even starts drawing it.

## 3. Antilag

Antilag does two things.

**Fresh poll.** When the game's controller read returns a sample that is nearly a refresh old,
the helper re-reads until the new one is in, 2.5 ms at most. In practice the wait is 0.2 to
0.5 ms and it never timed out. Where a game reads twice per frame (Galaxy on Fire 2) it pays
about 3 ms of CPU per frame; the game lost no frames because it was waiting for the screen
anyway.

**Whole-refresh sleep.** At a 30 or 20 fps limit a frame often sits in the display queue for a
whole refresh before it is shown. If so, the helper holds the game for that long *before* it
reads the controller, so the read lands on a newer sample and the frame still makes the same
refresh. The helper measures the queue time itself and only sleeps time that was idle; if
frames start arriving later it backs off for 60 frames and measures again. Without that guard
the sleep pushed Oddworld's frames a refresh later and made things worse, so the guard is not
optional.

Input to screen, same content, Antilag off and on. Frame rates were unchanged in every pair.

| Title (limit) | Off | On | What did it |
|---|---|---|---|
| Metal Gear Solid 2 HD (30), menu | 40 ms | 16 ms | fresh poll and the sleep on most frames |
| Metal Gear Solid 2 HD (30), in-engine cutscene | 32 ms | 16 ms | same |
| Oddworld: Stranger's Wrath HD (30) | 23 ms | 9 ms | fresh poll; the sleep correctly never started (short queue) |
| The Wolf Among Us (30) | 56 ms | 42 ms | fresh poll; no room for the sleep at 18 fps |
| The Swapper (30) | 49 ms | 35 ms | fresh poll; the game is GPU bound |
| Dead or Alive 5 Plus (60), menu | 49 ms | 38 ms | fresh poll |
| Dead or Alive 5 Plus (60), gameplay | 35 ms | 33 ms | the game already reads a fresh sample here |
| Galaxy on Fire 2 (own rate) | 21 ms | 16 ms | fresh poll, two reads a frame |
| Doom64EX (own rate), intro | 34 ms | 27 ms | fresh poll |
| Road Rage 2 (30) | 32 ms | 32 ms | nothing: the game reads mid-frame, no sample is about to land |
| Grim Fandango Remastered (30) | | | nothing, same reason |

An earlier version of Antilag slept for most of the frame's spare time instead. At a 60 fps limit
that moved the read *away* from the moment the new sample lands and made input older; that is
why the sleep is now whole refreshes only, and never at 60.

## 4. Low latency

Lowlat sets the display queue depth to one frame when the game starts up. It only helps a game
that keeps the second slot full:

| Title | Input to screen |
|---|---|
| Dead or Alive 5 Plus (60), menu | 38 ms with Antilag, 20 ms with Antilag and Lowlat |
| Metal Gear Solid 2 HD (60), menu | 26 ms with or without Lowlat |
| The Wolf Among Us (30) | 11 ms with Antilag, 8 ms with both; within the noise |

The overlay's QUEUE row tells you which case a game is: near a whole frame means Lowlat will
pay, a few milliseconds means it will not. Some homebrew ports set up graphics in a way the
helper cannot see (Doom64EX); the overlay reports Lowlat as pending there and it has no
effect.

## 5. Tearing

With vsync, a new frame is shown at the next refresh. "Vsync off" on PC shows it immediately,
mid-refresh, with a visible seam where the old and new frames meet, and that is the lowest
latency a display can give.

- The user-mode display call the games use refuses an immediate flip on every title tested
  (error `INVALID_UPDATETIMING`), and the screen stops updating.
- The kernel's display call accepts it. fpsvita's kernel module exposes that, and the helper
  routes every flip through it in Tearing mode. The flip really is immediate: with the
  "frame shown" wait timed right after each flip, Doom64EX's intro ran at 43 fps on normal
  flips (the wait blocks until the refresh) and 563 fps on kernel flips (it returns at once),
  and the seams are visible on a slow-motion phone recording.

Tearing mode is therefore Uncapped plus immediate flips, through the kernel module only, which
is why the lite plugin cannot offer it.

## 6. Things that were looked at and not built

- **Adaptive vsync** (skip the wait when the frame is already late): the data so far shows few
  frames missing by a small margin, so it is on hold.
- **50 Hz or 48 Hz panel modes** for an even 50 or 48 fps: the firmware lists such modes but
  nothing has been tried on hardware.
- **Scanline sync** from the kernel, and a **fresher button read** straight from the system
  controller chip: possible in principle, unmeasured.
- **A time-based limiter locked to the refresh** and self-correcting rates: the Limiter is
  still the simple version.

## Further reading

Projects whose source informed the design: Framecapper, VitaGrafix and VGi, PSVshell and
PSVshellPlus, divasplit, vitaGL, libvita2d, RetroArch's GXM driver and Vita3K for the Vita side;
NVIDIA Reflex, AMD Anti-Lag 2, Intel XeLL, LatencyFleX, RTSS, Special K, RetroArch's frame delay
and Android's Swappy for frame pacing and latency on PC and mobile; SaltyNX and FPSLocker
(Switch), GoldHEN (PS4) and gamescope for other console and compositor scenes. The henkaku wiki
pages on SceCtrl, SceSyscon and SceDisplay describe the hardware side.
