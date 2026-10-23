# rcsm — native Linux port of Ratchet & Clank: Size Matters (PSP, UCES00420) by static recompilation

The game's MIPS (Allegrex) code is translated ahead of time into C and compiled into an x86-64 Linux
executable. The game code is not interpreted or emulated. It runs on a small runtime that
re-implements the PSP operating system calls it uses. The parts of the PSP that are hardware rather
than code use components extracted from PPSSPP: the graphics chip, the SAS/ATRAC audio decoders, the
MPEG video decoder and the system save/message dialogs.

No game data is included. You build it from **your own** copy of the game.

## What you need

- Your game files, laid out like the UMD (`PSP_GAME/SYSDIR`, `PSP_GAME/USRDIR`):
  - `PSP_GAME/SYSDIR/EBOOT.BIN` must be the **decrypted** executable (a plain ELF). The build
    script also accepts a decrypted `BOOT.BIN`.
  - `USRDIR/BIN/*.PRX` and `USRDIR/MODULES/*.PRX`, which ship as plain ELFs on this disc.
  - `USRDIR/CLIPS` holds the FMVs: the logos, the title movie and the planet cutscenes (`01A.PMF`
    is the Pokitaru intro). They play with sound and can be skipped with Start, and file names can be
    any case. The folder is optional: missing videos are replaced by a 1-second black clip so the game
    can continue. The log says why a video didn't play. It might be missing, or unreadable because the
    files belong to root after being extracted with sudo or from a mounted ISO (`chmod -R a+rX CLIPS`
    fixes that).
- Linux x86-64, Python 3, gcc/g++, and the development packages for SDL2, FFmpeg (libavcodec,
  libavformat, libavutil, libswscale, libswresample), libpng and zlib:
  - Debian/Ubuntu: `sudo apt install build-essential pkg-config libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libpng-dev zlib1g-dev`
  - Fedora: `sudo dnf install gcc-c++ pkgconf SDL2-devel ffmpeg-free-devel libpng-devel zlib-devel`
  - Arch: `sudo pacman -S base-devel sdl2 ffmpeg libpng zlib`

## Build and run

```sh
./build.sh /path/to/game        # the folder that contains PSP_GAME
build/rcsm /path/to/game
```

With no folder argument, rcsm looks in the current directory, then in the folder the executable is in.
Paths with spaces work with or without quotes, and the `PSP_GAME` folder itself is accepted too.

The build recompiles 22 modules (~95,000 functions) into C and compiles them. It takes a few
minutes on a multi-core desktop and ~25 minutes on 2 cores. Later builds only redo what changed.
By default it optimises for your CPU (`MARCH=native`). Use `MARCH=x86-64-v2 ./build.sh ...` for a
binary that also runs on older CPUs.

## Controls

| PSP | Keyboard | Controller (Xbox layout) |
|---|---|---|
| Analog stick (move) | W A S D | left stick |
| Cross (jump) | Space or K | A |
| Circle (fire) | L, or **left mouse button** | B, or **R2** |
| Square (wrench) | J | X |
| Triangle (quick select) | I | Y |
| **Aim** (new, modern shooting) | Z or C, or **right mouse button** | **L2** |
| L / R (turn camera) | Q / E, or **move the mouse** | right stick left/right, or LB / RB |
| **Tilt camera** (new) | R / F, or **move the mouse** | right stick up/down |
| D-pad | arrow keys | d-pad |
| Start / Select | Enter / Backspace | Start / Back |
| Settings menu | F1 or Esc | Guide |
| Fullscreen / screenshot | F11 / F12 | |

**Modern shooting (the default).** Plays like the PS4 *Ratchet & Clank* and *Rift Apart*:
- **Hold L2 (or the right mouse button) to aim.** The camera moves in over Ratchet's right shoulder and
  zooms a little, a crosshair appears, and Ratchet draws his gun if he has the wrench out.
- **Ratchet faces where the crosshair points** and strafes while you aim: the left stick moves him in
  any direction without turning him.
- **R2 (or the left mouse button) fires** at the crosshair. The game's own lock-on still helps.
- **Mouse look.** With a mouse, moving it turns and tilts the camera (the pointer is captured while
  you play; F1/Esc releases it). It can be switched off in Controls.
- Switch the **Shooting style** to Classic in Controls to get the original PSP behaviour back. Classic
  mode has the strafe options below.

