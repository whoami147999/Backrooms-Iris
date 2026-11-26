# Backrooms Iris

Open-source co-op Backrooms horror. Up to 4 players, PvE: survive, keep your head together, find the exit.
One C++ codebase for **Linux, Windows and PS Vita**, plus a headless dedicated server for **FreeBSD**.

![0.6](docs/screenshots/v06/06-shotgun.jpg)

> Inspired by Backrooms games and the community wiki lore. All code, art and sound are original and generated
> in code: no assets, maps or art copied from any other game.

## What's in 0.6

| | |
|---|---|
| **Sound** | every sound synthesized in code (no files, no copyright): fluorescent hum, drones, wind, water, alarms, footsteps per surface, gunshots, entity noises, stings, heartbeat, music box, UI. Positional, muffled behind walls. Settings: VOLUME |
| **New models** | entities and players rebuilt from smooth curved shapes on a real skeleton (knees, elbows, a walk cycle); guns, crowbar and flashlight are 3D in your hands |
| **Exit doors** | real 3D doors: deep steel frame, push bar, hinges, closer, wired-glass window, a boxed EXIT sign |
| **Props everywhere** | chairs, tables, filing cabinets, boxes, barrels, cones, benches, plants, shelves, lockers, pallets, rubble, trees, parked cars, mailboxes, couches, TVs, fridges, scarecrows. You bump into them |
| **Wanderers' camps** | bedroll, lantern, fire barrel, boxes and supplies left behind, on most indoor levels |
| **More to walk into** | most hotel blocks are rooms now; Level 21's doors sometimes open into offices; two thirds of the mall's shops are open; doorways have arches |
| **Curves** | round columns with bases and capitals, round pipes, a vaulted ceiling in Level 2, round tables, barrels, pillows, trees |
| **3 new levels** | Level 3 Electrical Station, Level 10 Bumper Crop (wade through wheat), Level 13 The Infinite Apartments |
| **4 new items** | Batteries, Night Vision Goggles, Chalk, Painkillers |
| **Memory juice** | now makes you fast and unkillable... until it kills you |
| **Compass** | nerfed: only works while you hold it, short range, the needle wanders (worse in the dark), no distance |

## What was in 0.5

| | |
|---|---|
| **13 new levels** | Level 7 Thalassophobia, 19 The Attic, 21 Numbered Doors, 22 The Car Park, 24 The Moon, 33 The Infinite Mall, 36 Airport, 51 The Forgotten City, Level ! Run For Your Life, and TH3 SH4DY GR3Y with its sub-levels B14CK, M4NS10N and L0ST H0P3 |
| **10 new items** | Super Almond Water, Moth Jelly, Liquid Silence, Med Kit, Flash Bulb, Glow Stick, Music Box, Rabbit's Foot, Compass, Radio |
| **Sandbox guns** | Pistol, Shotgun, Rifle (hold to fire). Entities have health now: shoot them dead. Smilers just leave |
| **PS1 look** | chunky pixels, ordered dithering, wobbly vertices, swimming (affine) textures, crisp texels, in the spirit of LSD: Dream Emulator. Settings: RETRO PS1 LOOK |
| **More detail** | papers, stains, cracks, puddles, outlets, vents, mould on every level; real parked cars; rows of airport seats |
| **Tailscale** | play with friends anywhere: friends' games on your tailnet show up in JOIN GAME automatically, and you can connect by Tailscale IP or machine name |

### The new levels

