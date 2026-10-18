# Compatibility list

Games known to run correctly at the limit shown. fpsvita changes how many vblanks a game waits
per frame, or adds a wait when the game has none. A game that advances its world a fixed step
per frame runs too fast above its own rate and in slow motion below it; those are not listed.
An entry vouches for its limit only, not for the others: Jak and Daxter has a VitaGrafix
FPS patch of this kind and still runs at the wrong speed uncapped. If a game is not here, or
you want a different limit, Default is the safe setting. See the [guide](GUIDE.md) for the rest.

## Played

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

## VitaGrafix FPS patches

VitaGrafix ships an FPS patch for these games that does only what fpsvita does from outside:
change the vblank count (`vblank`) or add a wait after the flip (`wait`, what fpsvita calls
Vsync). The limit is the patch's default, which its author lists with no known issues. Not
played with fpsvita yet.

| Title | Title IDs | Type | Limit | Patch |
|---|---|---|---|---|
| Assassin's Creed III: Liberation | PCSB00074, PCSE00053, PCSG00116 | Retail | 30 | vblank |
| Dragon Quest Builders | PCSB00981, PCSE00912, PCSG00697, PCSH00221 | Retail | 60 | wait |
| Kidou Senshi Gundam Seed: Battle Destiny | PCSG00040 | Retail | 30 | vblank |
| Mahouka Koukou no Rettousei: Out of Order | PCSG00456 | Retail | 30 | vblank |
| Oddworld: Munch's Oddysee HD | PCSB00275, PCSE00369 | Retail | 30 | vblank |
| Project Root | PCSB00650, PCSE00486, PCSG00783 | Retail | 30 | vblank |
| Puella Magi Madoka Magica: The Battle Pentagram | PCSG00214 | Retail | 30 | vblank |
| Sly Cooper and the Thievius Raccoonus | PCSA00095, PCSA00096, PCSF00269, PCSF00338 | Retail | 30 | vblank |
| Sly Cooper 2: Band of Thieves | PCSA00095, PCSA00097, PCSF00270, PCSF00338 | Retail | 30 | vblank |
| Sly Cooper 3: Honor Among Thieves | PCSA00098, PCSF00271 | Retail | 30 | vblank |
| Sly Cooper: Thieves in Time | PCSA00068, PCSF00156, PCSF00206, PCSF00207, PCSF00208, PCSF00209 | Retail | 30 | vblank |
| Spy Hunter | PCSB00166, PCSE00068 | Retail | 30 | vblank |
| The Amazing Spider-Man | PCSB00428, PCSE00333 | Retail | 60 | wait |
| Ukiyo no Roushi | PCSG00480 | Retail | 30 | vblank |
| Ys Origin | PCSB01081, PCSE01033, PCSH10049 | Retail | 30 | vblank |

## Latency options

Antilag and Lowlat do not change the frame rate (unchanged in every pair measured), so they
are listed separately from the limits above. Input is the overlay's INPUT row: the age of the
frame's newest controller sample when that frame is flipped to the screen, measured on the same
content with the option off and on. How these were measured:
[latency-measurements.md](latency-measurements.md).

| Title | Title ID | Type | Rate | Antilag | Lowlat |
|---|---|---|---|---|---|
| Metal Gear Solid 2 HD | PCSE00078 | Retail | Own 30 | 40 to 16 ms (menu), 32 to 16 ms (cutscene) | No change |
| Oddworld: Stranger's Wrath HD | PCSE00043 | Retail | Own 30 | 23 to 9 ms | |
| The Wolf Among Us | PCSE00352 | Retail | Own 30 | 56 to 42 ms | Within noise |
| The Swapper | PCSE00387 | Retail | Limit 30 | 49 to 35 ms | |
| Dead or Alive 5 Plus | PCSB00296 | Retail | Own 60 | 49 to 38 ms (menu), 35 to 33 ms (gameplay) | 38 to 20 ms with Antilag (menu) |
| Galaxy on Fire 2 | GALAXYOF2 | Homebrew | Own | 21 to 16 ms | |
| Doom64EX | DOOM64EX1 | Homebrew | Own 60 | 34 to 27 ms | Cannot apply (graphics set up outside the hooked call) |

An empty Lowlat cell means not measured. Lowlat only helps a game whose QUEUE row sits near a
whole frame, and takes effect at the game's next launch.

