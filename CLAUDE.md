# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Writing style — hard rule

Applies to everything: terminal replies, commit messages, code comments, docs.
Breaking it is a defect, not a style preference. The author has context this
file does not capture; writing that pads, hedges or flatters wastes their time.

Banned outright:

- Sycophantic openers. "You're right", "Great question", "Good catch", "Fair",
  "Exactly", "That's a great point". Answer the question.
- Self-flagellation. "I should have", "I under-weighted that", "my mistake",
  "apologies for". State the correction in a clause and continue.
- "Worth noting", "worth flagging", "it's worth", "note that". If it were not
  worth saying it would not be in the message. Say it.
- "Let me ..." narrating a tool call before making it. Make the call.
- Unsolicited closing offers. "Want me to ...?", "Let me know if ...", "Happy
  to ...". Ask only when actually blocked. Otherwise stop talking.
- Recapping work the output above already shows.
- "Not just X, but Y". "It's not X — it's Y". "X isn't the point, Y is."
- Intensifiers used as filler: genuinely, truly, actually, really,
  deliberately, concretely, importantly, fundamentally, simply.
- "Here's the thing", "The thing is", "That's the thing that".
- Essay-slop vocabulary: robust, comprehensive, seamless, leverage, delve,
  crucial, vital, landscape, realm, testament to, dive into, unpack.
- Rule-of-three triads when two items or four would be honest.
- Announcing structure: "Two things:", "A few notes:", "Three takeaways".
- **Measured performance numbers in this file or in `docs/`.** No fps, no
  timings, no throughput comparisons. This machine has heavy run-to-run
  variance and is in use while Claude works, so any figure recorded is a lie
  by the time it is read. Say what is fast or slow and why; measure in the
  moment when it matters. Deterministic facts — `sizeof`, alignment, colour
  values, versions — are not this and belong here.

Constrained, not banned:

- Em dashes: one per paragraph at most. A full stop usually works better.
- Bold: real emphasis on a phrase, never a label opening every bullet.
- Tables: data with actual columns. Not two items with one property each.
- Headers: only when the reader will skip between sections. A short answer has
  none.
- Bullets: prose is the default. Bullets are for things that enumerate.

What to do instead: lead with the answer. Give the evidence that supports it.
Say what is uncertain once, plainly, without hedging twice. Match the length to
the question — a yes/no question can take a one-line answer. Swearing is fine.

## Project state

This is the **second** Encke. The first was built on a custom TypeScript-to-
native compiler; this one is C++. Design decisions carried over from v1 will not
be visible in this repository's code or history, so ask rather than infer intent.

`encke` is a Vulkan renderer and the start of a space sim: clustered deferred
shading with cascaded and spot shadows, a physically based atmosphere, TAA,
auto-exposure; an Earth-sized planet of procedural terrain, meshed with
Surface Nets in a camera-following octree and geomorphed between LODs; Jolt
physics on the terrain, with a walking character and ships that carry their
own physics space aboard; glTF models through fastgltf; a Dear ImGui overlay.
The test scene is a floor flattened into the ground 930 km from the north
pole, with props, lamps, the Khronos helmet on a table, and the torchship
standing on its tail beside the field.

## Where the detail lives

Each subsystem's design, reasoning and known gaps are in `docs/`. **Read the
matching file before changing a subsystem**, and keep it current when the
design changes: a new decision, a gap closed or found.

| File | Covers |
| --- | --- |
| `docs/renderer.md` | the passes, geometry pool and indirect draws, frames in flight, staging, colour and the UI fork, clustering, debug views and drawing, shadows, atmosphere, exposure, TAA, shaders, bindless |
| `docs/terrain.md` | noise and its bit-exactness, the FastNoise2 port, the example planet, the octree, geomorph and seams, modifiers, ground materials, surface maps and impostors |
| `docs/physics.md` | stepping, determinism, terrain collision, static colliders, ship spaces, the walking character, camera control |
| `docs/scene-and-assets.md` | the registry and the extract, transforms, assets and streaming, textures and UVs, glTF, the test scene |
| `docs/dependencies.md` | mimalloc and why it is built as it is, Jolt's build, glaze, ImGui, volk, and the rest's traps |