**Strafing (classic mode).** Hold L2 or R2 (or set it to toggle in the menu) and Ratchet keeps facing
the same way while the left stick moves him in any direction at analog speed, as in *Up Your Arsenal*:
- Pulling back walks him backwards.
- Releasing the trigger returns to normal movement.
- As in the PSP game, strafing works with a gun out, not with the wrench.
- The PSP's own d-pad strafing still works.

How it works: the game has a "Strafe Controls: Analog Stick" option, which makes every stick movement
a strafe. The trigger switches that option on only while it's held. Modern aiming uses the same
option, and turns Ratchet towards the point the crosshair looks at every frame.

## Settings menu (F1 / Esc / Guide)

Opening the menu pauses the game. Changes are saved to `~/.config/rcsm/settings.ini`.

**The same settings are in the game's own pause menu**, under Options > **Camera, Controls & PC**. Press L
or R there (Q/E on the keyboard, LB/RB on a controller) to page through PC: Graphics, PC: Lighting &
Display (including draw distance), PC: Camera, PC: Controls & Window and PC: Mods. Left/Right or Cross change a value. These pages reuse
the game's Camera & Controls screen: its menu shows the new rows, and its input and refresh code are
hooked while a PC page is open. The title screen's Options menu doesn't have them.

- **Graphics.**
  - Renderer: **OpenGL** (GPU, the default) or Software (CPU). A restart is needed after switching.
  - Internal resolution: 1x–8x of 480×272 (OpenGL only; changes live).
  - Texture filtering and anisotropic filtering.
  - **Frame rate: 30 (original) or 60.**
  - VSync, fullscreen, window scale, aspect ratio, and an FPS counter.
  - **Lighting: Original or Enhanced** (the default, OpenGL only), with real-time shadows, specular
    highlights, shadow quality and light intensity. See below.
- **Controls.** Shooting style (Modern / Classic), mouse look, strafe mode for classic (off / hold /
  toggle), right-stick camera, and stick deadzone.
- **Camera.**
  - **Field of view** slider: 25°–80° vertical. The original is 31.5° (53° horizontal); the default is
    now 40° (65° horizontal).
  - **Camera distance**: 0.6x–2.5x. The default is now 1.3x. The camera still pulls in near walls.
  - **Draw distance**: 1x–4x the original distance at which objects (scenery props, crates, enemies,
    characters) stop being drawn. The default is 2x.
  - **Free camera**: the right stick tilts the camera up and down as well as turning it. The tilt stays where
    you leave it. There is also an invert option and a speed setting.
- **Audio.** Volume.
- **Mods.** The Lacerator rework, the Mini-Turret Glove and the Riptide armour (see below), with the
  Riptide set's description and buttons to put it on or take it off, the multiplayer skins, and Play as Clank.
- **Debug** (available in a level):
  - **Level select.** Covers every planet, both Giant Clank battles and the skyboard races. The
    multiplayer maps and the developer test level are listed too. Warping into a multiplayer map
    skips the game setup and leaves you without a character; use Multiplayer Game instead (see below).
  - **God mode** (F5): health stays full whatever hits you. Pits and other instant deaths still
    respawn you.
  - **Noclip** (F6): Ratchet floats and passes through walls and floors. The left stick (WASD) moves
    him where the camera looks, jump (Space / A) rises, wrench (J / X) or Left Ctrl sinks, and Left
    Shift (or clicking the left stick) goes faster. The camera follows him. Switch it off and he drops.
  - **Free camera** (F7): a camera you fly on its own while Ratchet stands still. Same movement keys
    as noclip; look with the right stick, the mouse, Q/E and R/F. Start still pauses. Switching it off
    returns to the normal camera.
  - **Flying speed** for noclip and the free camera (0.25x–4x). These three toggles aren't saved and work
    in the story levels.
  - **Skyboard anywhere** (F9, experimental): Ratchet rides the skyboard from the skyboard races in any
    level (see below).
  - **Unlock all armour** (all 7 sets, every piece) and **unlock all skins** (Plundering Pirate
    Captain, Ratchetzilla, Super Incognito, Tropical Vacation). Equip skins in Special > Skins at
    no Titanium Bolt cost.
  - **Unlock all weapons.** A separate button unlocks gadgets and items.
  - **Weapon levels.** Set every owned weapon to V1–V8 at once, or set each weapon's level and
    ownership individually.
  - **Refill ammo.**
  - **Saving.** These changes go into your current game, so saving keeps them.

### 60 fps

