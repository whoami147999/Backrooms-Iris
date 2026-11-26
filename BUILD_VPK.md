# Build the PS Vita .vpk (instructions for whoever / whatever builds this)

This is Backrooms Iris, a C++17 game with one shared codebase for PC and PS Vita.
The Vita build uses VitaSDK + vitaGL (rendering) + SDL2 (input). CMake makes the .vpk.

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

Version 0.6.1 (network protocol 6, plays with 0.6.0). Sound uses SDL audio (SDL_InitSubSystem(SDL_INIT_AUDIO)); on the Vita that needs SceAudio_stub, which is already linked. The renderer uses a chunk-building worker thread (std::thread, so the Vita target
links `pthread`) and procedural textures (all power-of-two, 512 px on the Vita, generated at level load: expect
~2-3 s per level). The film-look GLSL shader is PC only (`#if !defined(IRIS_GL_VITA)`): the Vita draws grain, vignette
and scanlines as plain textured quads, so vitaGL only ever sees fixed-function GL. The PS1 look's GLSL world shader is PC only too; on the Vita the retro
setting just switches textures to nearest filtering. `src/client/tailscale.cpp` is compiled on the Vita but does
nothing there (`#elif defined(__vita__)` returns no peers).

## 3. If it fails

The Vita target has never been compiled yet, so expect small fixes:

- **undefined reference to `sce...`** at link time: add the matching `*_stub` library to the
  `target_link_libraries(iris_vita ...)` list in `CMakeLists.txt` (e.g. `SceNet_stub`, `SceHid_stub`).
- **undefined reference to `vgl...` / `gl...`**: vitaGL is missing: `vdpm vitaGL`.
- **SDL2 not found**: `vdpm sdl2`.
- **`SO_NONBLOCK` not declared** (src/net/udp.cpp): replace with `SCE_NET_SO_NBIO` from `<psp2/net/net.h>`
  and call `sceNetSetsockopt`.
- **`vglInitExtended` signature differs**: newer vitaGL takes
  `(int legacy_pool_size, int width, int height, int ram_threshold, SceGxmMultisampleMode msaa)`;
  adjust the call in `src/platform/vita/platform_vita.cpp`.

Vita-specific code lives only in `src/platform/vita/`, `src/net/udp.cpp` (`#if defined(__vita__)`),
`src/client/gl.h` and the `if(VITA)` block in `CMakeLists.txt`. Everything else is shared with PC.

**Also hand back `build-vita/iris_vita`** (the unstripped ELF, next to the .vpk). It's what turns a Vita crash dump
(`ux0:data/psp2core-*.psp2dmp`) into a file + line number. Without it crash dumps are nearly useless.

## 4. Run

Install the .vpk with VitaShell. vitaGL needs `libshacccg.suprx` on the console (install it with ShaRKF00D).
The game checks for it and shows a message instead of crashing. Also runs in Vita3K.

Every boot writes `ux0:data/BackroomsIris/log.txt`. After a crash its last line shows how far startup got.

### Crash tracker + safe mode (0.6.1)

Before each startup / level-load step the game writes the step's name to `ux0:data/BackroomsIris/boot.txt`.
A few seconds after it's running fine it writes `menu-ok` / `play-ok` (and `exit` on a clean quit). If the console
kills the game (C2-12828-1 etc.), the next boot finds an unfinished step there, and then:

- the title screen says **SAFE MODE - LAST RUN CRASHED AT: <STEP>** (that step is the bug report),
- **safe mode** turns on and stays on: no MSAA, more RAM headroom for the system, no sound, no chunk worker
  thread, no mipmaps, 256 px textures, shorter view distance. Settings > SAFE MODE turns it off again (applies on
  the next start).

Steps, in order: `libshacccg check`, `clocks`, `vitagl init`, `sdl init`, `audio init`, `renderer init`,
`network init`, `menu setup`, `first frame`, `level N textures`, `level N texture upload`, `level N first draw`,
`level N meshes`, `level N playing`. The log also records free memory at boot and after vitaGL starts.

## Optional: PC build (Linux) to check the code compiles

```bash
cmake -S . -B build -G Ninja && cmake --build build && ./build/backrooms-iris
```