VitaGrafix patches: [VitaGrafixPatchlist](https://github.com/Electry/VitaGrafixPatchlist).
Reports of games that misbehave, or that run correctly at a limit, are welcome as issues: title
id, name, the limit, and what happened.

## Probably incompatible

Reported to run at the wrong speed, or to break otherwise, when the frame rate changes. Most
reports are of VitaGrafix FPS patches or VGi's v-sync switch (the same as Uncapped); a game
that runs too fast above its own rate is very likely slow motion below it. Keep these on
Default. Confidence: **dev** a developer or patch author, **user** a direct report, **list** a
line in a compiled list.

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
| Gravity Rush | PCSA00011, PCSF00024, PCSC00002, PCSD00003 | Faster uncapped | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-1575445707) | user |
| Hatsune Miku: Project DIVA f / F 2nd | PCSE00326, PCSB00419, PCSG00074, PCSE00434, PCSB00554, PCSG00205, PCSH00088 | At 60: menus double speed, held notes fail | [VG#120](https://github.com/Electry/VitaGrafix/issues/120), [VG#80](https://github.com/Electry/VitaGrafix/issues/80) | dev |
| Hatsune Miku: Project DIVA X | PCSE00867, PCSB01007, PCSG00683, PCSH00176 | Faster uncapped; videos break at 60 | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463484080), [VG#13](https://github.com/Electry/VitaGrafix/issues/13#issuecomment-835914415) | dev |
| J-Stars Victory VS / VS+ | PCSG00300, PCSE00595, PCSB00713, PCSH00136 | Faster uncapped | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463484080) | user |
| Jak and Daxter: The Precursor Legacy | PCSA00080, PCSF00247, PCSF00248 | Faster uncapped, slow motion below 30 | [VG#35](https://github.com/Electry/VitaGrafix/issues/35#issuecomment-414147836) | dev |
| Jet Set Radio | PCSE00088, PCSB00136, PCSG00104, PCSH00022 | Faster uncapped | [VG#69](https://github.com/Electry/VitaGrafix/issues/69#issuecomment-417789931) | user |
| Killzone: Mercenary | PCSA00107, PCSF00243, PCSF00403, PCSC00045, PCSD00071 | No input above 59 fps | [VG#66](https://github.com/Electry/VitaGrafix/issues/66) | dev |
| Limbo | PCSE00268, PCSB00336, PCSG00208 | Faster uncapped | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463544013) | dev |
| LittleBigPlanet PS Vita | PCSA00017, PCSF00021, PCSC00013, PCSD00006 | Uncapped: dialogue cut by double-speed parts | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463427108) | user |
| Metal Gear Solid 2 / 3 HD | PCSE00078, PCSE00079, PCSB00177, PCSB00176, PCSG00080, PCSG00081, PCSE00020, PCSB00118 | Double speed at 60 (MGS2 measured here too) | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-1575445707) | user |
| Miracle Girls Festival | PCSG00610 | Faster above 30, slower below | [VGPL#37][37] | list |
| Need for Speed: Most Wanted | PCSE00089, PCSB00183, PCSG00106 | Speed follows the frame rate | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463544013) | dev |
| New Little King's Story | PCSE00066, PCSB00109 | Double speed at 60 | [VG#255](https://github.com/Electry/VitaGrafix/issues/255#issuecomment-2452948266) | user |
| Ninja Gaiden Sigma 2 Plus | PCSE00233, PCSB00294, PCSG00157 | Faster uncapped, slower when it drops | [VG#53](https://github.com/Electry/VitaGrafix/issues/53#issuecomment-419599728) | dev |
| One Piece: Burning Blood | PCSE00808, PCSB00893, PCSG00743, PCSH00202 | Faster uncapped (one ambiguous report says normal) | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463484080) | user |
| One Piece: Pirate Warriors 3 | PCSE00638, PCSB00717, PCSG00544, PCSH00145 | Faster uncapped | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-1575728215) | user |
| One Piece: Unlimited World Red | PCSE00423, PCSB00557, PCSH00071 | Faster uncapped | [VGPL#37][37] | list |
| Persona 3 / 4 / 5 Dancing | PCSE01274, PCSE00764, PCSE01275 and other regions | Uncapped: menus double speed | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-589908404) | user |
| Persona 4 Golden | PCSE00120, PCSB00245, PCSG00004, PCSH00021 | Double speed uncapped | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463544013) | dev |
| Ragnarok Odyssey ACE | PCSE00300, PCSB00471, PCSG00140, PCSH00044 | Double speed at 60, slower at 20 | [VG#158](https://github.com/Electry/VitaGrafix/issues/158#issuecomment-449603415) | dev |
| Ratchet & Clank Collection | PCSA00133, PCSF00482, PCSF00484, PCSF00485, PCSF00486 | Double speed at 60 | [VGPL#37](https://github.com/Electry/VitaGrafixPatchlist/issues/37#issuecomment-463544013) | dev |
| Resogun | PCSA00103, PCSF00262, PCSC00088 | Double speed at 60 | [VGPL#43](https://github.com/Electry/VitaGrafixPatchlist/issues/43#issuecomment-471982941) | dev |
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
| Ys: Memories of Celceta | PCSE00245, PCSB00497, PCSG00105, PCSH00181 | Faster uncapped | [VGPL#37][37] | list |

Reviewers also saw these slow down, rather than drop frames, when they miss their own rate, so
a lower limit will probably play as slow motion: Catherine: Full Body (PCSG01179), Dead or Alive
5 Plus below its 60 (PCSE00235, PCSB00296, PCSG00167), Kamen Rider Battride War Sousei, World of
Final Fantasy. Source: [Vita Game Performance](https://vitagameperformance.wordpress.com/).

[37]: https://github.com/Electry/VitaGrafixPatchlist/issues/37