The game is a fixed-step 30 Hz game:
- It caps itself by waiting for two vblanks per frame.
- The world advances 1/30 s per frame. Over 100 functions in each level hardcode that 1/30.
- The player logic runs two 1/30 s sub-steps per frame.

At 60 fps the runtime patches all of this during gameplay:
- The second vblank wait is skipped.
- The world's 1/30 constants become 1/60.
- The player runs one 1/30 s sub-step per frame, which is exactly its original rate.

Tested deterministically with identical input at 30 and 60 fps: Ratchet's position, camera and
cutscene timing match to within millimetres, and the Lacerator fires at the same rate.

The patches sit at code locations found by instruction pattern in every level module
(`data/hooks.json`). Menus stay at 30 fps. Anything that counts frames instead of time could still
run at a different speed at 60 fps. If something looks wrong, switch back to 30.

### Enhanced lighting (OpenGL, on by default)

The PSP lights the game per vertex, with one main directional light per level (the sun) plus a few
local lights. Enhanced lighting keeps that as the base and adds:
- **Per-pixel shading.** Everything the sun lights is re-shaded per pixel against it. Nearby surfaces get
  slight relief from their own textures (a bump derived from texture brightness, a few millimetres deep
  and clamped).
- **Real-time shadows.** Ratchet, enemies, crates, plants and buildings cast shadows from the sun. A
  shadow map follows the camera: 1024, 2048 or 3072 pixels covering 32, 48 or 64 units, depending on the
  quality setting. It is drawn from each frame's own opaque geometry, and that render pass is moved in
  front of the frame's first draw that reads it, so shadows don't trail behind moving objects.
- **Specular highlights** from the sun, with a Fresnel rim, stronger on bright texels.

Indoor areas that have no directional main light keep the original lighting. The sky, glows and other
unlit or additive effects aren't touched. Cut-out textures such as foliage cards don't cast shadows,
because the shadow pass doesn't read their alpha.

The work happens in the OpenGL renderer: extra shader bits and varyings in PPSSPP's shader generators,
and the shadow pass on its render manager (`runtime/gpu/ppsspp/GPU/GLES/RcsmLight.cpp`). The software
renderer always shows the original lighting.

### Camera

How the camera options are implemented:
- **FOV.** The game passes its field of view to its projection setup every frame. The runtime widens
  it there, so the whole game, cutscenes included, shows more around the edges.
- **Distance.** The follow distance and height that the camera aims for are scaled, so the game's own
  collision handling still applies.
- **Tilt.** The camera code already has tilt support (it reads d-pad up/down), but the
  third-person camera has it switched off. The runtime switches it on and feeds it the right stick.
- **Turning.** Turning speed follows how far the stick is pushed.
- **Draw distance.** Each object carries its draw distance as a byte, in world units, which the
  renderer compares with its distance from the camera. Static scenery typically uses 32–64 units and
  characters up to 128. Three code hooks scale that byte where the renderer reads it, and the fade-out
  band moves with it. The update distance, beyond which objects stop animating and thinking, isn't
  changed, so enemies still wake up at their original range. Terrain and the level's fog aren't
  affected.

### Mini-Turret Glove (restored weapon, on by default)

The Mini-Turret Glove was cut from the game. Weapon slot 14 still has its name and ammo texts, but
only one stat block, no icon, no description, and no vendor or menu shows it. Its real code and
models survive only in the developers' test level, HIG Treehouse (level 24 in the debug level
select). There, slot 14 throws a ball that unfolds into a turret on a pole and fires rockets.
The story levels have none of that code or those models, so the weapon is rebuilt here:
- **Where to get it.** Vendors sell it from Kalidon on, for 30,000 bolts, with a full load of ammo.
- **What it does.** It works like the Bee Mine Glove. A developer note in the game's text says the
  turret played very like it, which is why it was cut. Equipping it spawns that glove and its firing
  code: it throws a turret that lands and fires homing rockets at nearby enemies.
- **Looks.** The real Mini-Turret models from the test level are used: the turret, its red-and-white
  rockets, and the Mini-Turret Glove on Ratchet's hand. The glove is the Bomb Glove's model with the
  Mini-Turret Glove texture, exactly as in the test level. tools/model/turretviz builds them into
  runtime/turretviz_data.c. While the Mini-Turret is held, the Bee Mine Glove's classes wear these
  models and textures. They switch back the next time you hold the Bee Mine Glove itself. The mine's
  explosion chunks share the turret's texture, so they show flat yellow meanwhile.
