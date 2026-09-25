# Encke

A Vulkan renderer for a space sim at real scale, written in C++23. This is the
second Encke; the first was built on a custom TypeScript-to-native compiler.

The renderer is clustered deferred: cascaded and spot shadow maps, a lean
G-buffer, compute light clustering, compute lighting, histogram auto-exposure
and a choice of tonemap curves. World space is f64 on the CPU and the GPU only
ever sees view-space positions, so a scene can hold instruments centimetres from
the eye and planets millions of metres away without jitter.

The test scene is the north pole of an Earth-sized planet. The planet is
procedural terrain, FastNoise2 macro graphs plus hand-summed Perlin detail,
meshed with Surface Nets on a worker pool in an implicit octree that follows the
camera. On it stand boxes, spheres, CC0 PBR materials from ambientCG and the
Khronos DamagedHelmet, lit by a real-magnitude Sun low on the horizon, shadowed
spot lights and a ring of point lamps, with the Moon overhead at its real
distance. A Dear ImGui overlay shows frame and per-pass GPU timings.

## Requirements

Encke builds on Windows with the MSYS2 toolchains. MSVC is not supported.

- [MSYS2](https://www.msys2.org/) with the **UCRT64** toolchain (GCC) and,
  for the sanitizer build, the **CLANG64** toolchain:
  ```sh
  pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
  pacman -S mingw-w64-clang-x86_64-toolchain
  ```
  CMake 3.25 or newer and Ninja are needed.
- The [Vulkan SDK](https://vulkan.lunarg.com/). Configure fails without it:
  shaders are compiled at build time with the SDK's `slangc`, and the
  validation layers come from it.
- [vcpkg](https://github.com/microsoft/vcpkg), with `VCPKG_ROOT` set in the
  environment. Every preset takes its toolchain file from there.
- Git, since CPM clones some dependencies at configure time.
- A CPU with AVX2 and FMA (Haswell or newer). The binary is built for it.
- A GPU with Vulkan 1.3, dynamic rendering, synchronization2, descriptor
  indexing, `multiDrawIndirect`, `depthClamp`, `samplerAnisotropy` and
  `shaderStorageImageReadWithoutFormat`. Device selection rejects anything
  less and logs why.

### Dependencies

Nothing is vendored except `cmake/CPM.cmake`. The first configure fetches and
builds everything, which takes several minutes; later builds reuse it.

From vcpkg, in manifest mode (`vcpkg.json`, baseline pinned in
`vcpkg-configuration.json`), triplet `x64-mingw-static`:

| Package | Used for |
| --- | --- |
| volk, vulkan (headers) | Vulkan entry points; nothing links the loader directly |
| vulkan-memory-allocator | all device memory |
| sdl3 `[vulkan]` | window, input, events |
| sdl3-image `[jpeg, png]` | texture decoding |
| glm | maths |
| fastgltf | glTF loading |
| entt | the scene registry |
| fastnoise2 | terrain noise, from the overlay port in `ports/fastnoise2` |
| cpuinfo | physical core count, to size the worker pool |
| bshoshany-thread-pool | thread priority and naming only |
| catch2 | tests |

From CPM, cached in `.cpm/`:

| Package | Why not vcpkg |
| --- | --- |
| mimalloc 3.5.3 | it needs a define on MinGW that vcpkg's scrubbed environment cannot pass |
| Dear ImGui v1.92.9b-docking | the vcpkg port links the Vulkan loader, which collides with volk |
| ImPlot v1.0 | must build against the same ImGui |

## Building

Three configure presets live in `CMakePresets.json`:

| Preset | Compiler | For |
| --- | --- | --- |
| `gcc-debug` | GCC (UCRT64) | debug build |
| `release` | GCC (UCRT64) | `RelWithDebInfo`, for measuring performance |
| `clang-sanitize` | Clang (CLANG64) | ASan and UBSan; the everyday build |

**The toolchain's `bin` directory must be on `PATH`** when building from a
terminal. Without it GCC's `cc1plus.exe` cannot find its GMP/ISL/MPC/MPFR DLLs
and dies with no message, and the build stops at the first object with a bare
`FAILED`. IDEs that know about the MinGW toolchain (CLion) set this themselves.

With GCC, from PowerShell:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake --preset gcc-debug
cmake --build --preset gcc-debug
build\gcc-debug\encke.exe
```

Substitute your MSYS2 install path, and `release` for `gcc-debug` for an
optimised build.

`clang-sanitize` inherits a hidden base preset and needs the compiler paths,
which are machine-specific, so it goes in a `CMakeUserPresets.json` beside
`CMakePresets.json` (gitignored):

```json
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 25, "patch": 0 },
  "configurePresets": [
    {
      "name": "clang-sanitize",
      "inherits": "clang-sanitize-base",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "C:/msys64/clang64/bin/clang.exe",
        "CMAKE_CXX_COMPILER": "C:/msys64/clang64/bin/clang++.exe"
      }
    }
  ],
  "buildPresets": [
    { "name": "clang-sanitize", "configurePreset": "clang-sanitize" }
  ]
}
```

Then build it the same way, with `C:\msys64\clang64\bin` on `PATH`; the ASan
runtime DLL is loaded from there when the program runs, too. mimalloc is off in
this preset, since ASan cannot see allocations mimalloc makes.

vcpkg builds its ports with whichever compiler `PATH` offers, so the
dependencies of each build directory come from the toolchain that was on
`PATH` when it was first configured.

### Tests

```powershell
ctest --test-dir build\gcc-debug --output-on-failure
build\gcc-debug\encke_tests.exe "[camera]"     # one tag
```

### Running

`build/<preset>/encke.exe` opens the window. Shaders are found beside the
executable; textures and models are read from `assets/` in the source tree,
whose path is baked in at configure time.

Hold the right mouse button to fly: mouse to look, WASD to move, Space and Ctrl
up and down, Q and E to roll, Shift and Alt for faster and slower, the wheel to
change base speed. F1 hides the UI, keys 1 and 2 switch between clustered and
brute-force shading, 3 to 6 open the debug views (lights per cluster, normals,
motion vectors, shadow cascades), T cycles the tonemap curve.

`encke --headless` runs the terrain benchmark without opening a window.

Some environment variables, mostly for reproducible captures:

| Variable | Effect |
| --- | --- |
| `ENCKE_CAPTURE=out.png` | write frame `ENCKE_CAPTURE_FRAME` (default 10) to a PNG, then quit |
| `ENCKE_NO_UI` | start with the overlay hidden |
| `ENCKE_FIXED_TIME` | pin animation time |
| `ENCKE_CAMERA="px py pz tx ty tz [ux uy uz]"` | start at p looking at t, metres from the pole |
| `ENCKE_TONEMAP` | 0 ACES, 1 AgX, 2 PBR Neutral (default) |
| `ENCKE_EV100` | pin the exposure |
| `ENCKE_DEBUG_VIEW` | start with a debug view, numbered from 0 |
| `ENCKE_UI_SRGB` | force the UI onto the sRGB swapchain view |

## Layout

```
src/        the engine: core, platform, vulkan, ui, world, assets, render, terrain
shaders/    Slang, compiled to SPIR-V at build time; shared modules in lib/
tests/      Catch2, mirroring src/
ports/      the FastNoise2 vcpkg overlay port and its patches
assets/     ambientCG textures and glTF models; CREDITS.md in each says where they came from
cmake/      CPM.cmake
```

`CLAUDE.md` holds the design decisions and the reasons for them in detail.

## Credits

Textures are CC0 from [ambientCG](https://ambientcg.com/). The DamagedHelmet
is from the Khronos glTF sample models. Licences and sources are in
`assets/textures/CREDITS.md` and `assets/models/CREDITS.md`. The forked ImGui
Vulkan backend in `src/ui/imgui_vulkan.*` keeps ImGui's MIT notice.