```
src/
  main.cpp            entry point; constructs App and nothing else
  app.{hpp,cpp}       composition root: owns everything, runs the frame loop
  core/               pch, types prelude, log, the allocator hook, ASan options
  platform/           window and events (SDL3), cpu topology, thread priority/names, worker pool
  vulkan/             context, device, VMA allocator, bindless set, buffers, images, textures,
                      staging arena, swapchain, timestamps
  ui/                 ImGui layer, the forked ImGui Vulkan backend, stats and image windows
  world/              Transform and WorldTransform; Star, Body, Atmosphere
  assets/             handles, AssetManager, the asset worker, Model
  render/             camera, fly and walk cameras, scene, components, extract, debug lines,
                      materials, glTF, pixels, meshes, geometry pool, config, shadows,
                      gpu types, pipelines, renderer
  physics/            components (RigidBody, StaticCollider, ShipSpace, ShipShape), PhysicsWorld
  scenario/           scenario files read with glaze
  terrain/            noise, macro field, terrain field, modifiers, Surface Nets, ground,
                      surface maps, planet and octree, benchmark (--headless, --sweep)
shaders/              Slang: one file per pass family; lib/ holds what several share
tests/                Catch2, mirroring src/
scenarios/            scenario files, tracked; compare.ps1; out/ and reference/ gitignored
docs/                 subsystem design; see above
ports/                vcpkg overlay ports: fastnoise2 (two patches), joltphysics
assets/               textures/ (ambientCG), models/, torchship/; binaries in Git LFS
```

**Includes are always full paths from `src/`** — `#include "vulkan/device.hpp"`,
never `"device.hpp"`, even between files in the same directory. Headers sit
beside their `.cpp`; there is no `include/` tree.

`App`'s members are declared window-first so they destruct in reverse: UI,
renderer, swapchain, device, instance, window. `~App` calls `wait_idle()`
first, and every `shutdown()` checks its handle, so a partially constructed
`App` tears down correctly.

Asset binaries under `assets/` (`.png`, `.jpg`, `.bin`, `.glb`, `.ktx2`) go
through Git LFS; history before that commit holds the JPGs and the helmet as
plain blobs and was not rewritten.

## Rules that break silently

Each of these has cost a debugging session. They fail without an error.

- World space is f64; the GPU only sees view space. Every world -> view
  transform is composed in f64 on the CPU, which cancels the large
  translations, and only the small result is narrowed. Lights and shadow
  maps likewise. A shader that receives a world-space position is a bug. The
  planet's pole is at (1e6, 2.5e5, -7e5) on purpose; set `kWorldOrigin` in
  `render/scene.cpp` to zero and the render must not change.
- Depth is reversed-Z on `D32_SFLOAT`, and four things must agree:
  `glm::perspective` with near and far swapped, clear to `0.0`
  (`DepthTarget::kClearDepth`), compare `GREATER_OR_EQUAL`, the float
  format. Break one and geometry vanishes or z-fights; a normal projection
  with `GREATER` draws far surfaces, which back-face culling can disguise.
  Shadow maps are reversed too, so their bias is negative.
- The Y flip is a negative-height viewport (`flipped_viewport()`);
  projections stay conventional. Geometry winds counter-clockwise with +Y
  up, culled with `frontFace = COUNTER_CLOCKWISE`, which holds because of
  the flip; settled by experiment.
- `SV_VulkanInstanceID` and `SV_VulkanVertexID`, **never** `SV_InstanceID`
  or `SV_VertexID`: Slang lowers those to index minus base, so every draw
  reads object 0 or the first mesh's vertices.
- slangc runs with `-matrix-layout-column-major`, or every GLM matrix is
  silently transposed.
- `view_at` in `shaders/lib/cluster.slang` removes TAA's jitter, and
  `distance_from_depth` there is the one depth -> distance conversion.
  Cluster addressing lives only there too. Reconstruct view positions
  through them, never inline.
- Shader colour output is linear; the swapchain is `_SRGB` and encodes.
  Author in sRGB and call `srgb_to_linear`.