- **Levels.** It has all 8 levels: Mini-Turret Glove, V.2 to V.7, then the Mega-Turret Glove at V.8.
  Its name, description, ammo (10 rising to 30), pickups, price and XP are its own, and it levels up
  with use. The Weapons screen shows the glove's own preview model from the game's menu data.
- **Icon.** A new 32x32 HUD icon (a turret on a tripod) is registered with the HUD at every level
  load and shows on the ammo HUD, the Quick Select ring and the Weapons screen.
- **Menus.** The Weapons screen gets a 14th cell for it (bottom row, fourth), and it's added to
  Quick Select.
- **How it works.** The Bee Mine code always reads weapon record 6, so while the Mini-Turret is held
  its numbers are swapped into record 6. They're swapped back before the game saves.
- **Older saves.** A save that bought the old Plasma Gatling (which used this slot) now owns the
  Mini-Turret Glove instead. Switching the mod off in Mods takes effect on the next level load.

### Riptide armour (new armour set, on by default)

> *Gadgetron's deep-water division built the Riptide set for Pokitaru's lifeguard corps after one too
> many "incidents" at the Jowai Resort. Pressure-forged hydro-alloy plates ride on a lining of living
> tide-gel, and the fins aren't for show: they bleed off impact energy the way a breakwater turns a
> wave. Captain Qwark returned his set after it "made him look damp".*

An eighth armour set: helmet, body armour, gloves and boots in deep navy and sea-green, with a curling
crest, swept fins and forearm blades.
- **You have it from the start.** The first time a game is loaded, Riptide goes on every empty armour
  slot, so a new game starts in the full set. It is saved with your game.
- **Defence.** The same as Electroshock: 16 per piece, damage reduction 74 for the full set.
- **Breaker (full set).** Jump and slam your wrench: instead of Electroshock's violet shockwave, a ring
  of crashing surf rolls out, and the wrench trails sea spray. It hits like the Electroshock wave.
- **Tidal Mend (full set).** After 4 seconds without taking damage your health slowly refills, 3% of
  your maximum per second.
- **Wearing it.** The Armour screen has no room for an eighth column, so Riptide shares Electroshock's.
  CROSS on that column cycles Riptide → Electroshock (once you own it) → nothing. The name under
  the preview shows which one you have. The Mods tab can also put the whole set on or take it off.
