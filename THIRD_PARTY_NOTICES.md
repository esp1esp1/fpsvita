# Third-party notices

For anyone redistributing or building on fpsvita: the projects it uses or borrows from, and
their licences. fpsvita itself is licensed under GPL-3.0 (see [LICENSE](LICENSE)).

## Code, patterns and tools

| Project | Author | Used for | When | Licence |
|---|---|---|---|---|
| [Framecapper](https://github.com/Rinnegatamante/Framecapper) | Rinnegatamante | The display-wait hook approach | Adapted in the source | GPL-3.0 |
| [PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus) | GrapheneCt | The quick-menu hook pattern, impose widget ids, overlay styling | Adapted in the source | MIT |
| [taiHEN](https://github.com/yifanlu/taiHEN) | yifanlu | Plugin loading and hooks | Runtime dependency | MIT |
| [kubridge](https://github.com/bythos14/kubridge) | bythos14 | User-mode exception handler for the shell's crash log (weak import, optional) | Runtime dependency | None stated |
| [ScePaf-RE](https://github.com/VitaArchive/ScePaf-RE) | VitaArchive | Reverse-engineered ScePaf headers and export lists. `shell/paf_stubs/ScePafRE.yml` is generated from them, and `shell/paf_compat/` carries a few of its headers with GCC fixes | Build time | None stated |
| [vitasdk](https://vitasdk.org) | vitasdk contributors | Toolchain, headers and stubs | Build time | Various |
| [acdc](https://github.com/isage/acdc) | isage | Builds the RCO | Build time | See its repository (not redistributed) |

## Names

Option names borrowed from the Steam Deck, RTSS and NVIDIA are used only to explain the
options. No code from them is used.

## Projects whose source informed the design

- Vita: Framecapper, VitaGrafix and VGi, PSVshell and PSVshellPlus, divasplit, vitaGL,
  libvita2d, RetroArch's GXM driver, Vita3K.
- Latency on other platforms: NVIDIA Reflex, AMD Anti-Lag 2, Intel XeLL, LatencyFleX, RTSS,
  Special K, RetroArch's frame delay, Android Swappy.
- Other consoles and compositors: SaltyNX and FPSLocker (Switch), GoldHEN (PS4), gamescope.
- Documentation: the henkaku wiki pages on SceCtrl, SceSyscon and SceDisplay.