- Bindless binding 2 is read-only buffers, 4 writable and compute only;
  a sampled slot is always `READ_ONLY_OPTIMAL`, a storage slot `GENERAL`,
  and barriers must land there. `vulkan/bindless.hpp` and
  `shaders/lib/bindless.slang` must agree; only validation checks.
- All device memory goes through VMA, and every `vkCreate*` and its
  `vkDestroy*` pass `memory::vulkan_callbacks()`.
- Terrain samples are bit-exact across chunks because every chunk runs
  the same operations in the same order: no reordering octaves, no
  per-chunk vectorisation differences, `-ffp-contract=off` on the terrain
  sources and on everything including Jolt. Voxel sizes and the macro
  lattice spacing are powers of two.
- Jolt is deterministic only given the same calls in the same order, body
  creation and removal included: add bodies in registry or grid order,
  never in the order the worker pool finishes.
- Dynamic rendering and synchronization2 only: no `VkRenderPass`, no
  `VkFramebuffer`, and device selection requires every feature used, so
  nothing needs a fallback.

## Scenarios: how changes are verified

`encke --scenario file.json` runs settings, then steps in order, then quits,
exit code 1 if a step failed. Files live in `scenarios/`; captures go to
`scenarios/out/<name>/`, and `scenarios/compare.ps1` compares them with
`scenarios/reference/<name>/` byte for byte, then pixel by pixel with a diff
image. `-Accept` promotes out to reference; `-A x.png -B y.png` compares two
files.

- **Verify visible changes with a scenario, not by eye.** Run the covering
  scenarios before a change (accepting captures if there is no reference)
  and after it. An unexpected diff is a regression until explained; an
  expected one is shown to the user with its diff image before acceptance.
- **Write a scenario for every new feature and every bug found by eye**,
  from F3 poses (F3 copies the camera as a `camera` step).
- **Add ops and settings freely** in `src/scenario` and `App::run_step`,
  with a case in `tests/scenario/scenario_test.cpp`. Never work around a
  missing op with an env var, flag or temporary code.
- `settle` is what makes captures reproducible: at least `min_frames`, then
  the octree showing its leaves, every asset landed, streaming idle, every
  body asleep. `capture` does not settle; it discards TAA's history and
  captures one jitter cycle later. Put a `settle` before it.
- A scenario pins animation at t = 0, hides the overlay, and ignores the
  keyboard and mouse (the machine is shared). Physics steps once a frame
  under a scenario. Steps run within a frame until one waits, before
  anything that frame updates.
- `fly` runs with the octree live, so its runs differ; `walk` counts physics
  steps. `spawn` drops a grid of boxes or spheres.
- Positions are in the test scene's frame: metres from the middle of the
  floor, +Y its up.
- Two runs of a scenario are byte-identical; keep it that way.
- **Do not run a scenario ending in `interactive` unattended**: it never
  quits (`boxes.json` is one).
- Clustered against brute force (`debug_views.json`) is the check for any
  lighting or clustering change.
- Under clang-sanitize the binary needs `F:\msys2\clang64\bin` on PATH for
  the ASan runtime, or it exits with `0xC0000135`.

## Toolchain and build

Windows host, **MSYS2 toolchains**, not MSVC: GCC 16.2.0 at
`F:/msys2/ucrt64/bin`, Clang 22.1.8 at `F:/msys2/clang64/bin`, Ninja, vcpkg
triplet `x64-mingw-static`, `VCPKG_ROOT` in the environment
(`F:/Programming/vcpkg`). The MSVC branches in `CMakeLists.txt` are
untested.

**Use `.claude\cmake.ps1`, not raw `cmake`**: it puts the preset's toolchain
on PATH (see the PATH trap).

```powershell
.claude\cmake.ps1                                  # clang-sanitize, incremental
.claude\cmake.ps1 -Run                             # build, then execute
.claude\cmake.ps1 -Test                            # build, then ctest
.claude\cmake.ps1 -Preset release -Run             # perf check
.claude\cmake.ps1 -Target encke                    # a specific target
build\clang-sanitize\encke_tests.exe "[camera]"    # one test tag
```

