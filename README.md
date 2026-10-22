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
- High frame rates: a few spots of the game assumed 30 fps steps and are corrected above it (see
  "Frame-rate fixes" below).
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

### Character skins

A skin is an extra character on the Single Event Select Character and Select Rival screens. It races as one
of the eight characters (same moves, stats and code) with its own name, icon, textures, and optionally its
own racer files (model, textures and voice). Skins appear in a third row of icons, up to four of them. Story
mode always uses the game's own characters.

A skin is a folder `mods/<mod>/characters/<skin>/`:

- `character.ini`: `name=Dark Sonic` and `base=sonic` (sonic, shadow, knuckles, silver, metal sonic, tails,
  espio or rouge).
- `icon.png`, and optionally `icon_selected.png` for the highlighted icon. The first run writes
  `icon_template.png` and `icon_template_selected.png` there, the base character's icons, to paint over (any
  size, keep the layout).
- `textures/`: replacement textures named like a mod's `textures/` folder. They apply only while the skin is
  racing. To find the character's textures, dump textures (`SR2_TEX_DUMP=1`) in a race as the base character
  and in a race without it, and keep the ones only the first has.
- `RACER_<BASE>_0.STR` and `RACER_<BASE>_1.STR` (optional): the base character's racer files with the skin's
  model, textures and voice. They must keep the game's size.

`tools/sr2skin.py` (Python 3, nothing to install) starts a skin and swaps its voice:

    tools/sr2skin.py new <game folder> sonic "Dark Sonic" mods/myskins/characters/darksonic
    tools/sr2skin.py sounds mods/myskins/characters/darksonic    # racer files' clips -> sounds/*.wav
    # edit or replace the WAVs (any rate, mono or stereo, 16-bit)
    tools/sr2skin.py voice mods/myskins/characters/darksonic     # edited WAVs -> both racer files

A clip keeps its original length (longer audio is cut). The model inside the racer files has no editor yet.

## Racer files
`STREAMS/RACER_<CHAR>_0.STR` and `_1.STR` (0x166670 bytes each) are raw memory images. The race load copies
one to a fixed address and uses it as is, with no fix-ups: `_0` is made for load slot 14 at 0x08F61670, `_1`
for slot 15 at 0x090C7CF0 (single-player races only load `_1`). The world has 16 load slots of 0x4C0 bytes
at 0x089ABDA0 (slot 0 the level, 1-13 level sections, 14-15 the racers), and a racer file carries its own
copy of its slot header at file offset 0x5430 (+0xD0 root scene node, +0x410 start, +0x438 node count). The
racer object itself is at file offset 0x5900. The race's racer list (0x08A7C69C: count byte, then up to 5
racer pointers) has room for more than two.

The two images hold the same objects in the same order, so lining them up shows every word that depends on
the slot: about 3,600 pointers into the image (some through the uncached mirror), about 900 slot numbers
(mostly byte 2 of `0x0?SS00nn` handles), about 170 pointers to per-slot game objects, and a few pointers into
a per-racer block of the game heap. `tools/racergen.py` uses that to rebuild a character's racer file for
any slot and address; for slot 14 at the `_0` address it reproduces `_0` byte for byte for all eight
characters, and a `_1` rebuilt from `_0` races normally in place of the original.

Experimental: `SR2_EXTRA_RACER=<file>` loads a file made by `racergen.py` (for example slot 13 at
0x0A000000, heap block 0x0A200000) during the race load and builds its scene nodes right after the
game's own racers (hook 5), adding it to world slot 0 as a third racer. On the race's first frame it is
switched on (F_16c0b0), placed next to the local player the way F_159194 places racers (`SR2_EXTRA_DXYZ`
moves it), its computer driver is turned off (F_ade6c) and its input block is set up like a
human racer's (F_17138) with source 1. Its section index is kept current by hook 6. Hook 7 (F_17d58, a
racer's input step) reads source 1 from a second pad device (0x0A1F0000) that is updated from the second
controller (or the `press2` ctl command) when split screen is off. So with two controllers, player 2 runs
the extra Sonic around the same track.

Split view (on with the extra racer; `SR2_SPLIT_VIEW=tb` top and bottom, the default, `lr` side by side,
`0` off): the race camera (0x08A855D4, a 0x66c-byte object plus the 0x7c-byte transform node at +4)
follows the racer at +0x294. A second camera is kept as a copy of that state aimed at the extra racer and
swapped in around the camera step (hook 8, F_10a250) and around the race render's draws (hooks 9-11:
F_15a460 world, F_31300, F_30414), with the view rebuilt as F_581b0 does (camera vt+0x40, F_2ac50) and the
extra racer standing in as the local player (world +0x5024). Each view goes into its half of the screen by
moving the GE viewport centre (XPOS/YPOS) and, for the second, the scissor; the first view is drawn over
the whole screen, since leaving part of the screen empty during it washes out the colours. The HUD is
still player 1's.

