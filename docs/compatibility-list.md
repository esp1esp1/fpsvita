# Compatibility list

For players choosing settings for a game, and for testers adding results. It lists what is
known per title: measured latency gains, frame limits that work, and limits that break a
game. A title that is not listed is simply untested.

## How to read this list

The list has three parts:

1. [Latency results](#latency-results): Antilag and Lowlat measured on a title. These
   options keep the game at its own rate, so they apply whatever limit you use.
2. [Limits known to work](#limits-known-to-work): a limit that plays at the right speed.
   **Each entry vouches for its limit only.** Jak and Daxter is fine held at 30 and still
   runs at double speed uncapped.
3. [Probably incompatible](#probably-incompatible): limits that break a game, mostly
   because its speed follows the frame rate (see
   [why some games break](GUIDE.md#why-some-games-break-at-another-frame-rate)). The quick
   menu marks these with `(!)` and the overlay's TIP row names the problem.

`Default` leaves the game's own pacing alone and is safe for every title.

Each entry in the incompatible table carries a confidence level:

| Confidence | Meaning |
|---|---|
| played | Checked on the author's Vita with fpsvita |
| dev | Reported by a developer or patch author |
| user | A direct report from a player |
| list | A line in a compiled list |

## Adding a game

Report it through an issue, as described in
[Reporting a game](GUIDE.md#reporting-a-game). A report enters the list with `user`
confidence.

Say whether the game's speed looked right by eye. An FPS reading alone cannot show a
speed-tied game: it reaches 60 fps and runs at double speed.

## Latency results

Input to screen as the overlay's INPUT row reports it, option off then on. Method and the
full results are in [latency measurements](latency-measurements.md).

| Title | Title ID | Type | Rate | Antilag | Lowlat |
|---|---|---|---|---|---|
| Metal Gear Solid 2 HD | PCSE00078 | Retail | Own 30 | 40 to 16 ms (menu), 32 to 16 ms (cutscene) | No change |
| Oddworld: Stranger's Wrath HD | PCSE00043 | Retail | Own 30 | 23 to 9 ms | |
| The Wolf Among Us | PCSE00352 | Retail | Own 30 | 56 to 42 ms | Within noise |
| The Swapper | PCSE00387 | Retail | Limit 30 | 49 to 35 ms | |
| Dead or Alive 5 Plus | PCSB00296 | Retail | Own 60 | 49 to 38 ms (menu), 35 to 33 ms (gameplay) | 38 to 20 ms with Antilag (menu) |
| Galaxy on Fire 2 | GALAXYOF2 | Homebrew | Own | 21 to 16 ms | |
| Doom64EX | DOOM64EX1 | Homebrew | Own 60 | 34 to 27 ms | Cannot apply (graphics set up outside the hooked call) |

An empty Lowlat cell means not measured, not no effect.

## Limits known to work

### Played with fpsvita

| Title | Title IDs | Type | Limit |
|---|---|---|---|
| Assassin's Creed Chronicles | PCSE00700 | Retail | 30 |
| Futuridium EP Deluxe | PCSE00495 | Retail | 60 |
| God of War Collection | PCSA00126, PCSC00059, PCSF00438 | Retail | 30 |
| Grim Fandango Remastered | PCSE00547 | Retail | 30, 20 |
| Horizon Chase Turbo | PCSH10315 | Retail | 30 |
| I am Setsuna | PCSG00756 | Retail | 30 |
| Oddworld: Stranger's Wrath HD | PCSE00043 | Retail | 30 |
| The Swapper | PCSE00387 | Retail | 30 |
| The Walking Dead | PCSE00316 | Retail | 30 |
| The Walking Dead: Season Two | PCSE00372 | Retail | 30 |
| The Wolf Among Us | PCSE00352 | Retail | 30 |
| Babel Rising 3D | BABEL003D | Homebrew | 30 |
| devilutionX | DVLX00001 | Homebrew | 30 |
| Door Kickers: Action Squad | DKAS00000 | Homebrew | 30 |
| Fallout 2 CE | FOUT00002 | Homebrew | 60 |
| Flipper Volcano | FLPR12345 | Homebrew | 30 |
| HELP NO BRAKE | HELP04040 | Homebrew | 30 |
| PAKO Forever | GEOR20066 | Homebrew | 30 |
| Pony Island | PONY22412 | Homebrew | 30 |
| Rocky Castle 2 | TOMCASTLE | Homebrew | 30 |
| The Dark Knight Rises | KRAS00000 | Homebrew | 30 |
| The Simpsons: Hit & Run | ROADRAGE2 | Homebrew | 30 |
| Thimbleweed Park | THIMBPARK | Homebrew | 30 |
| VitaHOT | VTHT52118 | Homebrew | 30 |
| vitaRTCW | RTCW00001 | Homebrew | 30 |

### From patch lists

These titles have a frame-rate patch in VitaGrafixPatchlist, so the limit is known to play
at the right speed. None has been measured with fpsvita yet. The author played several of
them with the patch at that limit.

The Patch column says what the patch does:

- `vblank`: writes a vblank count into the game. An fpsvita limit sets the same count from
  outside.
- `wait`: adds a two-vblank wait after each flip. That is what fpsvita's limits and
  [Vsync](GUIDE.md#vsync) do from outside.

| Title | Title IDs | Own rate | Limit | Patch |
|---|---|---|---|---|
| Assassin's Creed III: Liberation | PCSB00074, PCSE00053, PCSG00116 | 30 | 60 | vblank |
| Dragon Quest Builders | PCSB00981, PCSE00912, PCSG00697, PCSH00221 | uncapped | 30 | wait |
| God of War Collection | PCSA00126, PCSC00059, PCSF00438 | 30 | 60 | vblank |
| I am Setsuna | PCSG00756 | 30 | 60 | vblank |
| Kidou Senshi Gundam Seed: Battle Destiny | PCSG00040 | 30 | 60 | vblank |
| Lumines: Electronic Symphony | PCSB00061, PCSE00009, PCSG00014 | 30 | 60 | vblank |
| Mahouka Koukou no Rettousei: Out of Order | PCSG00456 | 30 | 60 | vblank |
| Minecraft: PlayStation Vita Edition | PCSB00560, PCSE00491, PCSG00302 | uncapped | 30 | wait |
| Oddworld: Munch's Oddysee HD | PCSB00275, PCSE00369 | 30 | 60 | vblank |
| Project Root | PCSB00650, PCSE00486, PCSG00783 | 30 | 60 | vblank |
| Puella Magi Madoka Magica: The Battle Pentagram | PCSG00214 | 30 | 60 | vblank |
| Sly Cooper and the Thievius Raccoonus | PCSA00095, PCSA00096, PCSF00269, PCSF00338 | 30 | 60 | vblank |
| Sly Cooper 2: Band of Thieves | PCSA00095, PCSA00097, PCSF00270, PCSF00338 | 30 | 60 | vblank |
| Sly Cooper 3: Honor Among Thieves | PCSA00095, PCSA00098, PCSF00271, PCSF00338 | 30 | 60 | vblank |
| Sly Cooper: Thieves in Time | PCSA00068, PCSF00156, PCSF00206, PCSF00207, PCSF00208, PCSF00209 | 30 | 60 | vblank |
| Spy Hunter | PCSB00166, PCSE00068 | 30 | 60 | vblank (listed as playable, no issue filed) |
| SteamWorld Heist | PCSB00693, PCSE00583 | 30 | 60 | vblank |
| The Amazing Spider-Man | PCSB00428, PCSE00333 | uncapped | 30 | wait |
| Ukiyo no Roushi | PCSG00480 | 30 | 60 | vblank |
| Ys Origin | PCSB01081, PCSE01033, PCSH10049 | 30 | 60 | vblank |
| Ys VIII: Lacrimosa of Dana | PCSB01128, PCSE01103, PCSG00881, PCSH00297 | 30 | 60 | vblank |

Left out: patches that change something inside the game that fpsvita cannot reach, such as
Asphalt: Injection's millisecond timer, IA/VT Colorful's target-fps constant and the pending
Project Diva 60 fps timestep patches.

Some titles with a patch are still in the incompatible table below: Jak and Daxter,
Killzone: Mercenary (it needs `read=peek`), Project Diva f / f 2nd and WipEout 2048.

## Probably incompatible

| Title | IDs | What breaks | Source | Confidence |
|---|---|---|---|---|
| Asterisk War: Phoenix Festa | PCSH00249 | Faster uncapped | [VGPL#37][37] | list |
| Attack on Titan: Wings of Freedom | PCSE00812, PCSB00898, PCSG00762, PCSH00253 | Faster uncapped | [VGPL#37][37] | user |
| Batman: Arkham Origins Blackgate | PCSE00269, PCSB00353, PCSG00261 | Faster uncapped | [VGPL#37][37] | list |
| Berserk and the Band of the Hawk | PCSE01034, PCSB01085, PCSH10025 | Double speed at 60 | [VG#265](https://github.com/Electry/VitaGrafix/issues/265#issuecomment-2481259314) | dev |
| Chaos Rings, Omega, II | PCSG00457, PCSG00497, PCSG00498, PCSG00499 | Faster at 30 than its own 20 | [VG#6](https://github.com/Electry/VitaGrafix/issues/6#issuecomment-412756984), [VG#107](https://github.com/Electry/VitaGrafix/issues/107) | dev |
| Child of Light | PCSE00480, PCSB00598, PCSG00408 | Faster uncapped | [VGPL#37][37] | list |
| Croixleur Sigma | PCSE00689, PCSB00767, PCSG00537 | Faster uncapped | [VGPL#37][37] | list |
| Dead or Alive Xtreme 3: Venus | PCSH00250, PCSG00771 | Faster uncapped | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-1636723263) | user |
| Deception IV: The Nightmare Princess | PCSE00743, PCSB00829, PCSG00565 | Faster uncapped | [VGPL#37][37] | list |
| Dragon Ball Z: Battle of Z | PCSE00305, PCSB00396, PCSG00213 | Faster uncapped | [VGPL#37][37] | list |
| Dynasty Warriors Next | PCSE00014, PCSB00089, PCSG00002, PCSH00003 | Faster uncapped | [VGPL#37][37] | list |
| Final Fantasy X HD Remaster | PCSE00293, PCSB00395, PCSG00219, PCSH00067 | Faster uncapped | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-1575445707) | user |
| Final Fantasy X-2 HD Remaster | PCSE00283, PCSB00394, PCSG00220 | Faster at 60 | [VG#5](https://github.com/Electry/VitaGrafix/issues/5#issuecomment-1465163821) | dev |
| Freedom Wars | PCSA00147, PCSF00446, PCSC00054, PCSD00087 | Faster at 60 | [VG#46](https://github.com/Electry/VitaGrafix/issues/46#issuecomment-1218562019) | dev |
| Girls und Panzer: Senshadou, Kiwamemasu! | PCSG00339 | Slow motion below 30 | [VGPL#45](https://github.com/Electry/VitaGrafixPatchlist/issues/45#issuecomment-583486016) | dev |
| Gravity Rush | PCSA00011, PCSF00024, PCSC00002, PCSD00003 | Faster uncapped (37 fps uncapped here) | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-1575445707) | user |
| Hatsune Miku: Project DIVA f / F 2nd | PCSE00326, PCSB00419, PCSG00074, PCSE00434, PCSB00554, PCSG00205, PCSH00088 | At 60: menus double speed, held notes fail | [VG#120](https://github.com/Electry/VitaGrafix/issues/120), [VG#80](https://github.com/Electry/VitaGrafix/issues/80) | dev |
| Hatsune Miku: Project DIVA X | PCSE00867, PCSB01007, PCSG00683, PCSH00176 | Faster uncapped; videos break at 60 | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463484080), [VG#13](https://github.com/Electry/VitaGrafix/issues/13#issuecomment-835914415) | dev |
| J-Stars Victory VS / VS+ | PCSG00300, PCSE00595, PCSB00713, PCSH00136 | Faster uncapped | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463484080) | user |
| Jak and Daxter: The Precursor Legacy | PCSA00080, PCSF00247, PCSF00248 | Faster uncapped (48 fps here), slow motion below 30 | [VG#35](https://github.com/Electry/VitaGrafix/issues/35#issuecomment-414147836) | dev |
| Jak II: Renegade | PCSA00080, PCSF00247, PCSF00248 | Normal speed at 60 (56 fps here) but the game freezes after about a minute; presentation continues, input is dead, only a relaunch recovers | measured here | played |
| Jet Set Radio | PCSE00088, PCSB00136, PCSG00104, PCSH00022 | Faster uncapped | [VG#69](https://github.com/Electry/VitaGrafix/issues/69#issuecomment-417789931) | user |
| Killzone: Mercenary | PCSA00107, PCSF00243, PCSF00403, PCSC00045, PCSD00071 | No input above 59 fps unless the title's `state.txt` line has `read=peek` | [VG#66](https://github.com/Electry/VitaGrafix/issues/66) | played |
| Limbo | PCSE00268, PCSB00336, PCSG00208 | Faster uncapped | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463544013) | dev |
| LittleBigPlanet PS Vita | PCSA00017, PCSF00021, PCSC00013, PCSD00006 | Uncapped: dialogue cut by double-speed parts | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463427108) | user |
| Metal Gear Solid 2 / 3 HD | PCSE00078, PCSE00079, PCSB00177, PCSB00176, PCSG00080, PCSG00081, PCSE00020, PCSB00118 | Double speed at 60 (MGS2 confirmed here; 39 and 47 fps uncapped) | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-1575445707) | played |
| Miracle Girls Festival | PCSG00610 | Faster above 30, slower below | [VGPL#37][37] | list |
| Need for Speed: Most Wanted | PCSE00089, PCSB00183, PCSG00106 | Speed follows the frame rate | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463544013) | dev |
| New Little King's Story | PCSE00066, PCSB00109 | Double speed at 60 | [VG#255](https://github.com/Electry/VitaGrafix/issues/255#issuecomment-2452948266) | user |
| Ninja Gaiden Sigma 2 Plus | PCSE00233, PCSB00294, PCSG00157 | Faster uncapped (36 fps here), slower when it drops; a 60 limit gives 28 | [VG#53](https://github.com/Electry/VitaGrafix/issues/53#issuecomment-419599728) | dev |
| One Piece: Burning Blood | PCSE00808, PCSB00893, PCSG00743, PCSH00202 | Faster uncapped (one ambiguous report says normal) | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463484080) | user |
| One Piece: Pirate Warriors 3 | PCSE00638, PCSB00717, PCSG00544, PCSH00145 | Faster uncapped | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-1575728215) | user |
| One Piece: Unlimited World Red | PCSE00423, PCSB00557, PCSH00071 | Faster uncapped | [VGPL#37][37] | list |
| Persona 3 / 4 / 5 Dancing | PCSE01274, PCSE00764, PCSE01275 and other regions | Uncapped: menus double speed | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-589908404) | user |
| Persona 4 Golden | PCSE00120, PCSB00245, PCSG00004, PCSH00021 | Double speed uncapped; ignores a 60 limit (stays at 30 here, it paces itself) | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463544013) | dev |
| Ragnarok Odyssey ACE | PCSE00300, PCSB00471, PCSG00140, PCSH00044 | Double speed at 60, slower at 20 | [VG#158](https://github.com/Electry/VitaGrafix/issues/158#issuecomment-449603415) | dev |
| Ratchet & Clank Collection | PCSA00133, PCSF00482, PCSF00484, PCSF00485, PCSF00486 | Double speed at 60 | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463544013) | dev |
| Resogun | PCSA00103, PCSF00262, PCSC00088 | Double speed at 60 (checked here) | [VGPL#43](https://github.com/Electry/VitaGrafixPatchlist/issues/43#issuecomment-471982941) | played |
| Senran Kagura Bon Appetit! | PCSE00471 | UI and menus run faster above 30; rhythm mode does not hold 60 | [VGPL PR#154](https://github.com/Electry/VitaGrafixPatchlist/pull/154) | user |
| Shinobido 2: Revenge of Zen | PCSE00015, PCSB00049, PCSG00019, PCSH00001 | Double speed at 60 | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37) | dev |
| Sonic & All-Stars Racing Transformed | PCSE00056, PCSB00190 | At 60: menus double speed | [VG#75](https://github.com/Electry/VitaGrafix/issues/75#issuecomment-435598450) | dev |
| Tales of Hearts R | PCSE00429, PCSB00550, PCSG00125 | Faster uncapped | [VGPL#37][37] | list |
| Toukiden 2 | PCSE00940, PCSB01003, PCSG00830, PCSH00292 | At 60: menus double speed | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463484080) | user |
| Trails in the Sky the 3rd Evolution | PCSG00490, PCSH10082 | Double speed at 60 | [VG#260](https://github.com/Electry/VitaGrafix/issues/260#issuecomment-1809164671) | dev |
| Uncharted: Golden Abyss | PCSA00029, PCSF00001, PCSF00012, PCSD00001 | Faster uncapped in one list; a later report says normal at 60 | [VGPL#37][37] | list |
| UPPERS | PCSG00633, PCSH00279 | Faster uncapped | [VGPL#37][37] | list |
| Valkyria Revolution | PCSE01003, PCSB01090, PCSG00962, PCSH10035 | Faster uncapped | [VGPL#37][37] | list |
| Valkyrie Drive: Bhikkhuni | PCSE00948, PCSB01011, PCSG00632 | Faster uncapped | [VGPL#37][37] | list |
| Warriors Orochi 3 Ultimate | PCSE00448, PCSB00575 | Faster uncapped | [VGPL#37][37] | list |
| WipEout 2048 | PCSA00015, PCSF00007, PCSC00006, PCSD00005 | Ship shakes with a cap set | [VGPL#72](https://github.com/Electry/VitaGrafixPatchlist/issues/72) | user |
| Ys: Memories of Celceta | PCSE00245, PCSB00497, PCSG00105, PCSH00181 | Faster uncapped (48 fps here, speed not yet judged) | [VGPL#37][37] | list |

"Here" means measured on the author's Vita.

## Possibly slow below their own rate

Reviewers saw these titles slow down, rather than drop frames, when they missed their own
rate. A lower limit probably plays as slow motion:

- Catherine: Full Body (PCSG01179)
- Dead or Alive 5 Plus below 60 (PCSE00235, PCSB00296, PCSG00167)
- Kamen Rider Battride War Sousei
- World of Final Fantasy

Source: [Vita Game Performance](https://vitagameperformance.wordpress.com/).

## Sources

- Measurements and played entries: one retail OLED Vita (PCH-1000), enso, firmware 3.65,
  October 2026.
- [VitaGrafixPatchlist](https://github.com/Electry/VitaGrafixPatchlist): `patchlist.txt` and
  README as of September 2026, plus open pull requests. Its pull request #158 will add
  per-game markers for an internal FPS lock and for speed tied to frame rate.
- [VitaGrafixPatchlist issue #37][37] and the other VitaGrafixPatchlist (VGPL) and
  [VitaGrafix](https://github.com/Electry/VitaGrafix) (VG) issues linked in each row.
- [Vita Game Performance](https://vitagameperformance.wordpress.com/) reviews.

[37]: https://github.com/Electry/VitaGrafixPatchlist/issues/37
