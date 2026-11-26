# Build the PS Vita .vpk (instructions for whoever / whatever builds this)

This is Backrooms Iris, a C++17 game with one shared codebase for PC and PS Vita.
The Vita build uses VitaSDK + vitaGL (rendering) + SDL2 (input and sound). CMake makes the .vpk.
The Vita version is **single player only**: it's compiled with `IRIS_MULTIPLAYER=0` and contains no network
code at all (no `src/net/`, no SceNet).

## 1. Install VitaSDK (Linux, or WSL on Windows)

```bash
git clone https://github.com/vitasdk/vdpm && cd vdpm
./bootstrap-vitasdk.sh
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH
./install-all.sh        # installs vitaGL, vitaShaRK, SDL2, math-neon, taihen, ...
```

Or use the Docker image `vitasdk/vitasdk:latest`, which has the SDK ready (VITASDK is already set).

## 2. Build

From this folder:

```bash
cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita -j
```

Output: `build-vita/BackroomsIris.vpk` (title ID BKIR00001).

Version 1.0.2. Sound uses SDL audio (`SceAudio_stub`, already linked). The renderer builds chunk meshes on a
worker thread (std::thread, so the target links `pthread`) and generates its textures at level load (power-of-two,
256 px on the Vita: about a second per level). The GLSL post effects are PC only (`#if !defined(IRIS_GL_VITA)`), so
vitaGL only ever sees fixed-function GL.

## 3. If it fails

If the build fails, these are the usual suspects:

- **undefined reference to `sce...`** at link time: add the matching `*_stub` library to the
  `target_link_libraries(iris_vita ...)` list in `CMakeLists.txt` (e.g. `SceHid_stub`).
- **undefined reference to `vgl...` / `gl...`**: vitaGL is missing: `vdpm vitaGL`.
- **SDL2 not found**: `vdpm sdl2`.
- **`vglInitExtended` signature differs**: newer vitaGL takes
  `(int legacy_pool_size, int width, int height, int ram_threshold, SceGxmMultisampleMode msaa)`;
  adjust the call in `src/platform/vita/platform_vita.cpp`.

Vita-specific code lives only in `src/platform/vita/`, `src/client/gl.h` and the `if(VITA)` block in
`CMakeLists.txt`. Everything else is shared with PC (minus the `#if IRIS_MULTIPLAYER` parts).

**Also hand back `build-vita/iris_vita`** (the unstripped ELF, next to the .vpk). It's what turns a Vita crash dump
(`ux0:data/psp2core-*.psp2dmp`) into a file + line number. Without it crash dumps are nearly useless.

## 4. Run

Install the .vpk with VitaShell. vitaGL needs `libshacccg.suprx` on the console (install it with ShaRKF00D).
The game checks for it and shows a message instead of crashing. Also runs in Vita3K.

Every boot writes `ux0:data/BackroomsIris/log.txt`. After a crash its last line shows how far startup got.

### Crash tracker + safe mode

Before each startup / level-load step the game writes the step's name to `ux0:data/BackroomsIris/boot.txt`.
A few seconds after it's running fine it writes `menu-ok` / `play-ok` (and `exit` on a clean quit). If the console
kills the game (C2-12828-1 etc.), the next boot finds an unfinished step there, and then:

- the title screen says **SAFE MODE - LAST RUN CRASHED AT: <STEP>** (that step is the bug report),
- **safe mode** turns on and stays on: no MSAA, more RAM headroom for the system, no sound, no chunk worker
  thread, no mipmaps, 256 px textures, shorter view distance. Settings > SAFE MODE turns it off again (applies on
  the next start).

Steps, in order: `libshacccg check`, `clocks`, `vitagl init`, `sdl init`, `audio init`, `renderer init`,
`menu setup`, `loading screen`, `first frame`, `level N textures`, `level N texture upload`, `level N first draw`,
`level N meshes`, `level N playing`. The log also records free memory at boot and after vitaGL starts.

### Solved: stuck on vitaGL's spinning logo (`vitagl init`)

`vglInit*()` return **GL_FALSE on success** (GL_TRUE means it worked but had to fall back to a smaller
resolution; see `vglInitWithCustomSizes` in vitaGL's `source/vgl.c`). 0.6.1 to 1.0.1 read that the other way round
and stopped on an error screen right after a successful init, hidden behind vitaGL's boot splash (which stays up until
the first frame). 1.0.2 only logs the fallback flag. If something fails after vitaGL is up, the game quits and the
reason is in log.txt (nothing it draws itself would be visible behind the splash).

Boot breadcrumbs are stamped with the version (`1.0.2|stage`), so a leftover from an older build doesn't count as a
crash, and safe mode switched on by an older version is switched off again.

### Threads on the Vita

The Vita's scheduler doesn't time-slice threads of the same priority, so the chunk builder and the sound synth call
`platform_thread_background()`: lower priority, and cores 1 / 2. The main thread (game loop + every GL call) is
pinned to core 0.

## Optional: PC build (Linux) to check the code compiles

```bash
cmake -S . -B build -G Ninja && cmake --build build && ./build/backrooms-iris
```
