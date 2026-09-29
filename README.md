# Encke

A Vulkan renderer for a space sim at real scale, written in C++23. This is the
second Encke; the first was built on a custom TypeScript-to-native compiler.

The renderer is clustered deferred: cascaded and spot shadow maps, a lean
G-buffer, compute light clustering, compute lighting, a physically based
atmosphere that holds from the ground to orbit, TAA, histogram auto-exposure
and a choice of tonemap curves. World space is f64 on the CPU and the GPU only
ever sees view-space positions, so a scene can hold instruments centimetres from
the eye and planets millions of metres away without jitter.

Physics is Jolt, built double precision and deterministic, colliding with the
terrain it meshes on demand. You walk on the ground as a capsule character, or
fly a jetpack. A ship carries its own physics space aboard, fixed to its frame,
so what is inside keeps simulating in the ship's gravity wherever the ship goes,
and bodies and the character move between the ship and the world through its
doors.

The test scene stands 930 km from the north pole of an Earth-sized planet. The
planet is procedural terrain, FastNoise2 macro graphs plus hand-summed Perlin
detail, meshed with Surface Nets on a worker pool in an implicit octree that
follows the camera and geomorphed between levels of detail, with climate-driven
ground materials and a baked impostor for far off. On a floor levelled into it
stand boxes, spheres, CC0 PBR materials from ambientCG, the Khronos
DamagedHelmet, and the torchship, a 35 m, 270 t ship with walkable decks. They
are lit by a real-magnitude Sun low on the horizon, shadowed spot lights and a
ring of point lamps, with the Moon at its real distance. A Dear ImGui overlay
shows frame and per-pass GPU timings.

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
- Git, since CPM clones some dependencies at configure time, with
  [Git LFS](https://git-lfs.com/), which holds the textures and models; see
  *Getting the source*.
- A CPU with AVX2 and FMA (Haswell or newer). The binary is built for it.
- A GPU with Vulkan 1.3, dynamic rendering, synchronization2, descriptor
  indexing, `multiDrawIndirect`, `depthClamp`, `samplerAnisotropy` and
  `shaderStorageImageReadWithoutFormat`. Device selection rejects anything
  less and logs why.

### Getting the source

The binary assets under `assets/` (PNG, JPG, glTF buffers, GLB, KTX2) are
stored with Git LFS. Install it once before cloning, so the clone fetches them:

```sh
git lfs install
git clone https://github.com/king-numsgil/project-encke-cpp.git
```

A clone made without LFS has small text pointer files in their place, and
every texture and model fails to load. Install LFS and fetch them into the
existing clone with:

```sh
git lfs install
git lfs pull
```

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
| glaze | JSON, for scenario files |
| fastnoise2 | terrain noise, from the overlay port in `ports/fastnoise2` |
| joltphysics | physics, double precision and deterministic, from the overlay port in `ports/joltphysics` |
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

The mouse is captured, as in any first-person game; hold Alt to free it for the
UI. You start on foot. X switches between walking and the jetpack.

| Walking | |
| --- | --- |
| mouse | look |
| WASD | walk |
| Shift | sprint |
| Space | jump |

| Jetpack | |
| --- | --- |
| mouse | look, about the camera's own axes |
| WASD | move along the view |
| Space, Ctrl | up and down along the camera's up |
| Q, E | roll |
| Shift, C | five times faster, five times slower |
| wheel | base speed, from 2 m/s to 10,000 km/s |

| Anywhere | |
| --- | --- |
| F1 | hide or show the UI |
| F2 | freeze the terrain's octree, to fly around what it drew |
| F3 | copy the camera's pose as a scenario step |
| F4 | cycle the wireframe: off, over the image, alone |
| F5 | show collision shapes |
| F6 | show the terrain chunks' boxes |
| 1, 2 | clustered or brute-force shading |
| 3 to 6 | debug views: lights per cluster, normals, motion vectors, shadow cascades |
| T | cycle the tonemap curve |
| Escape | quit |

`encke --headless` runs the terrain benchmark without opening a window, and
`encke --sweep [n]` measures every level of detail over n random chunks.

`encke --scenario scenarios/pole.json` runs a scenario: a JSON file of
settings and steps (settle, move the camera, freeze the terrain, fly, walk,
spawn bodies, capture) that runs, writes its captures to
`scenarios/out/<name>/` and quits. `--out dir` puts the captures elsewhere.
`scenarios/compare.ps1` compares the captures with the reference archive in
`scenarios/reference/`, offline; `-Accept` makes them the reference.

## Layout

```
src/        the engine: core, platform, vulkan, ui, world, assets, render, physics, scenario, terrain
shaders/    Slang, compiled to SPIR-V at build time; shared modules in lib/
tests/      Catch2, mirroring src/
scenarios/  scripted runs and captures, and the offline comparison script
ports/      the FastNoise2 and Jolt vcpkg overlay ports, and FastNoise2's patches
assets/     ambientCG textures, glTF models and the torchship, in Git LFS; CREDITS.md says where they came from
docs/       each subsystem's design
cmake/      CPM.cmake
```

`docs/` holds each subsystem's design decisions and the reasons for them;
`CLAUDE.md` holds the rules that break silently and the build workflow.

## Licence

The code is MIT; see `LICENSE`. The torchship in `assets/torchship/` is MIT
too, made for this project. The other assets are not covered by it: each carries
its own licence, listed in `assets/textures/CREDITS.md` and
`assets/models/CREDITS.md`. The textures are CC0 from
[ambientCG](https://ambientcg.com/). The DamagedHelmet, from the Khronos glTF
sample models, is **CC-BY-NC-4.0**, so it must not be used commercially.

The forked ImGui Vulkan backend in `src/ui/imgui_vulkan.*` keeps ImGui's MIT
notice. Dependencies keep their own licences, and since they link statically, a
binary release must ship their licence texts.