Four players: `SR2_EXTRA_RACER` takes up to three files separated by `:`, each built for its own free load
slot and address (Blue Coast leaves slots 9-13 empty; levels with more sections use more of them), e.g.

    tools/racergen.py <game> SONIC 13 0a000000 0a200000 RACER_SONIC_13.STR
    tools/racergen.py <game> SHADOW 12 0a400000 0a600000 RACER_SHADOW_12.STR
    tools/racergen.py <game> KNUCKLES 11 0a800000 0aa00000 RACER_KNUCKLES_11.STR

Extra racer k reads controller k+2 (or the `press2`..`press4` ctl commands) through its own pad device
(0x0A1F0000 + k*0x100) and gets its own camera; the screen splits in four. The race has two seats (world
+0x502C/+0x5030) and only steps seated racers, so the extra racers are stepped after them with seat 0 lent
to them (hook 12, F_b2e48). None keeps a seat: with player 2 in Free Play's empty rival seat, player 1
could not move. The race only starts the seated racers: the race state change
(hook 13, F_592cc) sends its events (F_16c388, hook 14) to the two seats, so the ones the local player
gets are passed on to the extra racers without a seat.

Without `SR2_EXTRA_RACER`, the PC settings (F1) tab "Split screen" sets this up: the player count
(`mp_players`) and the characters of players 2-4 (`mp_char2`..`mp_char4`). During the race load the runtime
builds each racer file itself (runtime/racergen.c, the same rules as `racergen.py`, byte for byte) for the
highest load slot the level leaves empty, at 0x0A000000 + k*4 MB with its heap block 2 MB in. The debug
switch `pad_all` makes every player read controller 1 and the keyboard, and `mp_test_seats` lets player 1 seat
players without a controller on the Multiplayer screen (Y adds one, X removes one). Test aids: `SR2_P1_CPU=1` hands
player 1 to the computer too, `SR2_VOICE_TRACE`, `SR2_SMART_TRACE` and `SR2_UNSTICK_TRACE` log what they say.

Before the start every player gets the story races' banter: a camera shot of them saying a line (race states
3 and 4 of F_597d8). The check for it (F_81944, hook 30) says yes, the racer each shot asks for (F_3731c,
hook 31) is the next player, and the end of a shot goes back to state 3 until everyone has had one. The
results title ("Sonic Wins") names the winner: player 1's entries in the Wins / Loses string tables
(0x0899C7D4 / 0x0899C7F4) hold the winner's from the end of the race.

Each view also gets its own HUD: the HUD (F_12ad44, hook 15) is drawn once per view, scaled to half size
into it, with that player as the local player (the boost bar reads it as it draws) and that player's rings
(racer +0x72C) put into the HUD's ring count (HUD +0x30) for the draw. The timer and the race meter are
shared: the views' passes leave out those elements (hooks 17-21 filter what each pass draws), and the meter and
timer are drawn once for the whole screen, the timer moved down to just above the meter. Every player gets an
icon on the meter: player 1's icon element (HUD +0x1600) is drawn again per player, moved by the difference in
race progress (F_B070C) and showing that character's head (the HUD's "head ..." textures). Text the elements
queue (ring and timer digits) is drawn after each pass (F_12D868), so it lands in that pass's view.

Bosses (Act 4) and battles (Act 2, all six modes) also take 2-4 players. A boss counts its hits per racer
(hooks 32-35): every player has their own six hits and the first to land them wins. A battle keeps its rules in
the battle object (0x08A8561C: mode at +4, a rules object per mode at +8+4*mode), which only knows two racers:
index 1 (the local player) and index 0 (the rival). The rules' calls (start, restart, update and the six
dispatchers F_812a0..F_81858, hooks 36-44) seat the player they are about: the rival stays in index 0, and
index 1 holds player 1 or, for the length of the call, another player, whose score, flags and Tag clock are
kept per player in between. A player who reaches the goal is seated for good, so the game's own end runs for
them; at time-up the results rank everyone by score. King of the Hill's points and contesting and Tag's clock
for an unseated player are worked out per frame (hook 38), and a Chao can be brought to any base (hook 46,
F_103fc0, lends the base to the player). Each view has its own copy of the HUD (hook 45 runs the HUD update
once per view, with that view's player seated), so scores, laps, clocks and heads are per player.

Streaming with players 2-4: a course is loaded a section at a time into four buffers around the current
section (one behind, two ahead), picked from the focus racer (F_15f97c: F_15e0e8 at hook 47, F_1604ec at
hook 48, and F_15e4a8, hook 23). With more players the window follows whoever is in the furthest section, so
the leader's view never runs out of course; a player left outside the window (or fallen through a floor that
went away) for 0.7 s is put back on the course at its rear (`stream_keep_up`). `SR2_STREAM_P1=1` restores the
game's own streaming around player 1.

