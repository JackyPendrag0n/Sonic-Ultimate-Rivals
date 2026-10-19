# sr2recomp: native Linux port of Sonic Rivals 2 (PSP, ULUS10323) by static recompilation

Built on the rcsm (Ratchet & Clank: Size Matters) recompiler and runtime: the game's Allegrex code is
translated ahead of time into C (`tools/recomp.py`) and runs on a small runtime that re-implements the
PSP OS calls. GPU, SAS/ATRAC audio, MPEG and the system dialogs come from PPSSPP components
(`runtime/gpu`, `runtime/audio`, `runtime/mpeg`, `runtime/utility`). No game data is included.
`README.rcsm.md` is the original Size Matters README, kept for the runtime's history.

## Status
- Boots, shows the SEGA logo, title, menus and story cutscenes, and plays a race (Blue Coast Act 1).
- Sound works: music, effects and voices go through the new sceAudio channel mixer (`runtime/audio_ch.c`).
- Races run at 30 fps on the PSP (menus at 60). Gameplay is timed by the clock, so the race frame rate
  is a setting: 30 (original), 60 (default), 120, 144, 165 or 240.
- PC graphics settings are in the game's own Options screen: a fifth item, **Graphics**, turns the list
  into Resolution, Frame Rate, Fullscreen, V-Sync and Filtering (left/right or CROSS change a row,
  CIRCLE goes back). F1 / Esc opens the same settings as an overlay, plus window and audio options.
  Settings are saved to `~/.config/sr2recomp/settings.ini`; saves go to `~/.local/share/sr2recomp`.
- Wireless Play works, and two players can race on one PC in split screen (see below).

## Controls
Controller: left stick / d-pad move, A jump, B/X/Y the PSP's circle/square/triangle, LB/LT = L,
RB/RT = R, Start/Back. Keyboard: WASD or arrows move, Space or K jump, L circle, J square, I triangle,
Q/E = L/R, Enter Start, Backspace Select, F1/Esc PC settings, F9 start/stop player 2 (split screen), F10 rip the 3D scene
to OBJ, F11 fullscreen, F12 screenshot.

## Build and run
You need your own game files laid out like the UMD (`PSP_GAME/SYSDIR`, `PSP_GAME/USRDIR`), with
`PSP_GAME/SYSDIR/EBOOT.BIN` replaced by the **decrypted** EBOOT (a plain ELF). The Sony modules in
`USRDIR/MODULES` are encrypted and aren't needed.

```sh
./build.sh /path/to/game     # folder that contains PSP_GAME
build/sr2recomp /path/to/game
```
Dependencies are the same as rcsm (gcc/g++, SDL2, FFmpeg, libpng, zlib dev packages).

## Two players on one PC (split screen)
Press **F9** (or F1 > 2 Players, or start with `--2p`) and a second copy of the game opens in the right half of the
window (or the bottom half: F1 > 2 Players > Layout). The two copies race each other through the game's own
Wireless Play, exactly like two PSPs: both choose Multiplayer > Wireless Play, player 1 picks **Create** and sets up
the race, player 2 picks **Join**, chooses player 1's game and then a character. With two controllers each player
has one; with one controller, player 1 uses the keyboard and player 2 the controller. Player 2 has its own save
data in `<saves>/player2` (copied from player 1's the first time) and uses player 1's mods. Player 2's copy is muted,
so the music isn't heard twice.

How it works: `runtime/net.c` implements the PSP's ad hoc stack (sceNetAdhocctl, PDP sockets and the Matching
lobby protocol) over UDP. Each copy is a "PSP" with MAC `02:53:52:32:00:<n>` on UDP port 47320+n-1 on 127.0.0.1 and
finds the others by port. `runtime/splitscreen.c` starts the second copy with a hidden window and shares its frames
and player 2's input through shared memory. `SR2_NET_PEERS=host[:port],...` (with `SR2_NET_ID=n` set differently on
each PC) also sends the traffic to copies on other machines; that LAN path is untested.

## Mods

Put each mod in its own folder under `mods/` next to `PSP_GAME` (or under `~/.local/share/sr2recomp/mods/`;
`SR2_MODS=dir` uses that folder instead of both). Mods load in name order and a later mod wins when two
replace the same file. Rename a mod's folder to start with `_` or `.` to turn it off.