| level | what it is | who lives there |
|---|---|---|
| **7 - Thalassophobia** | a drowned house in a dark endless sea. You wade (slowly) between piers, rocks and lamp posts | one Smiler |
| **19 - The Attic** | long, low wooden lanes under a sloping roof, junk boxes, bare bulbs, mostly dark | Smilers, a Hound |
| **21 - Numbered Doors** | hallways lined with green numbered doors that never open | a Bacteria |
| **22 - The Car Park** | endless concrete parking levels, parked cars, sodium lights, broken floor | Hounds, a Skin-Stealer |
| **24 - The Moon** | grey dust, boulders and spires under a black sky; the Earth, Jupiter and Saturn hang overhead | a Hound |
| **33 - The Infinite Mall** | neon shop fronts, checkered tiles, planters, abandoned stores you can walk into | a Skin-Stealer, a Smiler, a Partygoer |
| **36 - Airport** | concourses, gate lounges with rows of seats, night-black windows with runway lights | a Bacteria, a Smiler |
| **51 - The Forgotten City** | ruined stone city with colonnades and stepped temples in a vast cavern, lit by braziers | Hounds, Smilers |
| **! - Run For Your Life** | one red, strobing corridor. They come up behind you the moment you arrive. The door is 150 m ahead | Partygoers, a Hound |
| **TH3 SH4DY GR3Y** | greyscale halls: no colour at all | Bacteria |
| **TSG - B14CK** | pitch-black halls, fast things in them | Bacteria, Hounds |
| **TSG - M4NS10N** | a grey mansion with rooms you can walk into | a Skin-Stealer, Smilers |
| **TSG - L0ST H0P3** | the last dark maze. Its exit is the way out of everything | Smilers, a Bacteria |

Exit order now: 0 → 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10 → 19 → 21 → 22 → 33 → 36 → 37 → Fun → ! → 51 → 24 → 11 → 13 → TH3 SH4DY GR3Y →
B14CK → M4NS10N → L0ST H0P3 → back to 0.

The level designs are our own takes on the community wiki pages (the wikis disagree a lot, especially on TH3 SH4DY GR3Y).

### The new items