- **How it works.** The game's model (`.gde`) and texture (`.gim`) formats were decoded, including the
  skinning (bone palettes and weights). The tools in `tools/model/` take Electroshock's skeletons and
  meshes, add the new geometry (crest, fins, blades) bound to the right bones, paint new textures and
  write `runtime/armour_data.c`. In game, a slot wearing Riptide is really wearing Electroshock (so all
  the game's armour logic applies), marked in a spare byte of the saved game state. While a slot is
  marked, Electroshock's model records point at the Riptide models, and the full-set effect textures
  are swapped for new ones.
- **Switching it off** in Mods shows Electroshock again. The stats don't change.

### Extra skins: the multiplayer characters and Clank

The eight multiplayer characters can be worn in the story like the skins in Special > Skins: Snowman,
Hotbot, Captain Qwark, Ninja, Training Bot, Nurse, Technomite and Dan, each in any of its team colours.
Clank is on the list too (see Play as Clank below). Pick one under Mods > Extra skins (F1), or on the
pause menu's PC: Mods page ("Extra Skin" and "Skin Colour"). It goes on when the game resumes and stays on across levels and restarts. Choosing a
skin in Special > Skins takes it off; so does setting it back to Off.

- **Why not in Special > Skins?** That screen's five rows are built into the game's menu code. The PC
  menus have room.
- **How it works.** They share Ratchet's skeleton and animations. The single-player skin table is
  replaced by a longer copy with the multiplayer models appended, and the level's own skin loader puts
  them on. Some changes are made to each model as it loads:
  - Every texture slot points at the chosen colour.
  - The shaders stop cycling through the eight team textures.
  - The first detail level is pointed at the second. A multiplayer model keeps only a stub in its first
    level, and the player is always drawn with the first. The second level's bone palette indexes a
    slightly smaller skeleton, so it's remapped by matching bind matrices.
- **Saves.** The game only draws a skin while the saved "equipped skin" number isn't 0. While a
  multiplayer skin is on, that number is your own equipped skin if it isn't Ratchet; otherwise it's
  skin 1. The real value is kept in settings.ini and put back when the multiplayer skin comes off, so
  saves never hold a number the original game doesn't know.
- **Not included:** RatchetMP, Ratchet in team colours. Its model is bigger than the skin buffer.

### Play as Clank

Mods > Play as Clank (or F8):
- **On Ryllus** you play as the real Clank anywhere in the level. Ryllus has Clank sections, so it has
  his own controls, model and animations. The game's own Ratchet-to-Clank switch is called whenever
  you like, and switching the option off switches back to Ratchet where you stand. If Clank dies, the
  game respawns Ratchet, and you're put back into Clank 2 seconds later.
- **Everywhere else** you get Clank's look on Ratchet's moves. The other levels' code was compiled
  without Clank's player code, and moving it across isn't practical: each level is a separate program
  with its own memory layout. So Clank's hero model is fitted to Ratchet's skeleton
  (tools/model/clankskin) and worn as a skin, and he runs, jumps, swings the wrench and fires
  Ratchet's weapons. The Clank on Ratchet's back is hidden meanwhile.
  - The fit stretches each limb so Clank's joints land on Ratchet's, at Ratchet's height. His arms
    come out a bit long and thin.
  - Rebuilding the model needs Clank's model from a Ryllus RAM dump; see `tools/model/clankskin/convert.py`.
- **Metalis** starts you as Clank already.
- The option isn't saved.

**No backpack Clank** (Mods): Ratchet goes without Clank on his back in every level, as on Inside
Clank. It hides both the backpack Clank and his hands (a separate object). The Clank skin hides them
too.

### Skyboard anywhere (Debug, F9, experimental)

Riding the skyboard is an ordinary player state in the player code that every level shares; the races
put Ratchet in it at the start. Turning this on puts him in it anywhere:
- **Controls** are the race's: he surfs forward, the left stick steers and CROSS jumps. Hold SQUARE
  or CIRCLE to boost. Jump and keep SQUARE held to fly: he holds his height and glides until you let
  go. The board code only allows flight inside a race's "flight zones". Story levels have none, so
  the zone check (hook 28, `SKY_FLIGHT_ZONE`) says yes everywhere outside the races.
- **Endless boost.** Boosting costs fuel, which the races refill with pickups. Outside the two race
  levels the tank is kept full. In the races it works as normal.
- **Sounds.** The board's seven sounds (start, idle, in flight, boost, decelerate, collide) are played
  by the shared board code, but their data only ships with the race level. They were extracted
  (tools/model/skyboard/extract_sounds.py, runtime/skyboard_sounds.c) and are put in place while
  riding outside the races.
- **Engine trails.** The board object that draws them only exists in the race levels, so the shared
  particle spawner is called each frame at the two engines, with the race's trail settings.
- **A chase camera** follows him. The race camera only exists in the race levels.
- **Falling off.** The race respawn follows the race track, which story levels don't have. So falling
  well below where he last touched ground (off a cliff or into water), or staying airborne for more
  than 12 seconds without flying, puts him back at the start of the level.
- **The board.** The board model only exists in the race levels' data, but the loading screen's
  model of Ratchet on the board is in memory in every level. Its board is cut out
  (tools/model/skyboard) and worn by the Clank on Ratchet's back while he rides: the game's own board
  code keeps that object under his feet. In the races Clank *is* the board, so he's off Ratchet's back
  meanwhile and comes back when you step off.
- **The race HUD** (place, fuel bar, timer) shows while riding, because the shared HUD code shows it
  for that state.
- **Stepping off.** Switch it off and he steps off and walks again. Getting on the board switches
  the player's camera control mode and moby callback to the race's. In the races, only the race's
  own exit sets them back, so they're saved when he gets on and restored when he steps off or
  falls. It isn't saved.

### Lacerator rework (on by default)

Each value can be set in the Mods tab, and the whole mod can be switched off there.

- **Fire rate 2x.** The refire cooldown is halved, and Ratchet's shooting pose ends early so it
  doesn't hold the next shot back.
- **Damage 0.6x.** It scales the per-level damage table (1, 3, 5, 4, 13, 18.5, 26.5, 44, plus the
  upgraded variants).
- **Max ammo 180** instead of 60. Upgraded levels and ammo pickups scale by the same factor.
- **Where it applies.** All patches are found by signature in every level module, so they work in
  every level and can be changed live.
- **Existing saves.** These keep their current ammo count, which tops up through pickups.

### Multiplayer maps (solo)

Real multiplayer is out of scope, but the four multiplayer maps can be loaded and run around in on your
own. The port includes a network stack with nobody else on it, so the game's own multiplayer menus work:

1. Main menu > **Multiplayer Game** > **Ad-hoc**.
2. The first time, there's no multiplayer data yet. The game says so: choose **Cancel**, then
   **Create Profile**. Pick a name on the on-screen keyboard (2+ characters, starting with a letter;
   Start finishes), press Circle to create it and save it when asked. Next time just **Select Profile**.
3. **Create Game**. Pick the mode and the map (left/right on the Map row: Island Escape, Danger Valley,
   and two more) and press Circle to create it.
4. In the Staging Room, press **Start**. You're alone, so nobody shoots back.

Quit from the pause menu (Start > Options > Quit) to go back to the multiplayer menu. Multiplayer runs
at 30 fps (the 60 fps option applies to the story levels). Joining or hosting real games, Quick Play
and Infrastructure mode don't work: there's no one on the network.

## Status

Working:
- Boot flow: language select, intro videos, "Press START", main menu, new game, save creation.
- Level loading and gameplay: running, jumping, combat, HUD, respawning.
- Saves (stored unencrypted in standard PSP layout).
- Sound effects (SAS) and ATRAC3 music decoding.
- FMVs (H.264 video, ATRAC3+ audio) when the `.PMF` files are present. Tested with the Pokitaru clips.
- Later levels (Challax onwards). They used to flicker and show stretched or misplaced objects: every
  level change leaked the 256 KB stacks of the level module's start/stop threads, until the game's
  per-frame allocations (skinning matrices) started failing. The stacks are now freed, and memory
  stays the same however many levels you visit.

Known limitations:
- **Software renderer speed.** The software renderer is CPU-heavy. Use the OpenGL renderer (the default).
- Multiplayer: there's no real networking. The multiplayer maps can be played alone (see above).
- Only the start of the game has been tested. Other levels load the same way, but untested code
  paths may hit an unimplemented OS call (logged as `UNIMPLEMENTED`) or an indirect jump target the
  recompiler didn't find. The latter stops with a message telling you which address to add to
  `data/seeds_<module>.txt`, followed by a rebuild.

## How it works (layout)

- `tools/recomp.py`: the recompiler. It parses a PSP ELF/PRX, applies relocations, finds functions,
  jump tables and indirect-call targets, and emits position-independent C (modules can load at any
  64 KB-aligned address). VFPU instructions call PPSSPP's VFPU interpreter (`runtime/vfpu`).
- `runtime/`: the native runtime.
  - `core.c`: memory, module loader, call dispatch.
  - `kernel.c`: threads as coroutines, semaphores, event flags, message boxes, mutexes, pools,
    callbacks, interrupts, module manager.
  - `io.c`: file system.
  - `hle_misc.c`: display, controller, audio output.
  - `fly.c`: debug god mode, noclip and free camera.
  - `net.c`: a stand-in network (WLAN, ad hoc, infrastructure) with nobody else on it.
  - `frontend.c`: SDL window, input, audio, screenshots.
  - `mods.c`, `armour.c`, `menu.cpp`, `nativemenu.c`: the settings menus (ImGui and the game's own),
    60 fps, camera, modern shooting and the mods (Lacerator, Mini-Turret Glove, Riptide armour;
    `armour_data.c` is generated).
  - `skins.c`: the multiplayer characters and Clank as single-player skins (`clankskin_data.c` is generated).
  - `clank.c`: Play as Clank. `skyboard.c`: skyboard anywhere.
- `tools/model/`: the model tools: GDE/GIM readers and writers, a preview renderer, and the
  Riptide design (`riptide.py`, `fx.py`, built by `build_armour.py`).
- `runtime/gpu`, `runtime/audio`, `runtime/mpeg`, `runtime/utility`: components extracted from
  PPSSPP (GPL-2.0-or-later; see the LICENSE.TXT files there). Because of them, a built binary is
  GPL-licensed.
- `gen/` (generated): recompiled C, one directory per module.

Builds made by this tool contain code derived from your game files. Keep them for your own use.
The same goes for `runtime/armour_data.c`: the Riptide models are built on the Electroshock armour's
skeletons and meshes. To rebuild it, dump RAM in a level with Riptide switched off
(`RCSM_DUMP_AT=<vblank>`, `riptide = 0` in settings.ini), run
`tools/model/reference.py` on the dump (it writes `tools/model/ref/`, which isn't included), then
`tools/model/build_armour.py`.
