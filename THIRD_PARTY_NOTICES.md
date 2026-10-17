# Third-party notices

fpsvita is licensed under the GPL-3.0 (see `LICENSE`). It builds on, borrows from or depends
on the following.

| Component | Use | Licence |
|---|---|---|
| [Framecapper](https://github.com/Rinnegatamante/Framecapper) by Rinnegatamante | The helper's display-wait hooks follow its approach. | GPL-3.0 |
| [PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus) by GrapheneCt | Quick-menu hook pattern, impose widget ids, HUD styling. | MIT |
| [taiHEN](https://github.com/yifanlu/taiHEN) | Plugin loading and function hooks (runtime dependency). | MIT |
| [kubridge](https://github.com/bythos14/kubridge) by bythos14 | User-mode exception handler for the shell's crash trace (runtime dependency, weak import). | No licence stated |
| [ScePaf-RE](https://github.com/VitaArchive/ScePaf-RE) | Reverse-engineered ScePaf headers and export lists, used at build time. `shell/paf_stubs/ScePafRE.yml` is generated from its export lists; `shell/paf_compat/` carries a few of its headers with GCC fixes. | No licence stated |
| [vitasdk](https://vitasdk.org) | Toolchain, headers and stubs. | Various (MIT, LGPL and others per component) |
| [acdc](https://github.com/isage/acdc) by isage | CXML compiler used to build the RCO (build-time tool, not redistributed). | See its repository |

The Steam Deck, RTSS and NVIDIA option names referenced in the documentation are used to
explain the options; no code from those products is involved.
