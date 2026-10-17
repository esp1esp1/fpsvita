# Latency measurements (2026-10-17)

How the options were chosen: a review of the frame-rate and latency techniques other projects
use, with the earlier design treated as open, followed by the measurements on hardware that
settled it.

A second look at every technique fpsvita implements or ruled out, against other projects' source
and the Vita's own internals, with the earlier verdicts treated as open. Sources: the Vita scene
(novsync, VGi, VitaGrafix, Framecapper, divasplit, PSVshell/PSVshellPlus, LOLIcon, reRescaler,
vitaGL, libvita2d, SDL, RetroArch's GXM driver, Vita3K, the henkaku wiki, vita-headers, xerpi's
baremetal DSI tables), PC and mobile low-latency work (Reflex, Anti-Lag 2, XeLL, LatencyFleX,
RTSS, Special K, RetroArch, Android Swappy) and other console scenes (SaltyNX/FPSLocker,
GoldHEN, gamescope, RPCS3, PPSSPP). **[SV]** = read in source or primary doc, **[INF]** =
inference. Links are at the end.

## 1. Findings that change earlier conclusions

### 1.1 Controller samples arrive once per vblank, so jit is aimed at the wrong target

**[SV]** henkaku SceCtrl: `ctrl_intr_handler` runs at 60 fps and starts `syscon_ctrl_read`
through `sceSysconCmdExecAsync`; the SceSyscon page lists the controller commands
(0x100/0x101/0x104/0x180) as used in `SceCtrlVblankHandler`. Vita3K models the same thing:
`ReadBufferPositive` blocks until the next vblank, `Peek` returns the newest sample at once,
`timeStamp` is in microseconds and buffered entries are 16667 us apart. LOLIcon also treats
`timeStamp` as microseconds.

**[INF]** A poll only sees newer data if it crosses the next vblank plus the syscon round trip.
jit (`jitSleep`, `companion/main.c`) sleeps 70% of `budget - max work - 2.5 ms`, which ignores
that:

- At a 60 cap the slack is always under one period, so the sleep cannot reach a newer sample.
  It adds risk with no freshness gain, unless the case below holds.
- At a 30 cap it gains exactly one sample (16.7 ms) when the sleep crosses vblank plus the
  syscon latency. PCSE00352's ~20 ms sleep did; a 15 ms sleep on another title would gain nothing.