- **Build `clang-sanitize` and nothing else**, unless the user asks or a
  reason below applies: `release` for measuring performance (`-O3`, no
  `_GLIBCXX_ASSERTIONS`, which block vectorisation), `gcc-debug` before a
  commit that touched build flags or headers. A three-preset sweep is
  minutes for nothing; a `CMakeLists.txt` change is not a reason.
- **Do not pass `-Configure`** unless something is stale: it re-runs
  `vcpkg install`, minutes even warm. CMake re-runs itself when
  `CMakeLists.txt` changes. Changing a preset's cache variables, or editing
  an overlay port (bump its `port-version`), does need it. Prefer
  incremental builds over `-Clean`.
- `.claude/` is gitignored; the script hardcodes MSYS2 paths.
- `clang-sanitize` lives in the gitignored `CMakeUserPresets.json`;
  `clang-sanitize-base` in `CMakePresets.json` supplies the flags. Presets
  set `CMAKE_CXX_FLAGS` wholesale, so a flag added to one reaches no other.
- Tests are Catch2 in `tests/`, one `<name>_test.cpp` per unit, each listed
  in `encke_tests`. Every source but `main.cpp` builds into `encke_core`, an
  OBJECT library, so the replacement `operator new` in `core/memory.cpp` is
  always linked. `catch_discover_tests` runs in `PRE_TEST` mode.
- `compile_commands.json` is exported per preset, for clangd.

### The PATH trap

A bare `cmake --build` from a terminal fails with no diagnostic, only
`FAILED: ... main.cpp.obj`. `c++.exe` runs fine on a bare PATH, but
`cc1plus.exe` in `lib\gcc\x86_64-w64-mingw32\16.2.0\` cannot find
`libgmp-10.dll`, `libisl-23.dll`, `libmpc-3.dll`, `libmpfr-6.dll` and dies with
`0xC0000135`. Prepending `F:\msys2\ucrt64\bin` fixes it. The loader error
names `api-ms-win-crt-*.dll`, which is a red herring. CLion is unaffected, as
is clang, which compiles in-process.

## Code rules

- **C++23**, no extensions, hidden visibility, PIC; AVX2/FMA is a baseline
  (`-mavx2 -mfma`), so no pre-Haswell CPUs.
- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
  -Wshadow -Wold-style-cast`. Spell out narrowing and signed/unsigned casts;
  no C-style casts. Not errors, but the build stays clean. Clang warns on
  designated initialisers that skip a field: name them all.
- `std::array` needs double braces for GCC.
- GLM, measured against 1.0.3 under this project's flags:
  - `GLM_FORCE_DEPTH_ZERO_TO_ONE` is set; without it depth is OpenGL's -1..1.
    Matrices are column-major, right-handed. The Y axis is fixed by the
    viewport, not a flag.
  - `GLM_FORCE_EXPLICIT_CTOR`: no implicit conversions between vector types.
    `GLM_FORCE_SIZE_T_LENGTH`: `.length()` is `size_t`.
  - **No GLM type is `constexpr`**: `GLM_FORCE_AVX2` forces
    `GLM_HAS_CONSTEXPR` to 0. A `constexpr f32vec3` is a compile error; use
    `const`.
  - Default types are packed (`vec3` is 12 bytes, align 4), so vertex
    structs upload as written. `aligned_vec3`/`aligned_vec4` opt into SIMD
    per type, and only they reach GLM's SIMD paths; geometric functions are
    SIMD only for 4 components. **Never add
    `GLM_FORCE_DEFAULT_ALIGNED_GENTYPES`**: it silently pads vertex structs.
  - A quaternion constructs `(w, x, y, z)` but stores `x, y, z, w`.
  - `GLM_ENABLE_EXPERIMENTAL` is on for `gtx/`, which has no stability
    promise between releases.
- A glaze-reflected type must not be in an anonymous namespace.
- The one asset worker and the terrain worker pool are the only threads
  besides the main thread and Jolt's; only the main thread touches the
  registry and the `AssetManager`.