Computer players: on the Multiplayer screen player 1's Y / triangle fills a seat with one (X / square takes it
out, left / right picks its character); `mp_bots` keeps a bit per player number. A bot is switched on as the
game's computer rival is: its input block reads source -2 (no pad) and its driver flag is set (F_ade6c). Bots
have no view. A bot that has made no headway for 4 s once it got going (stuck at the foot of a loop, in a wall,
or bunched up with another) is moved further along the path, further each time it is stuck again soon after
(`bot_unstick`). The experimental "smarter computer players" option (`mp_smart_ai`) changes how the game's
driver paces itself: it eases off or pushes by how far along it is next to its opponent (F_b1924, hook 50: the
other seat, else the local player), into racer+0x8dc by F_b1a84 (hook 51: 2 well behind, pushes; 1 well
ahead, eases off). With it a bot's opponent is whoever is furthest along of the others, and no computer driver
(bots, and the rival in Story and Single Event races) ever eases off. Each view of a boss shows only its own
player's hits, with their head.

Voices: a voice line (F_127080 -> F_d120, hook 49) is played at the speaker's position and fades with the
distance to the listener (player 1's camera), so far from player 1 the others' lines were not heard (their
death cries above all, F_93fb8 kind 13). In multiplayer the human players' lines play without a position, for
everyone. Races also show each player's place now (1st-4th, `mp_live_place`) in place of the game's own (HUD
+0xB00, which only knows the two seats).

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
- Frame-rate fixes (`runtime/game.c`, hooks 2-4 in `data/hooks.json`):
  - Tails' Tail Copter sets a lift timer to `dt*4` and a starting kick to `dt*30`, which only add up
    to a climb at 30 fps. Hooks 2 and 3 give those two dt loads at least 1/30 s, so at 240 fps he climbs
    about as high as at 30 (3.5 vs 3.3 units; it was 2.6).
  - Racer movement (hook 4 on the move function, active in races above 60 fps): with very small steps
    the collide-and-slide can creep into a corner without ever crossing onto the next surface, and the
    racer stops dead (seen on a Blue Coast ramp at 240 fps). When a racer has moved less than a quarter
    of its expected distance for 3 frames in a row, one step is taken with a 1/60 s dt.
- The Size Matters debug menu, mods, control schemes and test scripts are gone.

## Headless testing
`./run.sh <seconds> <log>` runs under Xvfb. Useful environment variables:
`SR2_LOG=2|3`, `SR2_INPUT="300:CROSS,308:-,..."` (scripted input by vblank),
`SR2_WINSHOT_EVERY=N SR2_SHOT_DIR=dir` (screenshots), `SR2_FPS=120`, `SR2_GL_SCALE=N`,
`SR2_2P=1` (start player 2), `SR2_CTL=fifo` (live control: `press`, `press2` for player 2, `shot`,
`winshot`, `dump`, `poke ADDR hex|float`, `track ADDR` (append those words to `<fifo>.track` every
vblank), `bosshit K` (player K+1 lands a boss hit), `btcall K FN A2 A3 [N]` (call battle rules function
0x08800000+FN for player K+1) commands, see `runtime/frontend.c`), `SR2_UNLOCK_ALL=1` (every stage open), `SR2_MP_CPU=1` (players 2-4 are left to the computer), `SR2_DETERMINISTIC=1` (virtual clock: the game runs as if the host were infinitely fast, which is how
the high frame rates were checked under the software GL here), `SR2_LBN_FATAL=1` (stop with a guest
backtrace when a sector read misses). `tools/watchbuild.sh sr2` builds `build/sr2recomp_watch` for
`SR2_WATCH=lo-hi` memory watches (VFPU stores included). `SR2_GPU=soft SR2_DRAW_EVERY=N` draws only every
Nth frame in the software renderer, which makes long headless runs several times faster (screenshots then
often catch a skipped frame).