- AddEntry returns just after a vblank (the display callback's wait releases the slot). If the
  game polls before that vblank's asynchronous syscon read has landed, it gets the previous
  sample, 16.7 ms old. A 1 to 2 ms sleep would then gain a whole frame, at 60 as well as 30.

So jit should be **phase-targeted**: sleep until just after the latest sample tick that still
leaves room for the frame's work, not as late as the budget allows. The deciding measurement:
in the controller hooks, after the real call, record `now - timeStamp` and the sample's phase
relative to the last vblank, with jit off and on, plus a short tight Peek loop to find when new
samples appear after a vblank.

Precondition: `timeStamp` is microseconds per Vita3K and LOLIcon, but whether it shares an epoch
with `sceKernelGetProcessTimeLow` is unverified. The first device measurement is that comparison
(successive Peek samples against process time across a few hundred vblanks); until it holds,
every number derived from `now - timeStamp` is suspect. If the epochs differ, sample arrival can
still be found from the tight Peek loop alone, by timing when `timeStamp` changes.

### 1.2 jit is blind on 21 titles: the Ext2 controller reads are not hooked

**[SV, on hardware]** 21 of the 216 swept titles have no controller hook bit set (mask bits
11-14): PCSB00467, PCSI00011, BULLY0000, DEADSPACE, GALAXYOF2, GTAVCRT00, GTACTW000, MASSEFFEC,
MAXP00000, SOUL00001, CONDUIT00, KRAS00000, BABEL003D, BACKSTAB0, BGDA00001, CRAZYTAXI,
DKAS00000, HCR000000, JETCAR002, LSWCS0000, MOCO30000. A scan of eight eboots pulled over FTP
for import NIDs:

| Title | Controller and touch imports |
|---|---|
| GALAXYOF2, SOUL00001, HCR000000, MASSEFFEC, LSWCS0000, CONDUIT00 | PeekExt2, TouchPeek |
| BABEL003D | TouchPeek only |
| DOOM64EX1 | Peek, PeekExt2, TouchPeek |

The helper hooks only Peek/Read Positive and Positive2, so `ctrl after` stays 0 and the jit gate
never opens on these titles. Add `sceCtrlPeekBufferPositiveExt` 0xA59454D3, `...Ext2`
0x860BF292, `sceCtrlReadBufferPositiveExt` 0xE2D99296, `...Ext2` 0xA7178860, the Negative forms
(0x104ED1A7, 0x81A89660, 0x15F96FB0, 0x27A0C5FB) and, for touch-only titles,
`sceTouchPeek` 0xFF082DF0 / `sceTouchRead` 0x169A1D58 as phase markers.

### 1.3 IMMEDIATE was ruled out for the wrong case

The August review rejected `SCE_DISPLAY_SETBUF_IMMEDIATE` because the outgoing buffer would be
released to the renderer while still being scanned out. **[INF]** That hazard belongs to a
NEXTFRAME flip with no wait after it: the old buffer stays on screen until the vblank, but the
callback returns and GXM frees it at once. That is exactly fpsvita's Uncap, novsync, VGi's
"DROP ALL" and every homebrew "vsync off" (vitaGL, libvita2d, SDL, RetroArch all keep NEXTFRAME
and just skip the wait **[SV]**). With IMMEDIATE the switch happens at the next line (the wiki
names value 0 `SCE_DISPLAY_UPDATETIMING_NEXTHSYNC` **[SV]**), so the old buffer has left scanout
before it is freed. The result is plain tearing between complete frames.

Supporting evidence: reRescaler renders into the buffer that is on screen every frame and shipped
without corruption reports **[SV code, INF outcome]**; VGi's killswitch threads report 2x speed
and occasional black lines, no torn frames **[SV]**. Nobody in the scene uses IMMEDIATE, and
Vita3K stubs it, so it is unproven on hardware. It is the only route to real "vsync off", which
is what a "vsync off" option means on PC.

### 1.4 A capped wait never lets a late frame through

The August review's "an overrunning frame returns immediately" meant that Multi does not
accumulate drift, which is right. What it does not do is skip: Vita3K models it as waiting until
max(next vblank, the thread's last wake + n) **[SV]**, so a 17 ms frame at a 60 cap still waits
to the next vblank and shows as 30. divasplit's adaptive vsync (AVS) returns at once when
`sceDisplayGetVcount() - last >= vcount`, keeps NEXTFRAME flips, and reports a 17 ms frame
running at about 58 fps instead of 30, no tearing **[SV]**. fpsvita can do the same in every
capped wait for one vcount read.

Caveat: divasplit applies it on the main thread of one title. In most GXM titles the wait runs
in the display-queue callback, and skipping it there releases the old buffer while it is still
scanned out: the Uncap hazard, limited to late frames. Opt-in only, and not before the
measurement shows how many capped frames actually miss by a small margin.

### 1.5 The panel may have 50 and 48 Hz modes (curiosity)

vita-headers lists three internal-panel screen modes for `ksceDisplaySetOutputMode`: 0x0000
(1050x594 total), 0x0020 (1375x567) and 0x0080 (1250x594); xerpi's DSI tables give all three the
same pixel clock **[SV]**. At the clock that makes 1050x594 run at 59.94 Hz, the others come to
about 50.4 Hz and 48.0 Hz **[INF]**; the firmware reports 59.94 for every mode **[SV]**. HDMI
modes on the PSTV include 50 Hz and 24 Hz.

A 50 Hz panel would give an even 50 fps (or 25) to a title that cannot hold 60, which is what
FPSLocker's "Display Sync" (rewriting the Switch's DSI clock and porches) and the Steam Deck's 40
Hz mode do **[SV]**. Costs: vblank-counting games run at 5/6 speed, no Vita prior art, and the
Switch OLED needed gamma fixes below 45 Hz and failed at 40.

The rates are arithmetic on an assumed shared pixel clock, and most retail titles pace on
vblank, so only delta-time titles would benefit. Recorded as a curiosity, not a roadmap item.

### 1.6 Input-to-scanout latency is measurable from inside the plugin

The review said the end-to-end baseline "cannot be measured from inside the plugin". Most of it
can: the controller sample's `timeStamp`, the AddEntry that consumes it, that frame's
SetFrameBuf on the display thread, and the vblank that latches it are all observable. Only the
panel's own response is missing. A per-frame "input age at scanout" in ms, on the overlay and in
the rate line, makes every option measurable. A 240 fps phone camera can calibrate the panel part
once; a white block written into the flipped buffer on a button press (Reflex's flash indicator)
makes that easy.

## 2. Techniques from other projects

| Source | Mechanism | Vita translation |
|---|---|---|
| XeLL "VSync alignment", Special K auto-bias, RetroArch auto frame delay **[SV]** | Closed-loop: time frame completion to land just before vsync; grow the pre-input delay while there is slack, cut it in 1-3 ms steps on a miss, hold before growing again | Measure slack as next vblank minus SetFrameBuf (GPU done) instead of CPU work; this includes GPU time, which jit ignores today |
| Swappy auto mode **[SV]** | 2 s window; >10% misses raises the swap interval; non-pipelined only when `(cpu+gpu+1ms)*1.5` fits | Gate for jit and for an automatic cap suggestion |
| LatencyFleX **[SV]** | Black-box pacing: probe at 1.10x, pace at 0.985x estimated throughput, latency EWMA on drain frames, positive-error compensation. Needs a frame-begin marker; no gain with vsync on | Uncapped GPU-bound titles: AddEntry return as the begin marker, SetFrameBuf as the end fence |
| RTSS back-edge limiter **[SV]** | Delay after present, locked to a clock at the target period, skip one wait after a stutter | softcap today is async-style; lock it to the vblank phase |
| SaltyNX limiter **[SV]** | Wait to `last + period - margin + range*k`, `range` adjusted from a 10-frame average | Self-correcting softcap at 40/45/50 |
| divasplit AVS **[SV]** | Skip the wait if the target vblank has passed | 1.4 above |
| VitaGrafix `peekPatched` **[SV]** | Blocking `sceCtrlRead*` becomes Peek (needed for Killzone at 60) | Per-game key; a blocking read is a hidden vsync |
| SceSyscon `sceSysconGetControlsInfoForDriver` 0x145F59A4 **[SV]** | Synchronous button read (cmd 0x100) | Kernel: fresh buttons at poll time, the only sample newer than the last vblank. Sticks need cmd 0x104 |
| DSI register 0x04C **[SV, partial]** | Bits 31:16 current vcount, bits 15:0 unknown | If 15:0 is a line counter, real scanline sync from the kernel |
| ds4vita **[SV]** | Sticks overwritten at read time from the latest Bluetooth report | On PSTV with a DS4, sticks are already fresh at the read; buttons likely still per vblank |
| FPSLocker-Warehouse, GoldHEN patches **[SV]** | Per-game code/data patches keyed by build, values as expressions of the target fps | VitaGrafix is the Vita's equivalent: coordinate, do not duplicate |
| Steam Deck limiter measurement **[INF, press]** | 60 cap 75.8 ms vs 31.8 ms uncapped (2022) | Supports throttling the game's own submit thread rather than holding queued frames |

## 3. Re-verdicts

| Technique | Earlier verdict | Now |
|---|---|---|
| jit | Largest single win, implemented | Keep, but phase-target it on sample ticks (1.1), close the loop on GPU-inclusive slack, hook Ext2 (1.2) |
| lowlatency (queue depth 1) | Implemented | Unchanged |
| inject after AddEntry | Implemented | Unchanged |
| softcap | Implemented, async | Lock to vblank phase, self-correct (SaltyNX) |
| IMMEDIATE tearing mode | Ruled out, torn GPU writes | Feasible as opt-in "tear" mode for Uncap and as late-tear for near-misses; device check that user SetFrameBuf accepts mode 0 |
| Adaptive vsync | Not considered | Opt-in after measurement (divasplit AVS); in the display callback it carries the Uncap hazard on late frames |
| Scanline sync | Not feasible from user mode | Still not from user mode; possible in kernel if DSI 0x04C bits 15:0 count lines |
| Late input sampling | Reorder and fresher sample not feasible | Fresher button sample possibly from the kernel via syscon (buttons only, contention and cost unknown); Read to Peek as per-game key |
| Non-divisor caps (40/45) | Judder on a 60 Hz panel | Unchanged; a 50 Hz panel mode is a curiosity (1.5) |
| Mailbox | Not feasible | Unchanged; the display layer already holds only the newest frame in Uncap |
| Timer hooks | Do not ship | Confirmed: RPCS3's clock scale is time dilation, its vblank rate only helps vblank-counting games |
| Overclocking | PSVshellPlus owns clocks | Unchanged |
| Analog extrapolation | Opt-in idea | Still unvalidated anywhere; lowest priority |
| Reprojection / frame warp | Not discussed | Not feasible: needs camera and depth |
| Triple-buffer emulation (SaltyNX) | Not discussed | Not worth 2 MB of copy per frame; AVS gives the same 45-55 band |
| Speed-tied detection | Not discussed | No tool exists anywhere; at most a heuristic from the measured rates |
| End-to-end latency measurement | Needs an external rig | Mostly internal (1.6); camera only for the panel |
| Option B, kernel-side hooks | Undecided | Unchanged: the kernel experiments below can live in the existing kernel module, and the syscon read is too weak a reason on its own |

## 4. Roadmap

**Tier 1, fix what is certain, then measure**
1. Hook the Ext/Ext2/Negative controller reads, and touch as a fallback phase marker. Pure
   correctness; all NIDs match vitasdk's SceCtrl.yml and SceTouch.yml.
2. Latency instrument. First check that `timeStamp` and process time share an epoch (1.1),
   then sample `timeStamp` to latch, sample phase against vblank, syscon arrival after vblank,
   and how many capped frames miss their vblank by under 2 ms. Decides the jit redesign and
   whether AVS is worth building.

**Tier 2, rework on the new data**

3. jit: target sample ticks, close the loop on next-vblank minus SetFrameBuf, Swappy-style gate.
4. softcap: vblank-phase-locked back-edge, SaltyNX-style error correction.
5. Read to Peek as a per-game key.
6. Adaptive cap (AVS), opt-in, if the near-miss count from item 2 justifies it.

**Tier 3, new opt-in modes**

7. Before any design: one device check that a user-mode `sceDisplaySetFrameBuf` accepts mode 0
   and tears mid-frame.
8. Tear mode: Uncap with IMMEDIATE flips.
9. Late-tear: IMMEDIATE only when a frame misses the vblank by a small margin.
10. LatencyFleX-style pacing for uncapped GPU-bound titles.

**Tier 4, kernel experiments**

11. Read DSI register 0x04C bits 15:0 across a frame to see whether they count lines.
12. Fresh button read via syscon at poll time, last. Buttons only (sticks need another
    command), contention with SceCtrl's own asynchronous read, unknown cost per synchronous
    transaction.

The 50/48 Hz panel modes (1.5) stay a curiosity outside the roadmap.

## 5. Open questions for the device

- Does `SceCtrlData.timeStamp` share an epoch with `sceKernelGetProcessTimeLow`?
- When does a new controller sample become visible relative to the vblank, and how stale is
  the sample a typical game reads right after AddEntry returns?
- Does a user-mode `sceDisplaySetFrameBuf(..., 0)` tear mid-frame, and does anything break
  with three buffers and with pending=1?
- How often does a capped title miss by under 2 ms (the late-tear and AVS population)?
- Does the panel accept modes 0x0020 and 0x0080, and is the OLED or the LCD model in use here?

## 6. Device results (2026-10-17, same day)

Tier 1 was built into the helper and run on the Vita with a player at the controls
(`companion/main.c`: Ext/Ext2/Negative and touch hooks, sample-age and phase numbers in the
rate line, development-build switches for the probe and IMMEDIATE flips, `freshPoll`,
the phase-targeted `jitSleep`, and `scan`, the age of the newest delivered controller sample at
the flip). **[SV, on hardware]** throughout. 

### 6.1 The stamp, the clock and when samples land

- `SceCtrlData.timeStamp` is on the process clock (`sceKernelGetProcessTimeWide`). Against
  the system clock it reads as garbage. The 1.1 precondition holds.
- Samples arrive once per vblank (stamp step 16.0 to 17.2 ms). The probe (wait a vblank, spin
  on Peek): the sample visible at the vblank is stamped 15.1 to 16.2 ms earlier; the next one
  appears 0.4 to 1.7 ms after the vblank, stamped 30 us before it is seen. The stamp is taken
  when the data lands, 0.5 to 1.5 ms after the vblank.

### 6.2 jit was aimed at the wrong target, confirmed; the fix is a re-poll

A game that polls right after AddEntry returns polls right after the vblank and loses the race
with the landing sample by under a millisecond, every frame. `freshPoll`: when a read returns
a sample 15 ms or older, re-poll until the stamp changes, 2.5 ms at most. Self-gating by age.

| Title, cap | Read after vblank | Age at the read | With fresh poll | Spin, timeouts |
|---|---|---|---|---|
| Wolf Among Us, 30 | 0.2 to 0.5 ms | 15.9 ms | 1.0 ms | 0.50 ms, 0 of 842 |
| MGS2 HD, 60 (menu) | 0.6 ms | 13.7 ms | 0.5 ms | 0.83 ms, 0 of 662 |
| Galaxy on Fire 2, off | n/a (2 reads a frame) | 15.3 ms | 0.03 ms | 1.4 ms, 1 of 6778 |
| Road Rage 2, 30 | 7.5 to 18 ms (mid-frame) | 6.3 to 7.8 ms | unchanged | rarely due |

The old slack sleep was harmful at a 60 cap: on MGS2 it moved the read 4 to 10 ms past the
vblank, where the sample is 3 to 9 ms old, and the re-poll (which needs a 15 ms-old sample)
no longer fired. `jitSleep` now sleeps whole vblanks only, k = what the cap budget allows after
the worst recent work, the re-poll wait and a margin; at a 60 cap k is always 0.

### 6.3 Input age at the flip, per frame (`fscan`)

`fscan` records the newest delivered controller sample at AddEntry and ages it at that frame's
own flip, tied by the framebuffer address in the game's callback data where the title stores
it there (Oddworld, Wolf Among Us: most flips; MGS2: a few, FIFO order for the rest). The
earlier `scan` aged the sample at whichever flip came next and understated a 2-deep queue by
up to a period (Wolf Among Us: 25 ms shown, 56 ms real). `flag` is the frame's own
submit-to-flip. First comparable window, same content, jit off then on:

| Title, cap | fscan off | fscan on | Fresh poll | Sleep | flag off / on |
|---|---|---|---|---|---|
| MGS2 HD, 30 (menu) | 40.2 ms | 15.5 ms | 641, 0 timeouts | 556 of 900, 1 miss | 28.6 / 11.8 ms |
| MGS2 HD, 30 (in-engine cutscene) | 32.2 ms | 15.6 ms | 1025 | 1828 of 3000, 2 backoffs | 23.8 / 9.4 ms |
| Oddworld, 30 (gameplay) | 23.2 ms | 9.1 ms | 1770 | never (queue time 5.7 ms) | 5.7 / 5.6 ms |
| Wolf Among Us, 30 (gameplay, 18 fps) | 55.9 ms | 41.6 ms | 842 | never (no budget) | 21.8 / 18.9 ms |
| The Swapper, 30 (gameplay) | 48.9 ms | 34.8 ms | 2016 | never (16 to 25 ms of work) | 8.1 / 7.9 ms |
| DOA5 Plus, 60 (menu) | 49.4 ms | 38.0 ms | 601 | never (60 cap) | 32.5 / 32.5 ms |
| DOA5 Plus, 60 (gameplay) | 34.6 to 36.1 ms | 32.8 ms | 3371 | never | 25 to 27 / 26 to 27 ms |
| Galaxy on Fire 2, uncapped 30 (vitaGL) | 21.1 to 24.0 ms | 14.8 to 15.9 ms | 6307 of ~6000 reads, 2 timeouts | never (off) | 4.8 to 8.0 / 3.8 to 8.9 ms |
| Road Rage 2, 30 (`scan`, batch 2) | 32.2 ms | 32.2 ms | rarely due | 280, 2 misses | |

Galaxy on Fire 2 pays about 1.4 ms of spin on every read, two reads a frame, and loses nothing:
it is vsync-bound at 33 ms of work either way, so the spin eats idle. DOA5 Plus in gameplay
already polls 1 to 1.3 ms after the vblank on a 2.3 ms-old sample, so fresh poll gains little
there; its 33 to 36 ms at 60 fps is queue depth (`flag` 25 to 27 ms), which is `lowlatency`'s
territory and unmeasured on `fscan`.

Frame rates were unchanged in every pair. Fresh poll is worth 11 to 14 ms wherever the game
polls right after the vblank on a vblank-old sample (MGS2, Oddworld, Wolf, Swapper, DOA5 menus,
Galaxy on Fire 2), and 2 to 3 ms where the poll already lands on a fresh one (DOA5 gameplay). The whole-vblank sleep is worth another
17 to 25 ms only where the frame would otherwise sit queued for a vblank or more (MGS2), and
the helper now measures that: while not sleeping it learns the per-frame submit-to-flip and
sleeps at most that much; while sleeping, a flip that gets later than the learnt baseline backs
it off for 60 frames and relearns (`hurt`). Without that gate (batch 5) the same sleep pushed
Oddworld's flips a vblank later: submit-to-flip 6.7 to 23.4 ms, flip age up 4 ms.

MGS2 HD is native 30 and speed-tied above it, so its 60-cap runs are at double game speed and
only compare with each other. The second MGS2 cutscene (in-engine, heavy) was the like-for-like
content; each pair was played from the same spot.

`lowlatency=1` (queue depth 1, now `lowlat=`) changed little on MGS2 and Wolf (batch 3,
`scan`: MGS2 at 60, menu, 26.4 ms with or without it; Wolf with jit 10.8 to 8.1 ms, inside the
noise). On DOA5 Plus at 60, which keeps the queue full (`flag` 25 to 32 ms), it is the bigger
lever: menu `fscan` 19.6 ms with lowlat and antilag against 38.0 ms with antilag alone and 49.4
ms with neither.

### 6.4 IMMEDIATE is rejected from user mode

With every flip sent as `SCE_DISPLAY_SETBUF_IMMEDIATE`, DOOM64EX1, MGS2 HD and Road Rage 2 all
returned `SCE_DISPLAY_ERROR_INVALID_UPDATETIMING` (0x80290006) on every call and the panel
stayed on the loading screen while audio continued; MGS2's submit thread ran at 370 presents
per second because the display wait only blocks after an accepted flip. 1.3 was right about the
mechanism but it is not reachable from user mode on this firmware. Tier 3 items 7 to 9 are
dropped unless done through `ksceDisplaySetFrameBuf` in the kernel module.

Kernel path (batch 9): the kernel module now exports `fpsvitaSetFrameBufImmediate`, which
copies the frame buffer descriptor in and calls `ksceDisplaySetFrameBuf(..., IMMEDIATE)` in the
caller's process context (a development-build switch at the time, now the Tearing mode; falling back to the normal flip on error).
Doom64EX and The Swapper, uncapped: 6000 calls each, one error each (the very first call,
0x80290006), every later call accepted, no panic; the screen kept updating at 44 to 733
presents per second. Whether the accepted calls switch at the next line (tearing) or are
quietly treated as next-vsync was settled by timing (batch 11): with `sceDisplayWaitSetFrameBuf`
called right after every flip, Doom64EX's intro ran at 43 fps on normal flips (the wait blocks
until the vblank) and at 563 fps on kernel IMMEDIATE flips (the wait returned at once). The
driver applies them immediately. Possible tearing was visible on that intro, nothing
conclusive on the small screen; `sceDisplayGetFrameBuf(IMMEDIATE)` reports the last buffer set,
not the one on screen (100% match either way), so it cannot tell. A 240 fps phone recording
would show the seams. Tear mode is therefore feasible from the kernel module: an opt-in
"Allow tearing" for Uncapped (every flip IMMEDIATE) and a late-tear variant for capped frames
that miss by a small margin (items 8 and 9 of the original roadmap, kernel path).

### 6.5 Still open

- `helper.log` showed garbage after long rate lines (batches 7, 8 and 11): the line outgrew its
  384-byte buffer and `sceClibSnprintf` does not terminate on truncation, so helperLog ran past
  it. Not an FTP race and not interleaving (the lock stays). Buffer is now 640 bytes with a
  forced terminator and newline.

- Near misses: `miss` is dominated by titles that cannot hold their cap in gameplay (MGS2 at
  60: 175 in 900 frames), `near` stays small; too early to decide on AVS (item 6).
- Galaxy on Fire 2 pays about 2.8 ms of spin per 33 ms frame for the 7.8 ms gain; a CPU-bound
  port could notice. Cap the spin or skip it when the spin EMA is high, if a title shows it.
- Road Rage 2 reads mid-frame; neither technique reaches it. A read-to-vblank phase shift
  (sleep the read itself, not the submit thread) is the only lever there.
- Doom64EX with jit (uncapped, 44 fps): fresh poll fires on a fifth of the reads, delivered
  age 3.6 to 7.9 ms, no baseline `scan` yet.
- The 50/48 Hz modes, DSI 0x04C and the syscon read were not touched.

### 6.6 Roadmap, revised

1. Done: Ext/Ext2/Negative/touch hooks; latency instrument (`ts`, `rd_ph`, `ts_ph`, `fresh`,
   `out`, `scan`, `fscan`, `flag`, `tied`, `miss`, `near`, `hurt`); fresh poll and the
   queue-time-gated whole-vblank sleep under `jit=1`.
2. Done: `scan` with `lowlatency=1` (small) and with jit at a 30 cap on a native-60 title
   (MGS2, native 30: 40 to 16 ms). Next: the same pair on more capped titles,
   including a native-60 one capped to 30, for generality and the miss/backoff behaviour.
3. softcap: vblank-phase-locked back-edge, judged by `scan`.
4. AVS only if the near-miss population appears in play logs.
5. Done: Tearing mode (Uncapped with every flip through `ksceDisplaySetFrameBuf`, the FPS
   row's last entry, `tear=` key); seams confirmed on a slow-motion phone recording.
   Late-tear for near misses, DSI 0x04C and the syscon read remain.


## Sources

- novsync: https://github.com/junminlee2004/novsync
- VGi: https://github.com/Electry/VGi ; reports: https://github.com/Electry/VitaGrafixPatchlist/issues/37
- VitaGrafix: https://github.com/Electry/VitaGrafix , https://github.com/Electry/VitaGrafixPatchlist
- Framecapper: https://github.com/Rinnegatamante/Framecapper
- divasplit: https://github.com/xpuska513/divasplit
- PSVshell: https://github.com/Electry/PSVshell ; PSVshellPlus: https://github.com/GrapheneCt/PSVshellPlus
- LOLIcon: https://github.com/dots-tb/LOLIcon ; reRescaler: https://github.com/Rinnegatamante/reRescaler
- ds4vita: https://github.com/xerpi/ds4vita ; baremetal DSI: https://github.com/xerpi/vita-libbaremetal , https://github.com/xerpi/vita-baremetal-sample
- henkaku wiki: https://wiki.henkaku.xyz/vita/SceCtrl , https://wiki.henkaku.xyz/vita/SceSyscon , https://wiki.henkaku.xyz/vita/SceDisplay , https://wiki.henkaku.xyz/vita/DSI_Registers
- vita-headers: https://github.com/vitasdk/vita-headers ; Vita3K: https://github.com/Vita3K/Vita3K
- Adrenaline input lag camera test: https://gbatemp.net/threads/i-tested-adrenaline-input-lag-vs-adrenaline-bubble-manager-nopspemudrm-and-by-extension-chovy-sign.637540/
- LatencyFleX: https://github.com/ishitatsuyuki/LatencyFleX , https://blog.ishitatsuy.uk/post/latencyflex/
- Reflex: https://www.nvidia.com/en-us/geforce/news/reflex-low-latency-platform/ ; Anti-Lag 2: https://gpuopen.com/learn/integrating-amd-radeon-anti-lag-2-sdk-in-your-game/ ; XeLL: https://www.intel.com/content/www/us/en/developer/articles/technical/xell-developer-guide.html
- RTSS limiter modes: https://forums.guru3d.com/threads/is-someone-able-to-explain-how-async-and-back-edge-sync-differ-thanks.447580/
- Special K: https://wiki.special-k.info/Advanced/Video ; RetroArch: https://github.com/libretro/RetroArch/blob/master/gfx/video_driver.c
- Swappy: https://developer.android.com/games/sdk/frame-pacing , https://android.googlesource.com/platform/frameworks/opt/gamesdk/+/refs/heads/main/games-frame-pacing/common/SwappyCommon.cpp
- SaltyNX / FPSLocker: https://github.com/masagrator/SaltyNX , https://github.com/masagrator/FPSLocker , https://github.com/masagrator/FPSLocker-Warehouse
- GoldHEN: https://github.com/GoldHEN/GoldHEN_Patch_Repository , https://github.com/GoldHEN/GoldHEN_Plugins_Repository
- gamescope: https://github.com/ValveSoftware/gamescope ; RPCS3: https://github.com/RPCS3/rpcs3 ; PPSSPP: https://github.com/hrydgard/ppsspp
- Steam Deck limiter latency: https://www.tomshardware.com/uk/news/steam-deck-framerate-limiter-egregiously-raises-input-latency
