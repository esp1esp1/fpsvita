# Developer guide

For developers who build or extend fpsvita: build, tests, architecture, the pitfalls the code
depends on, and the log formats. The player's view is in the [user guide](GUIDE.md).

## Prerequisites

On the build machine:

- [vitasdk](https://vitasdk.org), with the `VITASDK` environment variable set. CMake stops
  without it. The modules link the taiHEN stubs that come with it.
- `vita-libs-gen-2` on the `PATH`. It generates the ScePaf import stubs during the build.
- The kubridge stub library (`kubridge_stub_weak`), from kubridge's vitasdk package.
- The [ScePaf-RE](https://github.com/VitaArchive/ScePaf-RE) headers (main branch), copied
  into the SDK (`shell/paf_compat/`, first on the include path, overrides a few of them with
  GCC fixes):

  ```
  cp -r ScePaf-RE/include/* $VITASDK/arm-vita-eabi/include/
  ```

- [acdc](https://github.com/isage/acdc) by isage, to build the RCO. `res/gen_rco.sh` expects
  it cloned to `acdc-tool/` at the repository root and built there:

  ```
  cmake -B acdc-tool/build acdc-tool && cmake --build acdc-tool/build
  ```

  Set `ACDC` and `SCHEMA_DIR` to use a copy elsewhere. On Windows, `res/gen_rco.bat` uses the
  official SDK's `acdc.exe` instead.
- Python 3 and a host `g++` with AddressSanitizer for the tests.

On the device: taiHEN, and optionally kubridge v0.3 or later, which the quick-menu plugin uses
to log a crash.

## Build

```
bash res/gen_rco.sh
cmake -B build
cmake --build build
```

| Output | Install to |
|---|---|
| `res/fpsvita_plugin.rco` | `ur0:data/fpsvita/fpsvita_plugin.rco` |
| `build/kernel/fpsvita-kernel.skprx` | `ur0:tai/`, under `*KERNEL` |
| `build/shell/fpsvita-ui.suprx` | `ur0:tai/`, under `*main` |
| `build/companion/fpsvita-helper.suprx` | `ur0:tai/`, under `*ALL` |
| `build/companion/fpsvita-helper-lite.suprx` | `ur0:tai/`, under `*ALL`, instead of the three above |

The kernel module builds first: its `kernel/exports.yml` produces `libFpsvitaForUser_stub.a`,
which the helper and the shell link. `kernel/CMakeLists.txt` generates it itself because
`vita_create_stubs` runs `make` without `VITASDK` and cannot find the assembler.

### ScePaf import stubs

The shell calls ScePaf (the system UI library) by the names ScePaf-RE uses. Its import stubs
are generated during the build from `shell/paf_stubs/ScePafRE.yml` by `vita-libs-gen-2`, as
weak imports.

Do not link the `libScePaf*_stub.a` archives in ScePaf-RE's `lib/`. They are in Sony's stub
format, which `vita-elf-create` does not understand: the module ends up with no imports and
every ScePaf call returns -1.

`ScePafRE.yml` is checked in. Regenerate it only when ScePaf-RE changes:

```
python3 shell/paf_stubs/gen_yml.py path/to/ScePaf-RE/lib > shell/paf_stubs/ScePafRE.yml
```

`gen_yml.py` reads the export names from ScePaf-RE's `.emd` lists and computes each NID as the
first four bytes, little-endian, of SHA-1(name + the library's NID suffix). Its `EXTRA` table
adds three exports the lists do not name, with NIDs taken from those stub archives:
`paf::String::String(const char*)` and `Widget::PlayAnimation` / `PlayAnimationReverse`.

## Tests

```
bash tests/run.sh
```

It runs three suites on the host:

- **compat table**: `tools/gen_compat.py --check` fails when `shell/compat_data.h` no longer
  matches the incompatible table in `docs/compatibility-list.md`.
- **statetest**: the `config.txt` and `state.txt` read and write code from
  `shell/impose.cpp` and `companion/main.c`: format round-trip, preservation of unknown keys
  and lines, refusal to truncate, and the hand-off from shell to helper.
- **autodetect**: the Vsync auto-detection state machine (`autodetectTick`) across realistic
  per-title frame patterns.

`run.sh` extracts the functions from the plugin sources at run time and compiles them for the
host, `sce*` calls mapped to libc, under ASan and UBSan. Renaming an extracted function makes
the extraction fail loudly.

### Updating the compatibility table

The quick menu's `(!)` and the overlay's known-problem tips come from
`shell/compat_data.h`, which is generated from the `## Probably incompatible` table in
`docs/compatibility-list.md`. After editing that table, run:

```
python3 tools/gen_compat.py
```

The script reads every `PCSxNNNNN` id in the second column and classifies the third column by
keywords (`faster`, `double speed`, `freezes`, `no input above`, `videos break` and others;
see `FLAGS` in the script). Keep the heading, the column order and the row wording that the
keywords depend on.

## Architecture

```
fpsvita-kernel.skprx  (*KERNEL)
  Holds FpsvitaState: limit, Vsync mode, native rate, title id, overlay mode,
  latency flags, Limiter rate and the overlay's numbers. Exports the
  FpsvitaForUser library as syscalls.

fpsvita-ui.suprx  (*main, inside SceShell, C++)
  Reads config.txt at boot and seeds the kernel. Adds the quick-menu rows,
  writes config.txt and state.txt, draws the overlay.

fpsvita-helper.suprx  (*ALL, inside every game process, C)
  Reads config.txt and state.txt at launch, hooks the game's display, GXM and
  controller imports, and reads the kernel state on every hooked call.
```

The kernel module is the only memory both SceShell and a game process can reach, so every
setting passes through it. The code keeps older names for the options: interval (limit),
inject and autodetect (Vsync), jit (Antilag), lowLatency (Lowlat), softcap (Limiter).

### How settings flow

1. **Boot.** The shell's one-shot hook loads the RCO, reads `config.txt` (writing defaults if
   it is missing) and pushes the globals to the kernel.
2. **Game launch.** The helper's `module_start` reads the title id (app parameter 12, `NPXS*`
   system apps are skipped), `config.txt` and this title's `state.txt` line, and hooks the
   main module. If no display or GXM hook lands, it hooks the module loaders and retries on
   each module the game loads.
3. **Ownership.** At the first display or GXM hook, the helper writes the title id (the
   kernel records its process as owner and clears old numbers), resets the native rate, and
   sets the limit and latency flags from the title's line or the globals. With `panel=` it
   then asks for the panel rate.
4. **Every frame.** Each hook reads the whole state with one copy-out syscall. No helper
   threads, no polling.
5. **Quick menu and overlay.** The rows write the kernel, so changes are live; Save writes the
   title's line. While the overlay is on, the helper pushes `FpsvitaStats` every 250 ms and
   the shell polls every 200 ms.
6. **Exit.** `module_stop` clears the title id, then restores the globals. A killed process
   skips `module_stop`: the kernel's process-exit handler clears the title id, numbers and
   panel rate, and the shell restores the globals at the next quick-menu open.

### Kernel exports

| Export | Purpose |
|---|---|
| `fpsvitaGetState` | Copies the whole `FpsvitaState` to the caller |
| `fpsvitaSetInterval` | Limit; out-of-range values become Default |
| `fpsvitaSetInject`, `fpsvitaSetAutodetect` | Vsync forced/auto |
| `fpsvitaSetNativeInterval` | The game's own rate, for the `og:` display |
| `fpsvitaSetTitleIdPart` | Title id as three words; part 0 sets the owner and clears the numbers |
| `fpsvitaSetHud` | Overlay mode and corner |
| `fpsvitaSetAdvanced` | Lowlat, Antilag (full or auto), Tearing flags and the Limiter rate (10 to 120, else off) |
| `fpsvitaSetStats` | Overlay numbers; owner only |
| `fpsvitaSetFrameBufImmediate` | Tearing's immediate flip through the kernel display call; owner only |
| `fpsvitaSetPanel` | Experimental panel refresh change, owner only; see `kernel/main.c` |

### Shell hooks

| Stage | Hook | Purpose |
|---|---|---|
| 1 | `sceAVConfigRegisterCallback` (SceAVConfig `0x79E0F03F` / `0xFB5E3E74`) | One-shot init during SceShell start-up; released after it runs |
| 2 | `paf::system::SupportsWiredEthernet` (ScePafMisc `0x3D643CE8` / `0xB7CFFF5C`) | Fires on every quick-menu open |

On each open, a main-thread task builds the rows from the RCO template `fc_template_impose`
inside the quick menu's root, which comes from `power_manage_plugin`'s interface at vtable
offset `+0x54`. The rows are destroyed when the menu closes, detected by the menu's box
(`0x0EE0C8AF`) disappearing. The overlay is a page of the RCO (`fc_page_hud`) opened once at
draw priority 6 and shown or hidden from then on.

### Helper hooks

All hooks are import hooks by NID with `TAI_ANY_LIBRARY`. Bit *i* of the logged hook mask is
entry *i*:

| Bits | Functions | NIDs | Why |
|---|---|---|---|
| 0-3 | `sceDisplayWaitVblankStart` `Multi`, plain, `MultiCB`, `CB` | `0xDD0A13B8` `0x5795E898` `0x05F27764` `0x78B41B92` | Apply the limit; capture the native rate |
| 4-7 | `sceDisplayWaitSetFrameBuf` `Multi`, plain, `MultiCB`, `CB` | `0x7D9864A8` `0x9423560C` `0x3E796EF5` `0x814C90AF` | Same, for games that wait on the flip instead |
| 8 | `sceDisplaySetFrameBuf` | `0x7A410B64` | Count presents, Vsync detection, present period, overlay push, Tearing flip, per-frame INPUT/QUEUE |
| 9 | `sceGxmInitialize` | `0xB0F1E4EC` | Lowlat: clamp the display queue depth |
| 10 | `sceGxmDisplayQueueAddEntry` | `0xEC5C26B5` | Submit thread: CPU time, Vsync wait, Antilag sleep, Limiter |
| 11-14 | `sceCtrlPeekBufferPositive`, `ReadBufferPositive`, `PeekBufferPositive2`, `ReadBufferPositive2` | `0xA9C3CED6` `0x67E7AB83` `0x15F81E8C` `0xC4226A3E` | Sample age, read phase, fresh poll, `read=peek` |
| 15-22 | `sceCtrl` `Peek`/`Read` `BufferPositiveExt`, `PositiveExt2`, `Negative`, `Negative2` | `0xA59454D3` `0xE2D99296` `0x860BF292` `0xA7178860` `0x104ED1A7` `0x15F96FB0` `0x81A89660` `0x27A0C5FB` | Same; `Ext2` is what vitaGL ports use |
| 23-24 | `sceTouchPeek`, `sceTouchRead` | `0xFF082DF0` `0x169A1D58` | Read phase only, on games with no controller hook |

For games that keep their display calls in a module loaded later, the helper also hooks
`sceKernelLoadStartModule` (`0x2DCC4AFA`), `sceKernelStartModule` (`0x702425D5`) and
`_sceKernelLoadStartModule` (`0x60647592`) in the main module and retries the hooks on each
new module.

The native rate is the game's first wait count, updated after `FPSVITA_NATIVE_STREAK` (16)
identical requests. The Vsync wait runs after `sceGxmDisplayQueueAddEntry` on the submit thread
while that hook has run in the last 500 ms, else inside `sceDisplaySetFrameBuf`: pacing the
thread that reads input and submits keeps the queue short. Auto-detect engages after
`FPSVITA_AUTODETECT_ENGAGE` presents without a wait when the game is at least
`FPSVITA_AUTODETECT_RATE_MARGIN_PCT` faster than the limit, and releases after
`FPSVITA_AUTODETECT_RELEASE_STREAK` paced presents (`common/fpsvita_common.h` explains why).

### Panel refresh (experimental)

`fpsvitaSetPanel` patches the vertical timing (vtotal and front porch) of the OLED's output
mode 0 entry in SceLowio's display timing table with `taiInjectDataForKernel`. The display
controller re-reads it every frame, so the change is live with no mode switch. The kernel
applies it only in output mode 0 and only when exactly one table entry matches every known
word, so a firmware with another layout is left alone (the Slim's LCD uses the same entry and
is untested). It releases the patch on a new or cleared title id and when the owner exits or
is killed. A sysevent handler restores the stock timing at the first suspend event and
re-applies the rate at late resume (`0x100000`): resuming while stretched latched part of the
display setup, and the shell's layers flickered half-drawn once the game exited. `ksceKernelGetModuleInfo` is resolved
at run time (3.60 and 3.63+ NIDs) so the module still loads on 3.60. `FpsvitaState.panel_hz`
is the rate granted; the helper's vblank length and the overlay's limit and tip maths follow
it.

### Lite variant

`fpsvita-helper-lite.suprx` is built from the same `companion/main.c` with `FPSVITA_LITE`
defined. `companion/lite.h` replaces the kernel exports with file-local functions over a
private `FpsvitaState` in the game process, seeded from the two files in `module_start`.
It has no overlay push, and Tearing is masked out because the immediate flip needs the
kernel.

## Invariants and pitfalls

Each of these is load-bearing. The comments next to the code say more.

### Build

- **Every module builds with `-fno-section-anchors`.** GCC's section anchors can be placed
  past the end of `.bss`; `vita-elf-create` then drops those relocations, and the first
  access to the static faults at its link-time address (fsr 7, far `0x8102xxxx`).
  `arm-vita-eabi-nm` on the ELFs must show no `LANCHOR` symbols.
- **The shell builds with `-fshort-wchar`.** ScePaf wide strings are 16-bit. GCC's default
  32-bit `wchar_t` handed labels a UTF-32 buffer.
- **Never link ScePaf-RE's stub archives** (see [ScePaf import stubs](#scepaf-import-stubs)).
- **A weak import that does not resolve crashes when called.** Imports bind lazily; a missing
  one jumps to address 0, with `ip` holding the stub address.
  ScePaf-RE's `Widget::Show`/`Hide` hash to NIDs this firmware does not export, which is why
  show and hide go through `PlayAnimation` (`widgetShow`/`widgetHide` in `shell/global.h`).
  The shell checks the first instruction of `kuKernelRegisterExceptionHandler` before calling
  it, because kubridge is optional.

### Kernel interface

- **Syscall return values lose bit 30** on the way back to user mode: -1 arrives as
  `0xbfffffff`. All values come back through the `fpsvitaGetState` copy-out, wrapped by the
  `fpsvitaGet*()` helpers in `common/fpsvita_common.h`. Never add a getter that returns a
  value. The helper tests the immediate flip's result only for its sign.
- **Deploy the kernel module, shell and helper together.** `fpsvitaGetState` copies
  `sizeof(FpsvitaState)` into the caller's buffer, so a module built against a shorter struct
  has its stack overrun. A new field means rebuilding and redeploying all three, and updating
  the initializer in `companion/lite.h`.
- **The kernel setters sanitise every value.** The helper passes the limit to the vblank wait
  as an unsigned count, so -2 would become `0xFFFFFFFE` and block the render thread for good.
  Hand-edited files and any process calling the syscall can both supply bad values.
- **Owner-only calls.** Only the process that wrote the title id may push stats, flip
  immediately or change the panel. The immediate flip is a kernel-privilege display write.

### Helper

- **Capped waits go through the helper's own SceDisplay imports**, never through
  `TAI_CONTINUE` with a changed count. On DOOM64EX1 the continuation returned `0x80290001` at
  once, removing all pacing, while the direct call blocked for two vblanks.
- **The injected Vsync wait must stay invisible to the helper's own hooks.** It leaves through
  the helper's import stub, and the hooks patch the game's import table, so the wait does not
  reset the auto-detect counter. If it did, auto mode would release and re-engage forever.
  Switching to export hooks breaks this.
- **Uncapped still yields.** The hooked wait returns after `sceKernelDelayThread(1)`, so a
  high-priority render thread does not starve audio and loader threads on the same core. The
  `CB` variants also process callbacks, or the game would stop receiving suspend and PS-button
  notifications.
- **Take kernel ownership only once a display or GXM hook lands.** A game with its input in
  the executable and its renderer in a later module must still get the loader hooks.
- **Set the limit from the files at launch, never inherit it.** A killed process never runs
  `module_stop`, so the kernel may still hold the last game's override.
- **`module_stop` clears the title id before restoring anything.** The shell treats a change
  seen while a title is named as a user toggle, which would put a bogus entry in the LAST row.
- **No fresh poll on a blocking read of more than one sample.** A second blocking read returns
  only the samples after the first, so re-polling would drop the first batch.
- **`readGameConfig` is `noinline` with a static buffer.** `module_start` runs in every
  process on the device, on a stack that is not the helper's to size.
- **`helperLog` allows one writer at a time** and drops a line after 20 ms rather than stall
  a game thread. `sceClibSnprintf` does not terminate a truncated string.
- **The present counter is atomic.** Some titles present from more than one thread.
- **Lowlat only applies where `sceGxmInitialize` goes through the hooked import.** Homebrew
  that initialises GXM another way (DOOM64EX1) keeps showing `RESTART GAME`.

### Shell

- **`Plugin::InitParam` is filled as a raw 0x94-byte buffer**, never as the ScePaf-RE class.
  `paf_compat` swaps `paf::map` for `std::map`, which makes the class larger than the
  firmware's and shifts every field after it. The overlay's `PageOpenParam` is filled the
  same way.
- **Never read checkbox state with ScePaf-RE's `IsChecked()`.** It is an inline field read at
  an assumed offset. The shell tracks each box's state and flips it on every click;
  `SetCheck()` is a real export and sets the initial state.
- **One callback per arrow**, and **`ui::Text::SetString` by its mangled name**: both avoid
  reading ScePaf-RE fields or vtable slots at assumed offsets.
- **Child widgets are found with `IDParam("string_id")`**, which hashes the string; the numeric
  ids (such as `0x0EE0C8AF`) belong to the quick menu's own widgets.
- **The overlay page is opened once and never closed.** `Plugin::PageClose` has no verified
  NID on this firmware, so Off only hides it, and a failed open is not retried.
- **SCE error check on pointers:** user pointers live at `0x81000000` and above, so test the
  top byte for `0x80`, not `>= 0x80000000`.
- **`module_stop` undoes everything**: both hooks, queued tasks, the live rows, the overlay task
  and the crash handler. Anything left behind points into freed code.
- **Unregister the open task before clearing its flag.** The reverse lets the hook thread
  re-register in the gap and lose the rows for the rest of the session.
- **The overlay uses 64-bit process time.** 32-bit microseconds wrap every 71.6 minutes, which
  would read as a fresh launch.

### Settings files

- **Never rewrite a file that was not read whole.** Readers share one buffer size,
  `FPSVITA_STATE_BUF` (4 KB), so the writer can never produce a file a reader cannot see.
  Save refuses to write when `state.txt` does not fit. Rewriting from a partial read with
  `O_TRUNC` destroys profiles.
- **Save keeps every other line verbatim**, including lines and keys this version does not
  know. `read=peek` and `panel=` are carried over on this title's line.
- **Bound every `snprintf` copy.** `snprintf` returns the length it would have written, so
  adding it unchecked to an offset runs past the buffer.

## Logs

Both logs are in `ur0:data/fpsvita/`.

### helper.log

The helper always writes a few lines per game process at start-up, for example:

```
helper: PCSE00078 hooks=14 mask=0x7ffd loaders=0 pergame=2 read=0
helper: env vg=-1 ib=- fb=- fps=-1 arm=333 gpu=111 bus=222
helper: gxm init pending=2 lowlat=0
```

| Line | Fields |
|---|---|
| `helper: <TITLEID> ...` | `hooks`: hooks installed in the main module; `mask`: which ones (see [helper hooks](#helper-hooks)); `loaders`: loader hooks installed (0 when a display hook landed); `pergame`: the title's limit from `state.txt`, -99 for none; `read`: `read=peek` |
| `helper: env ...` | The title's VitaGrafix config section (`vg` is its `ENABLED`, -1 for none; `ib`, `fb` resolutions; `fps`) and the ARM, GPU and bus clocks in MHz |
| `helper: late module <name> hooks+=<n> mask=...` | Hooks added from a module the game loaded later |
| `helper: gxm init pending=<n> lowlat=<n>` | The display queue depth the game asked for, and whether Lowlat is on |

With `log=1` in `config.txt`, it also writes a `helper: rate` line every 3000 presents
(frames 900, 3000, 6000 and so on), each measuring the 600 presents before it.
`tools/ingest_playlog.py` turns them into TSV rows. Times are in microseconds:

| Field | Meaning |
|---|---|
| `frames`, `flips/s` | Presents so far; presents per second over the window |
| `cur` | Limit: -1 Default, 0 Uncapped, 1 60, 2 30, 3 20 |
| `inject`, `since_vblank` | Auto Vsync wait engaged; presents since the game's own last wait |
| `ae_period` | Submit-to-submit period |
| `work`, `work_max` | CPU time per frame, average and recent maximum (the CPU row) |
| `ctrl_after_pct` | Controller reads within a quarter period after submit; the sleep needs 75 |
| `jit`, `slack`, `applied`, `missed`, `hurt` | Antilag (0 off, 1 full, 2 auto); current sleep; sleeps done; frames missed while sleeping; back-offs because flips came later |
| `softcap`, `softcap_applied` | Limiter rate; frames it held |
| `ll` | One-frame queue in effect |
| `ts=min/ema/max`, `ts_n` | Age of the newest sample at each read; number of reads |
| `rd_ph`, `ts_ph` | Read time and sample time after the last capped wait returned |
| `fresh=hits/timeouts/spin` | Fresh poll results and its average spin |
| `out`, `scan` | Age of the sample handed to the game; its age at the next flip |
| `fscan`, `flag` | The frame's own input age and submit-to-flip at its flip (the INPUT and QUEUE rows) |
| `tied`, `unmatched` | Flips matched to their submit by framebuffer address; flips with no submit found |
| `miss`, `near` | Present periods near twice the limit; CPU frames over budget by 2 ms or less |
| `imm=calls/errors/code` | Tearing's kernel flips, failures and the first error code |
| `present_lag` | Unused, always 0 |


### boot.log

The shell appends a few start-up lines per boot (ending in `stage1 done: ...`, or
`RCO not found - bailing` / `kernel module not found - bailing`), one
`Impose: game=.. interval=.. native=.. inject=.. autodetect=..` line per quick-menu open, and,
with kubridge, a `=== CRASH ===` block on a SceShell fault: registers plus the module and
offset that `pc` and `lr` fall in.