- **Game files.** A mod mirrors the disc: `mods/mymod/PSP_GAME/USRDIR/TEXT/STR_EN_NA.TXT` replaces the game's
  text. A mod with no `PSP_GAME` folder is read relative to `USRDIR`, so `mods/mymod/TEXT/STR_EN_NA.TXT` works
  too. Replaced files may be larger than the originals: files the game reads by disc sector are given new
  sectors past the end of the disc.
- **Textures.** Run once with `SR2_TEX_DUMP=1` and every texture the game draws is written to
  `~/.local/share/sr2recomp/texdump/` as `<16 hex digits>.png`. Edit one and put it in `mods/mymod/textures/`
  with the same name. It can be any size, so 2x or 4x redraws are drawn at full detail with the OpenGL
  renderer. The names hash the texture's pixels and palette, not where it sits in memory, so they stay the
  same between runs.

- **Models and levels.** Press F10 in game (OpenGL renderer) and the next frame's 3D scene is written to
  `~/.local/share/sr2recomp/rips/rip_<n>/` as `scene.obj` + `scene.mtl` with every texture it uses as PNG,
  ready to open in Blender. Each draw is its own object, posed and in world space; the PNG names are the
  same as the texture dump names, so a texture you edit from a rip can go straight into `textures/`.

The log lists the enabled mods and the files and textures they replace.

## Changes from rcsm
- `tools/recomp.py`: jump tables built as `lui/addiu base; addu; lw 0(p)` (GCC 3.3 style) are found
  (all 253 in this game), and `.init` is merged into the code range across alignment padding.
- `runtime/io.c`: raw sector reads (`disc0:/sce_lbn0x..._size0x...`). The game finds a file's start
  sector through `sceIoGetstat` (`st_private[0]`) and reads STREAMS by sector, so the runtime carries the
  disc's sector map (`runtime/lbnmap_ulus10323.inc`, generated by `tools/lbnmap.py <iso>`) and maps
  those reads back to files in the extracted folder. `sceIoIoctl(0x01020004)` returns the same sectors.
  Device names are matched case-insensitively (the game uses `DISC0:`).
- `runtime/hle_misc.c`: sceRtcGetCurrentTick, sceUmdCheckMedium, sceUmdWaitDriveStatWithTimer,
  UMD callbacks, scePowerSetClockFrequency.
- Memory stick size capped at 1 GB (`runtime/utility/shim/Core/Config.h`): the game multiplies free
  clusters by the cluster size in 32 bits, so 16 GB wrapped to 0 and it asked to delete saves.
- `runtime/audio_ch.c`: sceAudio hardware channels (ChReserve, Output[Panned][Blocking], volumes), mixed on one timeline. `SR2_AUDIO_DUMP=file` writes the mixed output as raw 44.1 kHz stereo.
- `runtime/game.c` replaces the Size Matters mods with this game's layer: the settings file, the race
  frame rate (an entry hook on the display manager's swap-interval setter, see `data/hooks.json`, plus a
  faster emulated vblank above 60 fps), and the Graphics entry in the Options screen. The frontend is
  data-driven (0x420-byte widgets baked into the menu data at fixed addresses, text by index into
  `STR_EN_NA.TXT`), so the runtime adds a fifth widget to the Options list and appends its strings to the
  game's text table.
- The Size Matters debug menu, mods, control schemes and test scripts are gone.

## Headless testing
`./run.sh <seconds> <log>` runs under Xvfb. Useful environment variables:
`SR2_LOG=2|3`, `SR2_INPUT="300:CROSS,308:-,..."` (scripted input by vblank),
`SR2_WINSHOT_EVERY=N SR2_SHOT_DIR=dir` (screenshots), `SR2_FPS=120`, `SR2_GL_SCALE=N`,
`SR2_2P=1` (start player 2), `SR2_CTL=fifo` (live control: `press`, `press2` for player 2, `shot`,
`winshot`, `dump` commands, see `runtime/frontend.c`), `SR2_DETERMINISTIC=1` (virtual clock: the game runs as if the host were infinitely fast, which is how
the high frame rates were checked under the software GL here), `SR2_LBN_FATAL=1` (stop with a guest
backtrace when a sector read misses). `tools/watchbuild.sh sr2` builds `build/sr2recomp_watch` for
`SR2_WATCH=lo-hi` memory watches.
