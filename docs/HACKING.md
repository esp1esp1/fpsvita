# Hacking on fpsvita

## Building

Prerequisites:

- [vitasdk](https://vitasdk.org), with `VITASDK` set and `$VITASDK/bin` on `PATH`
  (`vita-libs-gen-2` runs at build time).
- [ScePaf-RE](https://github.com/VitaArchive/ScePaf-RE) C++ headers copied into the SDK:
  `cp -r ScePaf-RE/include/* $VITASDK/arm-vita-eabi/include/`. Do not link the
  `libScePaf*_stub.a` archives from its `lib/`: they are in Sony's stub format, which
  vita-elf-create ignores, so every PAF call would return -1. The import stubs are generated
  at build time from `shell/paf_stubs/ScePafRE.yml` instead, which `gen_yml.py` derives from
  the ScePaf-RE `.emd` export lists (NID = SHA-1(name + "PAF")[:4], little-endian).
  `shell/paf_compat/` holds a few ScePaf-RE headers with GCC fixes and shims for the MSVC-only
  ones; it is on the include path before the SDK.
- [acdc](https://github.com/isage/acdc) for the RCO:
  `git clone https://github.com/isage/acdc.git acdc-tool && cmake -B acdc-tool/build acdc-tool && cmake --build acdc-tool/build`.
- [kubridge](https://github.com/bythos14/kubridge) on the device: the shell's crash handler
  logs the pc, lr, far and owning module of a SceShell fault to `ur0:data/fpsvita/boot.log`.

Build:

```bash
(cd res && bash gen_rco.sh)        # res/fpsvita_plugin.rco (gen_rco.bat on Windows)
cmake -B build && cmake --build build
```

Outputs: `build/kernel/fpsvita-kernel.skprx`, `build/companion/fpsvita-helper.suprx`,
`build/companion/fpsvita-helper-lite.suprx`, `build/shell/fpsvita-ui.suprx`. CMake builds the
kernel first because the other two link against its generated stub. All modules build with
`-fno-section-anchors`; `arm-vita-eabi-nm <elf> | grep LANCHOR` must stay empty (see the notes).

Host tests, no device needed: `bash tests/run.sh` compiles the state-file and auto-detect code
out of the plugin sources and runs them under ASan and UBSan.

Install: the three modules to `ur0:tai/`, the RCO to `ur0:data/fpsvita/`, and the
`ur0:tai/config.txt` lines in the README. Always replace all four together.

## Architecture

Three plugins communicate through a kernel module acting as shared state:

```
fpsvita-kernel.skprx  (*KERNEL)
  volatile int32_t: interval, inject, autodetect, native_interval
  volatile uint32_t[3]: titleid (packed as 3 x u32)
  Exports FpsvitaForUser library: syscall-accessible getters/setters. The stats push and
  the Tearing flip are accepted only from the process that wrote the title id.

fpsvita-ui.suprx  (*main / SceShell)
  shell/hud.cpp: overlay page (fc_page_hud) over the game, PSVshellPlus-style; fed by
  FpsvitaStats that the helper pushes through the kernel every 250 ms while hud != 0.
  Two-stage hook init (mirrors PSVshellPlus pattern):
    Stage 1: sceAVConfigRegisterCallback (one-shot) → load RCO, get impose root,
             read config.txt, push to kernel, hook stage 2
    Stage 2: paf::system::SupportsWiredEthernet → fires each quick menu open
             → MainThreadCallList creates Impose widget
  Impose widget is created on every quick menu open, destroyed on close.
  Impose root obtained via power_manage_plugin interface at vtable offset +0x54.

fpsvita-helper.suprx  (*ALL / every game process)
  On start: detect titleid (sceAppMgrAppParamGetString param 12), install hooks in the
  main module; if it imports no display or GXM function, hook the module loaders and retry
  on each late module. Title id, native reset and per-game override go to the kernel when
  the first display or GXM hook lands (controller hooks alone do not count).
  Hooks by NID with TAI_ANY_LIBRARY (bit i of the logged hook mask = entry i):
    0-3   sceDisplayWaitVblankStart(Multi)(CB)      0xDD0A13B8 0x5795E898 0x05F27764 0x78B41B92
    4-7   sceDisplayWaitSetFrameBuf(Multi)(CB)      0x7D9864A8 0x9423560C 0x3E796EF5 0x814C90AF
    8     sceDisplaySetFrameBuf 0x7A410B64          auto-detect, inject, present period, stats push
    9     sceGxmInitialize 0xB0F1E4EC               lowlatency queue depth
    10    sceGxmDisplayQueueAddEntry 0xEC5C26B5     submit thread: work/period/lag, jit, softcap
    11-14 sceCtrl(Peek|Read)BufferPositive(2)       controller read phase, sample age
    15-22 sceCtrl(Peek|Read)Buffer(PositiveExt|PositiveExt2|Negative|Negative2)
                                                   same; Ext2 is what vitaGL ports use
    23-24 sceTouch(Peek|Read)                       phase marker only when no ctrl hook landed
  No threads, no polling: reads kernel state on every hook invocation.
  Forced vsync (inject) waits after sceGxmDisplayQueueAddEntry on the submit thread while
  that hook is active, else inside SetFrameBuf.
```

### Interval values (`common/fpsvita_common.h`)

| Value | Meaning |
|-------|---------|
| `-1` | Default (passthrough, shown as "Default") |
| `1`  | 60fps |
| `2`  | 30fps |
| `3`  | 20fps |
| `0`  | Uncapped (Tearing when `FPSVITA_ADV_TEAR` is also set) |

UI cycle order: Default → 60fps → 30fps → 20fps → Uncapped → Tearing → Default. "Tearing" is
Uncapped plus `FPSVITA_ADV_TEAR` (every flip IMMEDIATE through the kernel's
`fpsvitaSetFrameBufImmediate`). UI labels and config keys use the same PC terms (Default,
Vsync, Latency row with Lowlat / Antilag / Limiter); the code keeps its older internal names
(interval, inject, autodetect, jit, softcap, lowLatency).

### Config and state files (`ur0:data/fpsvita/`)

`config.txt` holds the globals, all shell-owned and regenerated whole on every change:
`limit`, `vsync` (`auto`/0/1, the old inject + autodetect pair), `hud`, `hudpos`, `lowlat`,
`antilag`, `limiter`, `tearing`. `state.txt` holds one
`TITLEID=<limit>[,lowlat=,antilag=,limiter=,tearing=]` line per title and nothing else;
`SavePerGame()` replaces or appends this title's line and keeps every other line verbatim,
refusing to write if the file does not fit the read buffer (`FPSVITA_STATE_BUF`). The shell
writes `config.txt` with the defaults when it is missing; nothing is read from a pre-split
`state.txt` any more. The helper reads both files at game start (`readGameConfig`: two reads,
one buffer). The helper pushes the effective latency options to the kernel
(`fpsvitaSetAdvanced`) and reads them back every frame, so the Latency row changes antilag,
the limiter and tearing live.

The lite helper (`fpsvita-helper-lite.suprx`, `-DFPSVITA_LITE`, `companion/lite.h`) has no
kernel module: the kernel accessors are file-local over a process-private `FpsvitaState`,
seeded from the two files in `module_start`. Same source as the full helper; never deploy
both.

### Auto-detect inject

The helper tracks `g_frame_count` (incremented in `SetFrameBuf`) and `g_last_vblank_frame`
(set in the vblank-wait hooks). `autodetectTick` engages inject when the gap exceeds
`FPSVITA_AUTODETECT_ENGAGE` (30 presents) **and** the measured present period is at least
`FPSVITA_AUTODETECT_RATE_MARGIN_PCT` shorter than the cap's, and releases it only after
`FPSVITA_AUTODETECT_RELEASE_STREAK` (60) consecutive presents within
`FPSVITA_AUTODETECT_RELEASE` (2) of a wait. The comment above the constants in
`common/fpsvita_common.h` explains why a plain engage/release pair oscillates;
`tests/autodetect.cpp` exercises the state machine.

### config.txt ordering

Put `fpsvita-ui.suprx` **before** `PSVshellPlus_Shell.suprx` in `*main` for the fpsvita rows
to appear above PSVshellPlus's (taiHEN LIFO hook ordering); after it puts them below.


## Key Implementation Notes

- **Deploy kernel, helper and shell together**: `FpsvitaState` is copied out whole, and a
  module built against a shorter struct gets its stack overrun.
- **Kernel values come back through `fpsvitaGetState()`**, a copy-out syscall. Syscall return
  values lose bit 30 on this firmware (-1 arrives as 0xbfffffff). The `fpsvitaGet*()` helpers in
  `common/fpsvita_common.h` wrap it; never add a getter that returns a value.
- **ScePaf is called by its ScePaf-RE names** through stubs generated from `ScePafRE.yml`. Weak
  imports bind lazily on first call; a NID the firmware lacks traps at pc=0 with ip = stub
  address. `gen_yml.py` `EXTRA` holds the two exceptions.
- **`Plugin::InitParam` is filled as a raw 0x94-byte buffer** (see coreInitPlugin); never
  instantiate the ScePaf-RE class, paf_compat's `std::map` changes its layout.
- **Shell is built with `-fshort-wchar`**: PAF wide strings are 16-bit.
- **All modules build with `-fno-section-anchors`.** GCC's section anchors can land past the
  end of `.bss`; vita-elf-create then drops those relocations and the first access to the
  static faults at its link-time address (fsr 7, far = 0x8102xxxx). `nm | grep LANCHOR` on
  the ELFs must stay empty.
- **Never read checkbox state through ScePaf-RE's `IsChecked()`** (inline field read, wrong
  offset). Track the state yourself and flip it on each decide; `SetCheck()` is a real export.
- **lowlatency only applies where `sceGxmInitialize` goes through the hooked import** after
  the helper starts; some homebrew (DOOM64EX1) initialises GXM another way and the overlay
  keeps reporting `LOW LAT PENDING` (`RESTART GAME` in Full).
- Show/hide a widget with `widgetShow`/`widgetHide` (PlayAnimation), not `Show()`/`Hide()`.
- **Helper vblank hooks wait through the helper's own SceDisplay imports** when capping, never
  via `TAI_CONTINUE` with a changed count: on DOOM64EX1 that continuation returned
  `0x80290001` immediately and removed all pacing. Off chains unchanged.

- **VitaGrafix FPS patches set the vblank count inside game code**, so on those titles the
  helper's native value is VitaGrafix's and a cap overrides it; its `withWait` titles would
  stack a second wait behind inject (none installed). 
- **Shell plugin is C++** (required for ScePaf widget API). Companion and kernel are C.
- `SYSCALL_ENTER`/`SYSCALL_EXIT` macros (ARM CP15 TPIDRURO manipulation) wrap the kernel exports, as in other plugins. They are not what mangles return values (tested).
- Widget child lookup uses `IDParam("string_id")` which hashes the string via ScePaf's internal hash. Hardcoded numeric hashes (e.g. `0x0EE0C8AF`) are for impose_plugin widgets borrowed from PSVshellPlus.
- Native interval (`nativeInterval`) is captured from the game's first vblank call and displayed as `"30fps (game 60)"` in the mode label; a sustained change (16 identical requests) updates it.
- Save button visibility: hidden when no game is running (titleid empty), shown otherwise.
- `fpsvita-ui.suprx` returns `SCE_KERNEL_START_NO_RESIDENT` (bails cleanly) if the RCO file or kernel module is missing.

## The helper log

`ur0:data/fpsvita/helper.log` gets a few lines per game process the helper starts in, all at
startup: `helper: <TITLEID> hooks=.. mask=.. loaders=.. pergame=..` (the mask bits as in the
hook table above), one `helper: late module ..` line per late module that added a hook, and
`helper: gxm init pending=<queue depth> lowlat=<0|1>` when the title initialises graphics
through the hooked import. There is no periodic logging: the helper never writes to flash from
a game thread once the title is running. The per-frame numbers go to the overlay through the
kernel instead.

The shell's `ur0:data/fpsvita/boot.log` gets a handful of lines per boot, one
`Impose: game=.. interval=.. native=..` line per quick-menu open, and the crash handler's
register dump if SceShell faults.
