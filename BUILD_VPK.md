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

Version 0.3.0 (network protocol 3). 0.3 adds a chunk-building worker thread (std::thread, so the Vita target links
`pthread`) and more procedural textures (all power-of-two, generated at level load: expect ~2-3 s per level on the Vita).

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

## 4. Run

Install the .vpk with VitaShell. vitaGL apps usually need `libshacccg.suprx` on the console
(install it with ShaRKF00D). Also runs in Vita3K.

## Optional: PC build (Linux) to check the code compiles

```bash
cmake -S . -B build -G Ninja && cmake --build build && ./build/backrooms-iris
```