| | |
|---|---|
| **Super Almond Water** | stamina full, +70 sanity, +25 health |
| **Moth Jelly** | heals you slowly for 15 s |
| **Liquid Silence** | 30 s of making no sound at all (hounds can't hear you) |
| **Med Kit** | +70 health |
| **Flash Bulb** | throw it: everything that can see the flash is stunned for ~5 s (Smilers flee). Don't look at it |
| **Glow Stick** | drop it: green light for 3 minutes (keeps your sanity up, doesn't scare anything) |
| **Music Box** | throw it: for 20 s everything nearby wanders over to listen |
| **Rabbit's Foot** | keep it: the next thing that would kill you doesn't (once per foot) |
| **Compass** | a needle at the top of the screen pointing to the nearest exit door, with the distance |
| **Radio** | static gets louder (and the meter climbs) the closer something is |
| **Pistol / Shotgun / Rifle** | sandbox only. Click / Square to shoot; the rifle fires while you hold. Gunfire brings hounds running |

### Playing with friends over Tailscale

[Tailscale](https://tailscale.com) puts your PCs on one private network, wherever they are, with no port forwarding.

1. Everyone installs Tailscale and logs in to the **same tailnet** (or the host shares their machine with friends from the
   Tailscale admin console).
   - Arch: `sudo pacman -S tailscale`, `sudo systemctl enable --now tailscaled`, `sudo tailscale up`
   - Windows: install from tailscale.com and sign in
   - FreeBSD server: `pkg install tailscale`, `sysrc tailscaled_enable=YES`, `service tailscaled start`, `tailscale up`
2. The host picks **MULTIPLAYER → HOST GAME** (or runs the dedicated server). The host screen shows its Tailscale IP.
3. Friends open **JOIN GAME**: games running on any online machine in the tailnet appear in the list marked
   `[TAILSCALE]`. You can also type the host's Tailscale IP (`100.x.y.z`) or machine name in **IP OR NAME**.

The game finds friends with the `tailscale` command (`tailscale status --json`), so the Tailscale CLI has to be
installed (it is with the normal installers). Windows may ask to allow the game through the firewall the first time
you host: allow it. The Vita can't run Tailscale; it plays on the LAN.

## What was in 0.4

| | |
|---|---|
| **Difficulty** | Easy, Medium, Hard, Nightmare (single player, sandbox, hosting and the dedicated server, PC and Vita) |
| **Objectives** | every level has one: find the lost tapes, turn on the breakers, or survive. Finishing it gives everyone a **perk for the rest of the run** |
| **Perks** | Big Lungs (+25% stamina), Second Wind (faster recovery), Steady Nerves (less sanity loss), Thick Skin (less damage), Light Feet (quieter), Spare Cells (longer flashlight). They stack up to 3 |
| **Exit doors** | set into real walls now, with a small sign over them, no longer standing in the open. Fewer of them on harder difficulties, and on Hard/Nightmare the signs barely glow |
| **Level 5 rooms** | hotel blocks you can walk into: guest rooms with a bed, nightstands, a lamp, a dresser, a wardrobe, a painting. More loot inside |
| **Graphics** | film look on PC (bloom, grain, lens fringing, colour grading, vignette), optional camcorder look, light halos and shafts, flickering tubes, dust in the air, contact shadows, 2x textures and finer lighting on PC, new entity and player models |

### Difficulty

| | entities | speed | damage | sanity loss | items | exits | grace period | healing |
|---|---|---|---|---|---|---|---|---|
| **Easy** | 0.6x | 0.85x | 0.6x, and a touch only hurts | 0.6x | 1.5x | many | 60 s | 2x |
| **Medium** | 1x | 1x | 1x | 1x | 1x | normal | 35 s | 1x |
| **Hard** | 1.5x | 1.08x | 1.35x | 1.3x | 0.75x | fewer, dim signs | 20 s | 0.5x |
| **Nightmare** | 2x | 1.18x | 1.8x | 1.7x | 0.5x | few, dim signs | 8 s | none |

Entities also hear and see further on Hard and Nightmare. Pick it under DIFFICULTY in the single player, sandbox or
host menu. Dedicated server: `difficulty = nightmare` in the config, or `--difficulty nightmare`.

### Objectives

Shown top left. **Lost tapes** lie on the floor (look for the faint blue glint) and **breaker boxes** hang on walls with
a blinking red light: walk up and press E / Cross. **Survive** just counts down. Objective things are always somewhere
you can reach from where you start. The reward is listed before you start, and in multiplayer everyone gets it.
Perks stay through deaths and level changes until you quit the run.

### Graphics settings

Settings menu: **RETRO PS1 LOOK** (on PC: ~360-line pixels, 15-bit colour with ordered dither, vertices snapping to a coarse grid, affine texture swim; on both PC and Vita: crisp, unfiltered texels), **FILM EFFECTS** (bloom, grain, fringing, grading; on the Vita, grain and a vignette), **CAMCORDER LOOK**
(scanlines, a rolling tracking band, REC and a clock), **LIGHT HALOS + DUST** (glowing lamps, light shafts, flickering
tubes, dust). The film look needs GLSL 1.20 on PC; without it the game falls back to the Vita-style overlays.

## What was in 0.3

| | |
|---|---|
| **5 new levels** | Level 2 Pipe Dreams, Level 4 Abandoned Office, Level 6 Lights Out, Level 37 Sublimity (poolrooms), Level Fun =) |
| **7 new items** | Flashlight, Crowbar, Royal Ration, Liquid Pain, Cashew Water, Lucky O' Milk, Firesalt |
| **Hotbar** | pick any item with 1-9 / mouse wheel / D-pad, use it with left click / Square |
| **Sandbox mode** | spawn any entity or item, god mode, noclip, freeze or clear entities, jump to the exit or next level |
| **Textures** | redone without seams or stretching; every new level has its own set |
| **Fixes** | pause menu no longer turns the level into Level 0; items can always be picked up; plus a pile of smaller ones (see below) |

### Bugs fixed in 0.3 

- **Pause menu made everything Level 0.** The menu background re-pointed the renderer at the title screen world.
  Pausing now draws the real game behind the menu and never touches the level.
- **Items sometimes couldn't be picked up.** The server compared your pickup counter with a stale value after a
  level change or reconnect, so the first press(es) were thrown away. Counters now reset with the session and the
  server takes a fresh baseline. Pickup range is a bit bigger too.
- Texture seams, mirrored signs, stretched walls on low and tall surfaces, z-fighting on lights.
- Chunk meshes build on a background thread, so walking into new areas no longer hitches.
- Entities could spawn in plain sight in open levels; snapshots could drop nearby entities with lots of players.

## What was in 0.2

| | |
|---|---|
| **6 levels** | Level 0 The Lobby, Level 1 Habitable Zone, Level 5 Terror Hotel, Level 8 Cave System, Level 9 The Suburbs, Level 11 The Endless City |
| **5 entities** | Bacteria, Hounds, Smilers, Skin-Stealers, Partygoers |
| **Items** | Almond Water, Energy Drink, Memory Juice |
| **Sanity** | drains in the dark, near entities and over time; low sanity messes with your eyes; zero sanity hurts |
| **Menus** | single player / multiplayer, settings (mouse + stick sensitivity, invert, FOV, head bob, fullscreen, vsync), controls |
| **Multiplayer** | host from inside the game or run the FreeBSD server; LAN browser; join by IP; PC and Vita in the same game |
| **Vita** | full build target: same game, same menus, controller-only UI, LiveArea art |

## Levels

Every level is endless and generated from a seed, so the same seed gives the same world on every machine.
Find the **EXIT** door: it's set into a wall somewhere, with a small green sign over it. Walk into it and everyone goes to the next level.

| level | what it is | who lives there |
|---|---|---|
| **0 - The Lobby** | mono-yellow rooms, damp carpet, buzzing lights, some dead | Bacteria, a Smiler in the dark |
| **1 - Habitable Zone** | huge concrete warehouse, columns, crates, puddles, misty, whole sections without power | Hounds, Bacteria |
| **5 - Terror Hotel** | endless corridors, red carpet, wood panels, ballrooms, and guest rooms you can walk into (beds, lamps, loot) | Partygoers, a Smiler |
| **8 - Cave System** | pitch-black caves, lumpy ceilings, glowing fungus. You see a few metres | Smilers, Hounds |
| **9 - The Suburbs** | streets of identical houses at night, half the streetlights dead | Skin-Stealers, Hounds |
| **2 - Pipe Dreams** | narrow maintenance tunnels, pipes everywhere, hot boiler rooms, caged bulbs | Hounds, Bacteria, a Smiler |
| **4 - Abandoned Office** | empty cubicle farm, desks, fluorescent lights, all quiet. Mostly | Bacteria |
| **6 - Lights Out** | every light is dead. Bring a flashlight (there's one where you spawn) | Smilers. Lots of them |
| **37 - Sublimity** | the poolrooms: white tile, warm water, sunlight from nowhere. **Safe** | nobody |
| **Fun =)** | party rooms, balloons, confetti carpet. It is not fun | Partygoers |
| **11 - The Endless City** | grey daytime city, endless towers. **Safe**: sanity recovers here | nobody |

Exit order: 0 → 1 → 2 → 4 → 5 → 6 → 8 → 9 → 37 → Fun → 11 → back to 0. Of course it loops.

## Entities

| | how it acts | how to live |
|---|---|---|
| **Bacteria** | tall, thin, black. Freezes for a second when it spots you, then runs you down. Faster than your sprint in a straight line, but it turns slowly | break line of sight: corners, doorways. One touch kills |
| **Hounds** | hunt by **sound**: sprinting is loud, walking less, crouching is nearly silent | look them in the eye and don't run: they hesitate and back off. Turn your back and they lunge |
| **Smilers** | a grin in the dark, nothing else. Drifts closer while you're not looking | don't stare at it, don't sprint near it, get to the light. Looking too long makes it charge |
| **Skin-Stealers** | patrol the streets of Level 9, chase on sight, hit hard | a full sprint barely outpaces them. Energy drinks help |
| **Partygoers** | `=)` They want you at the party. Slower than you, but they never stop following | keep moving and find the exit. Being touched is final |

## Items

| | |
|---|---|
| **Almond Water** | refills stamina and restores a chunk of sanity |
| **Energy Drink** | ~14 s of moving much faster with no stamina cost, then a crash |
| **Memory Juice** | don't. A long deliriant trip: shadow people, things on the floor that aren't there, a "friend" standing there staring, spiders on your eyes, whispers, double vision, then the dark closes in. Then you die |

| **Flashlight** | toggle with F / Triangle. The battery runs ~3 min and recharges when off. Smilers hate it |
| **Crowbar** | swing it: knocks entities down for a few seconds. Doesn't kill anything |
| **Royal Ration** | heals a lot |
| **Liquid Pain** | hurts going down (costs health) but clears your head (big sanity boost) |
| **Cashew Water** | cures memory juice if you drink it early enough, but slows you down for a while |
| **Lucky O' Milk** | ~35 s of being much quieter: Hounds and others barely hear you |
| **Firesalt** | throw it: burns for ~40 s and lights the area up. Entities keep away from the fire |

Everyone starts with one Almond Water, and Levels 0, 6 and 8 have a flashlight right next to where you spawn.
Drinks stack up to 5; tools (flashlight, crowbar) don't stack. Dying drops your items.

## Sandbox

Main menu → **Sandbox**, pick a level, then press **Tab** (Select on Vita) in game:

- **Entities**: spawn any of the 5 in front of you
- **Items**: spawn any item at your feet
- **Tools**: god mode, noclip, auto spawning on/off, freeze entities, clear all entities, heal, go to the nearest exit, next level


## Screenshots

0.6:

| | |
|---|---|
| ![](docs/screenshots/v06/06-shotgun.jpg) | ![](docs/screenshots/v06/09-exit-door.jpg) |
| ![](docs/screenshots/v06/10-camp.jpg) | ![](docs/screenshots/v06/11-wheat.jpg) |
| ![](docs/screenshots/v06/03-skin-stealer.jpg) | ![](docs/screenshots/v06/12-apartment.jpg) |

0.5:

| | |
|---|---|
| ![](docs/screenshots/v05/02-level7.jpg) | ![](docs/screenshots/v05/07-level33.jpg) |
| ![](docs/screenshots/v05/06-level24.jpg) | ![](docs/screenshots/v05/10-level-run.jpg) |
| ![](docs/screenshots/v05/15-shotgun.jpg) | ![](docs/screenshots/v05/11-tsg.jpg) |
| ![](docs/screenshots/v05/08-level36.jpg) | ![](docs/screenshots/v05/03-level19.jpg) |

0.4:

| | |
|---|---|
| ![](docs/screenshots/v04/05-level5-hotel-room.jpg) | ![](docs/screenshots/v04/06-exit-in-wall.jpg) |
| ![](docs/screenshots/v04/08-breaker-objective.jpg) | ![](docs/screenshots/v04/13-camcorder.jpg) |
| ![](docs/screenshots/v04/09-bacteria.jpg) | ![](docs/screenshots/v04/12-partygoer.jpg) |
| ![](docs/screenshots/v04/04-level5-corridor.jpg) | ![](docs/screenshots/v04/16-lights-out-flashlight.jpg) |

0.3:

| | |
|---|---|
| ![](docs/screenshots/v03/02-level-2-pipe-dreams.jpg) | ![](docs/screenshots/v03/03-level-4-abandoned-office.jpg) |
| ![](docs/screenshots/v03/06-level-37-sublimity.jpg) | ![](docs/screenshots/v03/07-level-fun.jpg) |
| ![](docs/screenshots/v03/08-smiler-vs-flashlight.jpg) | ![](docs/screenshots/v03/09-firesalt-flare.jpg) |
| ![](docs/screenshots/v03/11-sandbox-menu.jpg) | ![](docs/screenshots/v03/12-hotbar-all-items.jpg) |

0.2:

| | |
|---|---|
| ![](docs/screenshots/01-main-menu.png) | ![](docs/screenshots/02-level-0-lobby.png) |
| ![](docs/screenshots/03-level-1-habitable-zone.png) | ![](docs/screenshots/05-level-8-cave-system.png) |
| ![](docs/screenshots/06-level-9-suburbs.png) | ![](docs/screenshots/07-level-11-endless-city.png) |
| ![](docs/screenshots/08-entity-bacteria.png) | ![](docs/screenshots/10-entity-smiler.png) |
| ![](docs/screenshots/12-entity-partygoer.png) | ![](docs/screenshots/11-entity-skin-stealer.png) |
| ![](docs/screenshots/16-memory-juice-figure.png) | ![](docs/screenshots/17-memory-juice-spiders.png) |
| ![](docs/screenshots/14-exit-door.png) | ![](docs/screenshots/24-multiplayer-host-view.png) |

---

## Arch Linux

### 1. Install the tools

```bash
sudo pacman -S --needed base-devel git cmake ninja sdl2-compat mesa
```

`sdl2-compat` is Arch's SDL2 package now (it replaced `sdl2`). If pacman asks to replace `sdl2`, say yes.

### 2. Build

From the `backrooms-iris` folder:

```bash
cmake -S . -B build -G Ninja
```

```bash
cmake --build build
```

### 3. Play

```bash
./build/backrooms-iris
```

Optional: `ctest --test-dir build` runs the world, sim, item and network tests.

---

## Windows (10 / 11)

### 1. Install MSYS2

Download and run the installer from <https://www.msys2.org>, keep the default folder (`C:\msys64`).

### 2. Install the tools

Open **"MSYS2 UCRT64"** from the Start menu (that exact one) and run:

```bash
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL2
```

### 3. Build

```bash
cd /c/Users/BigG/Downloads/backrooms-iris
```

```bash
cmake -S . -B build -G Ninja
```

```bash
cmake --build build
```

### 4. Play

```bash
./build/backrooms-iris.exe
```

`SDL2.dll` is copied next to the `.exe`. To play on another PC, copy both files.
The first time you host a game, Windows Firewall asks about network access: allow it on private networks.

---

## PS Vita

Same game, same menus. Controls on the Vita: left stick move, right stick look, R sprint, Circle crouch,
Cross pick up / select, D-pad left/up/right use items, Start pause.

### 1. Install VitaSDK (on Arch)

```bash
sudo pacman -S --needed base-devel cmake git curl python
```

```bash
git clone https://github.com/vitasdk/vdpm && cd vdpm
```

```bash
./bootstrap-vitasdk.sh
```

```bash
echo 'export VITASDK=/usr/local/vitasdk' >> ~/.bashrc && echo 'export PATH=$VITASDK/bin:$PATH' >> ~/.bashrc && source ~/.bashrc
```

```bash
./install-all.sh
```

`install-all.sh` installs every VitaSDK library, including **vitaGL** and **SDL2**.

### 2. Build the .vpk

Back in the `backrooms-iris` folder:

```bash
cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
```

```bash
cmake --build build-vita
```

Output: `build-vita/BackroomsIris.vpk`.

### 3. Run it

- **Vita3K**: `vita3k-bin` from the AUR (or the AppImage from vita3k.org), install the firmware, then
  File → Install .pkg/.zip/.vpk.
- **Real Vita**: copy the `.vpk` over with VitaShell (FTP or USB) and install it.
  vitaGL apps usually need `libshacccg.suprx` on the console. If it crashes at boot, install it with
  [ShaRKF00D](https://github.com/Rinnegatamante/ShaRKF00D) first.
- Multiplayer on the Vita: connect it to the same Wi-Fi as the PC/server, then Multiplayer → Join Game.
  The LAN browser finds games automatically, or enter the IP with the D-pad.

---

## Multiplayer

**From inside the game (easiest):** one player picks *Multiplayer → Host Game*. Everyone else on the same
network picks *Multiplayer → Join Game* and the game shows up in the list. Or type the host's IP.

**Dedicated server (your FreeBSD X230):** see below. Players join it the same way: it appears in the LAN list,
or use its IP. For friends outside your house, forward **UDP 27315** on your router to the server.

The server runs the real game (entities, items, sanity, exits); clients predict their own movement so it
feels instant even on laggy Vita Wi-Fi. Dead players respawn after 10 seconds. When anyone reaches the exit,
everyone goes to the next level.

## FreeBSD dedicated server

### 1. Get the code onto the server

```bash
pkg install git
```

```bash
git clone <your repo url> backrooms-iris && cd backrooms-iris
```

(or copy the folder over with `scp -r`)

### 2. Run the installer as root

```bash
su -m root -c 'sh server/freebsd/install.sh'
```

It's a TUI (bsddialog, which ships in FreeBSD 14's base system). Pick **Install / update** and it will:

1. `pkg install cmake ninja` if they're missing (clang is already in base)
2. ask for the server name, UDP port, max players, world seed, starting level and difficulty
3. build `iris_server` from the checkout
4. install the binary, config, rc.d service, log rotation and an unprivileged `iris` user
5. start the service and check that it answers

Run it again any time to update, change settings, start/stop, read the log, or uninstall.

![installer](docs/screenshots/freebsd-installer-2-form.png)

### 3. Day-to-day

```bash
service iris_server status
```

```bash
service iris_server restart
```

```bash
tail -f /var/log/iris_server.log
```

```bash
iris_server --query 127.0.0.1
```

| file | what |
|---|---|
| `/usr/local/etc/iris/server.conf` | settings (restart after editing) |
| `/usr/local/etc/rc.d/iris_server` | service script (a crash means auto-restart after 5 s) |
| `/var/log/iris_server.log` | log, rotated by newsyslog |

---

## Controls

| action | keyboard + mouse | gamepad / PS Vita |
|---|---|---|
| move | WASD | left stick |
| look | mouse | right stick |
| sprint | Shift | R / R2 |
| crouch | Ctrl or C (hold) | Circle / B (toggle) |
| pick up / use (tapes, breakers) | E | Cross / A |
| use selected item / swing crowbar | left click or Q | Square / L / D-pad up |
| pick item | 1-9 or mouse wheel | D-pad left / right |
| flashlight | F | Triangle |
| sandbox spawn menu | Tab | Select |
| pause | Esc | Start |
| screenshot | F12 | |
| fullscreen | F11 or Alt+Enter | |
| fps + position (terminal) | F3 | |

Menus: arrows or mouse + Enter + Esc, or D-pad/stick + Cross + Circle. To edit a text field (name, IP, seed)
select it, then type, or use up/down to change the letter under the cursor and left/right to move.

## Command line

```
backrooms-iris [--level N] [--seed N] [--difficulty easy|medium|hard|nightmare] [--sandbox] [--host] [--join IP]
```

`--level` jumps straight into a level (0 1 2 3 4 5 6 7 8 9 10 11 13 19 21 22 24 33 36 37 51 fun ! grey black mansion hope) in single player; add `--sandbox` for sandbox mode. Settings live in
`~/.local/share/BackroomsIris/BackroomsIris/settings.cfg` (Linux), `%APPDATA%\BackroomsIris\BackroomsIris\` (Windows),
`ux0:data/BackroomsIris/` (Vita).

## Reporting bugs

1. what you did and what looked or felt wrong
2. the terminal output (press **F3** in game to add fps/position/health lines)
3. the level + seed (shown in single player setup) and a screenshot (**F12**)

## How it's built

```
src/core/       shared by everything, no SDL/GL: levels, collision, movement, entities, items, sanity, the game sim
src/net/        UDP sockets (Linux, Windows, Vita, FreeBSD), server core, client
src/client/     menus + HUD (app), sessions (single player / network), renderer, textures, effects, font
src/platform/   pc/ (SDL2 + OpenGL) and vita/ (vitaGL + SDL2 input)
src/server/     headless dedicated server
server/freebsd/ install.sh TUI, rc.d script, sample config, newsyslog rule
vita/sce_sys/   LiveArea images (regenerate: python3 tools/gen_livearea.py build/backrooms-iris)
tests/          ctest: every level, items, a 3-minute sim soak per level, a real UDP round trip
```

- **Levels aren't stored.** Every wall, room, house, light, item and exit is a pure function of `(level, seed, position)`.
- **One sim, three places.** `GameSim` runs in single player, in "Host game", and on the dedicated server.
- **Server-authoritative + prediction.** Clients send inputs (with redundancy so lost packets don't matter), predict
  their own movement with the same code the server runs, and replay unacknowledged inputs when a snapshot arrives.
- **One renderer for PC and Vita**: the OpenGL subset vitaGL implements (fixed function + VBOs + fog), everything
  drawn from procedural textures and box models. Screen warping for the memory juice is PC-only; the Vita gets
  the overlays.

## Roadmap

- [x] 1. Walk around a procedurally generated Level 0 on PC
- [~] 2. Vita build (target is complete; first hardware test is yours)
- [~] 3. Audio (procedural hum, footsteps, growls, drones) still to do; light flicker, film grain and VHS look are in
- [~] 4. Dedicated server + LAN co-op (working; lag compensation for hits next)
- [~] 5. Online PC ↔ Vita, entities, more levels (5 entities + 11 levels + 10 items + sandbox + objectives + difficulty done; carpet moss next)

## License

MIT, see [LICENSE](LICENSE).
