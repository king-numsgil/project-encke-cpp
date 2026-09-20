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
- **Measured performance numbers in this file.** No fps, no timings, no
  throughput comparisons. This machine has heavy run-to-run variance and is in
  use while Claude works, so any figure recorded here is a lie by the time it is
  read, and stale again an hour later as features land. Say what is fast or slow
  and why; measure in the moment when it matters. Deterministic facts —
  `sizeof`, alignment, colour values, versions — are not this and belong here.

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

`encke` is a Vulkan renderer. It draws a gouraud-shaded triangle from a Slang
shader, survives resize, and reports throughput. There are no vertex buffers,
descriptors, textures or depth yet — the triangle's positions come from
`SV_VertexID`.

```
src/
  main.cpp            entry point; constructs App and nothing else
  app.{hpp,cpp}       composition root: owns everything, runs the frame loop
  core/
    pch.hpp           precompiled header
    types.hpp         global type prelude
    log.{hpp,cpp}     unbuffered stderr diagnostics, millisecond stamps
  platform/
    window.{hpp,cpp}  SDL3 init, window, event pump -> FrameEvents
  vulkan/
    context.{hpp,cpp} volk, instance, validation, surface
    device.{hpp,cpp}  physical device selection, logical device, queues
    swapchain.{hpp,cpp}  swapchain, images, views, recreation
  render/
    pipeline.{hpp,cpp}   shader module loading, VkPipeline construction
    renderer.{hpp,cpp}   command pool, per-frame sync, record and present
shaders/
  triangle.slang         compiled to SPIR-V at build time by slangc
  lib/
    screen.slang         fragment/NDC/UV conversions and the Y conventions
    colour.slang         sRGB <-> linear, luminance
```

**Includes are always full paths from `src/`** — `#include "vulkan/device.hpp"`,
never `"device.hpp"`, even between files in the same directory. `src` is the
only include root, so a bare name would be ambiguous about where it lives.
Headers sit beside their `.cpp`; there is no separate `include/` tree, because
nothing here is consumed as a library.

`App`'s members are declared window-first so they destruct in reverse: renderer,
swapchain, device, instance, window. `~App` calls `wait_idle()` before any of
that runs. Every `shutdown()` checks its handle, so a partially constructed
`App` tears down correctly.

## Rendering decisions already made

- **Dynamic rendering, not `VkRenderPass`.** Core in Vulkan 1.3. There is no
  `VkRenderPass` or `VkFramebuffer` anywhere, and there should not be.
- **synchronization2** for barriers and submits (`vkCmdPipelineBarrier2`,
  `vkQueueSubmit2`). Device selection *requires* both features, plus
  `shaderDrawParameters`, and rejects any GPU lacking them, so none of them
  needs a fallback path.
- **Viewport and scissor are dynamic state**, so a resize does not invalidate
  the pipeline. The colour format does, since dynamic rendering bakes it in.
- **The Y flip lives in the viewport**, via `flipped_viewport()` — negative
  height, set per frame. Projection matrices stay conventional.
- **Back-face culling is on with `frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE`,
  and geometry is wound counter-clockwise in NDC with +Y up.** That is the
  glTF convention, and it holds *because* of the negative viewport: the flip
  cancels against Vulkan's y-down framebuffer. Settled by experiment, not
  derivation — a clockwise-wound triangle is culled under this setting and a
  counter-clockwise one survives, which also proves culling is live rather
  than silently disabled.
- **Two frames in flight.** `image_available` semaphores and fences are
  per-frame; `render_finished` semaphores are **per swapchain image**, because
  a frame index maps to a different image over time and signalling a semaphore
  that still has a pending wait is invalid.
- **MAILBOX present mode where offered**, FIFO otherwise. FIFO is the only mode
  guaranteed to exist.
- **The clear colour is linear.** The swapchain is `B8G8R8A8_SRGB`, so the
  hardware encodes on write. Linear `(0.03, 0.12, 0.18)` lands as sRGB
  `(48, 97, 118)` on screen — verified by screen capture, not assumed.
- The swapchain is only rebuilt when the window size actually differs, since a
  resize event also fires once at startup for the initial size.

The frame loop reports throughput once a second. Treat it as a relative signal
within a single session, never as a figure worth recording.

## Renderer architecture — decided, not yet built

**Clustered deferred**, with a forward pass for transparency. Decided
2026-09-20 after weighing it against Forward+ and the visibility buffer.

The previous three iterations of Encke were all Forward+. Part of the reason
for going deferred is to not build the same renderer a fourth time, and that is
a legitimate reason — do not "correct" it back toward Forward+ on efficiency
grounds. The hardware case also holds: Pascal has no hardware ray tracing, so
screen-space techniques carry GI and reflections and those want a G-buffer, and
Pascal's weak async compute blunts one of Forward+'s real advantages.

Clustered light culling carries over from the Forward+ iterations, so the
genuinely new work is the G-buffer and the lighting pass.

### G-buffer rules

- **Keep it lean.** The 1070 has ~256 GB/s and a fat G-buffer spends it. Fine
  at 1080p, the first thing to optimise at 4K.
- **Octahedral-pack normals into RG16.** Not RGBA32F.
- **Motion vectors from the first version.** RG16F, previous-frame clip
  position minus current. Committed deliberately at design time because
  retrofitting them means touching every shader and re-deciding the layout.

### Antialiasing

Motion vectors exist for **TAA**, which is the intended approach: it handles
shading aliasing (specular, normal maps) that MSAA cannot, and deferred makes
MSAA expensive since the G-buffer would need to be sample-rate. TAA is not
formally locked — the commitment so far is the motion vectors, which every
candidate wants. The same buffers later feed temporal SSAO/SSR denoising and
FSR-style upscaling.

### Visibility buffer — considered, set aside

Store instance + triangle ID, resolve material in a full-screen pass. Rejected
for now, not from ignorance:

- The bandwidth win scales with resolution and geometric density, and is modest
  at 1080p with hand-authored geometry.
- The real advantage is that no fixed G-buffer layout constrains materials.
  That one applies at any scale, and is the reason to revisit this if the
  material system starts fighting the layout.
- Costs: mandatory bindless, analytic UV derivatives (hardware `ddx`/`ddy` are
  wrong across triangle boundaries in a full-screen pass), and per-material
  binning with indirect dispatch to avoid divergence.

## Shaders

**Slang**, compiled to SPIR-V at build time. `slangc` ships with the Vulkan SDK,
so it adds no dependency to fetch, and `slangd.exe` gives CLion a language
server over LSP. Nothing links against Slang at runtime.

`encke_add_shader()` in `CMakeLists.txt` drives it. **The build now requires the
Vulkan SDK** — `find_program` fails loudly at configure time rather than leaving
something cryptic for link time. That was the open question and this is the
answer: accepted, not worked around. The SDK is needed for validation layers and
`spirv-val` regardless.

- **One `.slang` file produces one `.spv` holding every `[shader(...)]` entry
  point in it.** `-fvk-use-entrypoint-name` keeps their names; without it they
  all become `main` and the pipeline cannot distinguish them.
- **`.spv` lands in `shaders/` beside the executable**, and the runtime resolves
  it against `SDL_GetBasePath()`.
- **`SV_VertexID` requires `shaderDrawParameters`.** Slang lowers it to
  `gl_VertexIndex` and emits the SPIR-V `DrawParameters` capability. Device
  creation enables the feature and selection requires it; without it
  `vkCreateShaderModule` is a spec violation that drivers accept silently and
  validation rejects.
- **Shader colour output is LINEAR.** The swapchain is `_SRGB` so the hardware
  encodes on write, and values written look considerably lighter than the
  numbers suggest. Author in sRGB and call `srgb_to_linear` rather than
  hand-computing linear constants, which leaves the intent unreadable.

### Shared modules

`shaders/lib/` holds Slang modules imported with `import lib.<name>;`, resolved
by `-I shaders`. **Conventions that more than one shader depends on belong
there, not open-coded per shader.**

| Module | Holds |
| --- | --- |
| `lib/screen.slang` | fragment ↔ NDC ↔ UV conversions, and the Y-direction rules they encode |
| `lib/colour.slang` | sRGB ↔ linear, luminance |

`lib/screen.slang` matters most. NDC has +Y up while `SV_Position` and every
render target are addressed top-left with +Y down, so screen-space work crosses
that boundary constantly — and an inverted screen-space effect looks plausible
enough to ship. One helper, used everywhere, rather than a flip rederived per
shader.

slangc writes a depfile and the custom command consumes it, so editing a module
rebuilds every shader that imported it without any dependency listed by hand.


## Allocator

mimalloc backs `operator new`/`delete` (via `mimalloc-new-delete.h` in
`src/core/memory.cpp`, which must stay the only translation unit that includes
it), SDL3 through `SDL_SetMemoryFunctions`, and Vulkan host allocations through
`memory::vulkan_callbacks()`.

`ENCKE_USE_MIMALLOC` gates all of it. When off, `vulkan_callbacks()` returns
`nullptr`, which is exactly what Vulkan reads as "use the driver's allocator",
so no call site needs a branch.

**Every `vkCreate*` must pass `memory::vulkan_callbacks()` and so must its
matching `vkDestroy*`.** Vulkan requires compatible callbacks at both ends and
validation reports each mismatch individually. Five destroy sites were missed on
the first pass and validation caught all five.

### Off under the sanitizers, on purpose

`clang-sanitize` sets `ENCKE_USE_MIMALLOC=OFF`. This is not a workaround.
Measured with an identical deliberate use-after-free:

| Allocator | Result |
| --- | --- |
| system | ASan reports `heap-use-after-free` with a stack trace, exit 1 |
| mimalloc | reads freed memory, prints garbage, exit 0, no report at all |

mimalloc takes memory from its own OS arenas, so ASan never sees the
allocation and cannot place redzones around it. `MI_TRACK=ASAN` exists to make
mimalloc ASan-aware, but it is unreachable through vcpkg (the port exposes only
`override` and `secure`) and would need mimalloc compiled with ASan by the same
compiler, while vcpkg builds dependencies with ucrt64 GCC and the preset uses
clang64. Even if built, ASan's own allocator is the stronger detector — it has
redzones and a free quarantine that mimalloc has no equivalent for.

### Built by CPM, not vcpkg, and why

mimalloc 3.5.3 comes from `CPMAddPackage` in `CMakeLists.txt`, with `CPM.cmake`
vendored at `cmake/CPM.cmake` and sources cached in `.cpm/` (gitignored). It is
the **only** CPM dependency; everything else stays in the vcpkg manifest.

The reason is one build-time define. **`MI_MINGW_UCRT64` must be set or mimalloc
aborts at startup** with `assertion failed: "mi_out_default == NULL"`.
`_mi_auto_process_init()` runs twice: `mi_tls_attach` registers a
`DLL_PROCESS_ATTACH` callback through data sections, and because
`MI_PRIM_HAS_PROCESS_ATTACH` is left undefined on mingw, `src/prim/prim.c` also
installs an `__attribute__((constructor))` calling the same function.

Upstream's CMake sets that define only when `$ENV{MSYSTEM}` is `UCRT64`. vcpkg
scrubs the environment for port builds and the triplet's `VCPKG_ENV_PASSTHROUGH`
lists `PATH` alone, so it never arrives and setting `MSYSTEM` in the preset does
not help. Building mimalloc ourselves is what makes the define reachable.

Confirmed by A/B on one build tree, same compiler and flags, toggling only that
define: with it the app runs clean, without it the identical assertion fires.
Do not remove the `if(MINGW)` block that applies it.

This also puts `MI_SECURE`, `MI_GUARDED` and arena tuning within reach if they
ever matter.

### What it is worth: unmeasured, and that is fine

No throughput claim here survives scrutiny on the current workload, and an
earlier attempt to record one had to be retracted. The renderer performs
essentially no per-frame allocation and the loop is bound by present throughput,
so there is nothing for an allocator to win yet.

mimalloc is wired in ahead of the allocation traffic a scene graph, material
system and per-frame staging will bring, and because threading
`VkAllocationCallbacks` through every create/destroy pair is far cheaper now
than as a retrofit.

If it ever needs measuring: interleave the configurations within one time window
rather than building and running them back to back, and use a workload that
allocates. Sequential A/B on this machine reads drift, not the allocator.

## Toolchain

Windows host, **MSYS2 toolchains**, not MSVC:

- GCC 16.2.0 at `F:/msys2/ucrt64/bin`, Clang 22.1.8 at `F:/msys2/clang64/bin`
- Ninja generator, vcpkg triplet `x64-mingw-static`
- Requires `VCPKG_ROOT` in the environment (currently `F:/Programming/vcpkg`);
  every preset points its toolchain file at `$env{VCPKG_ROOT}`.
- The preset's toolchain `bin` **must** be on PATH or compilation fails with no
  error message — see *The PATH trap*.

`CMakeLists.txt` also carries MSVC branches for warnings and codegen, but no
preset configures MSVC — treat those branches as untested.

## Build

**Use `.claude\cmake.ps1`, not raw `cmake`.** Bare `cmake --build` fails from a
terminal for a non-obvious reason — see *The PATH trap* below. The script sets
PATH correctly for the chosen preset and is the supported way to build here.

```powershell
.claude\cmake.ps1                                  # clang-sanitize, incremental
.claude\cmake.ps1 -Run                             # build, then execute
.claude\cmake.ps1 -Preset release -Run             # perf check
.claude\cmake.ps1 -Target encke                    # a specific target
.claude\cmake.ps1 -Configure                       # force a reconfigure (slow — see below)
```

The script picks the toolchain directory per preset, auto-configures when
`build/<preset>/CMakeCache.txt` is missing, and exits nonzero on any configure
or build failure rather than falling through to `-Run`.

### Build one preset, not all three

**`clang-sanitize` is the default and the one to use while working.** ASan and
UBSan catch what matters during development, and clang's diagnostics are the
better ones to iterate against. Building all three on every change wastes
minutes for nothing.

Reach for the others only when there is a reason:

| Preset | When |
| --- | --- |
| `clang-sanitize` | Default. Everything, unless a row below applies. |
| `release` | Measuring performance. `-Og` numbers are meaningless. |
| `gcc-debug` | Before a commit that touched build flags or headers, to catch GCC-only diagnostics; or when chasing a codegen difference. |

**Build `clang-sanitize` and nothing else.** Not "usually" — always, unless the
user asks for another preset or a row above genuinely applies. A three-preset
sweep is minutes of the user's time for a result that is almost never different,
and a `CMakeLists.txt` change is *not* a reason to run one. Commit on the
strength of one preset; the others get built when someone needs them.

And **do not pass `-Configure` to force it.** CMake re-runs itself when
`CMakeLists.txt` or a preset changes, so a plain build already picks that up.
Forcing it re-runs `vcpkg install` and turns a seconds-long build into a
minutes-long one for nothing.

`.claude/` is gitignored — the script hardcodes machine-specific MSYS2 paths, so
it will not exist in a fresh clone. Recreate it or fall back to raw `cmake` with
`F:\msys2\ucrt64\bin` prepended to PATH manually.

Run directly: `build/<preset>/encke.exe`

`ctest` is enabled (`enable_testing()`) but no tests are registered yet. Once
tests exist: `ctest --test-dir build/gcc-debug -R <name>` for a single test.

### Do not force a reconfigure casually

`-Configure` (and deleting a build directory) re-runs `vcpkg install`, which
costs minutes even with a warm binary cache. Incremental builds need no
reconfigure — CMake re-runs itself
automatically when `CMakeLists.txt` or a preset file changes. Only force it when
something is genuinely stale, and expect the wait.

Likewise, prefer plain incremental builds over `-Clean`. `--clean-first` throws
away every object file for a project whose full rebuild has no upside unless you
are specifically chasing a stale-artifact bug.

### The PATH trap

A bare `cmake --build` in a terminal fails like this — **no diagnostic at all**,
just exit code 1:

```
FAILED: CMakeFiles/encke.dir/main.cpp.obj
ninja: build stopped: subcommand failed.
```

The compiler is not broken and neither is the project. `c++.exe` lives in
`ucrt64\bin` and resolves its DLLs from its own directory, so it runs fine on a
bare PATH — `c++ --version` succeeds, which makes the toolchain look healthy. It
then delegates the actual compile to `cc1plus.exe` in
`lib\gcc\x86_64-w64-mingw32\16.2.0\`, and from *there* `libgmp-10.dll`,
`libisl-23.dll`, `libmpc-3.dll` and `libmpfr-6.dll` are unreachable. `cc1plus`
dies at load with `STATUS_DLL_NOT_FOUND` (`0xC0000135`) before it can print
anything.

Prepending `F:\msys2\ucrt64\bin` to PATH fixes it. That is all `.claude\cmake.ps1`
is doing.

Two traps within the trap:

- The loader error names `api-ms-win-crt-*.dll`, which looks like a broken UCRT
  install. **It is not.** The working `c++.exe` driver imports the exact same
  apisets; those are virtual and the loader resolves them fine. Do not chase
  this — the real missing DLLs are the GMP/ISL/MPC/MPFR set.
- CLion is unaffected, because it puts the toolchain `bin` on PATH itself when
  its MinGW home points at `F:\msys2\ucrt64`. A build that works in the IDE and
  fails in a terminal is this bug, not a project difference.

`clang-sanitize` is immune: clang does codegen in-process rather than spawning a
backend, so it has no equivalent failure mode.

### Preset notes

- `clang-sanitize` lives in `CMakeUserPresets.json`, which is **gitignored** —
  it holds machine-specific compiler paths. `clang-sanitize-base` (hidden, in
  the tracked `CMakePresets.json`) supplies the sanitizer flags; the user preset
  only pins the clang binaries.
- Presets set `CMAKE_CXX_FLAGS` wholesale, and inheritance replaces rather than
  appends. `release` inherits `gcc-debug`'s flag string; `clang-sanitize-base`
  overwrites it. Adding a flag to `gcc-debug` silently changes `release` too,
  and silently does *not* reach `clang-sanitize`.
- `CMAKE_EXPORT_COMPILE_COMMANDS` is on, so `build/<preset>/compile_commands.json`
  drives clangd.

## Dependencies and build settings that constrain code

All deps come from vcpkg manifest mode (`vcpkg.json`, pinned via a baseline in
`vcpkg-configuration.json`): `volk`, `vulkan`, `vulkan-memory-allocator`,
`sdl3`, `glm`.

- **volk owns every Vulkan entry point.** `VK_NO_PROTOTYPES` is defined
  globally and the target links `Vulkan::Headers`, never `Vulkan::Vulkan`.
  Linking the loader's import library alongside volk is a multiple-definition
  error at link time — both define `vkCreateInstance` and friends. Call order
  is `volkInitialize()` → `vkCreateInstance` → `volkLoadInstance()`, and
  `volkLoadDevice()` once a device exists.
- **`src/pch.hpp` is the precompiled header** and must include `volk.h` before
  anything that reaches `vulkan.h`. `SDL3/SDL_vulkan.h` declares its own Vulkan
  handle typedefs unless `VULKAN_H_` is already defined, and volk hard-errors if
  it sees `vulkan.h` without `VK_NO_PROTOTYPES`.
- **sdl3 needs its `vulkan` feature explicitly.** It is not a default feature of
  the vcpkg port, so a bare `"sdl3"` dependency builds with `SDL_VULKAN=OFF` and
  `SDL_CreateWindow(SDL_WINDOW_VULKAN)` fails at runtime with "Vulkan support is
  either not configured in SDL...". The manifest requests `sdl3[vulkan]`.
  Changing this rebuilds SDL from source, once per build directory.
- **GLM is configured strictly**: `GLM_FORCE_EXPLICIT_CTOR` (no implicit
  conversions between vector types), `GLM_FORCE_SIZE_T_LENGTH` (`.length()`
  returns `size_t`), `GLM_FORCE_AVX2`.
- **Alignment is opt-in per type, by design.** The default gentypes stay packed
  — `vec3` is `size 12, align 4`, so `struct Vertex { vec3 pos; vec3 nrm; vec2
  uv; }` is 32 bytes at offsets 0/12/24 and uploads to a vertex buffer directly.
  `glm::aligned_vec3` / `aligned_vec4` (`#include <glm/gtc/type_aligned.hpp>`,
  16/16) are opted into per type where SIMD or std140/std430 layout is wanted;
  `packed_*` typedefs exist in the same header for the other direction.

  **Do not add `GLM_FORCE_DEFAULT_ALIGNED_GENTYPES`.** It does not remove a
  capability — `packed_vec3` stays 12/4 under it, so both layouts remain
  expressible either way. What it changes is which one `vec3` means by default,
  and the failure modes are not symmetric. With it, `struct Vertex { vec3 pos;
  vec3 nrm; vec2 uv; }` is silently 48 bytes at offsets 0/16/32, so forgetting
  `packed_vec3` at a vertex-data site corrupts what is uploaded to the GPU.
  Without it, forgetting `aligned_vec4` in math code only costs SIMD. The
  default is deliberately the one whose mistake is cheap. Tried and reverted.
- **`GLM_FORCE_AVX2` alone does almost nothing.** GLM's SIMD paths are gated on
  the *qualifier* being aligned, not on the arch — the define only selects which
  instruction set those paths use once reached. Plain `vec4` is `align 4` and
  never reaches them. Only the `aligned_*` types get SIMD: `a + b` on
  `aligned_vec3` is one `vaddps` versus seven component-wise instructions on
  plain `vec3`.
- **Geometric functions are hand-specialized only for `L == 4`.** `dot` and
  `normalize` on any 3-component vector pay extra `vblendps` masking and lose
  the `vrsqrtps` path, so prefer `aligned_vec4` in hot math even when the data
  is logically 3D.
- **`GLM_ENABLE_EXPERIMENTAL` is on**, which unlocks `gtx/`. Those extensions
  carry no API stability promise between GLM releases, so a version bump can
  break call sites. `dquat` (dual quaternion) is the one currently used.

## GLM vs Vulkan conventions

Measured against GLM 1.0.3 under this project's flags, not assumed:

| Convention | Status |
| --- | --- |
| Depth range | Fixed by `GLM_FORCE_DEPTH_ZERO_TO_ONE`: near → `0.0`, far → `1.0` |
| Matrix storage | Column-major already, matches the GLSL/SPIR-V default |
| Handedness | Right-handed (GLM default); `GLM_FORCE_LEFT_HANDED` not set |
| **Y axis** | **Wrong, and no flag fixes it** |

`GLM_FORCE_DEPTH_ZERO_TO_ONE` is the only Vulkan-correctness flag GLM offers.
Without it, `glm::perspective` emits OpenGL's `-1..1` depth, which wastes half
the depth buffer and breaks depth testing in ways that look like Z-fighting.

**The Y axis has no compile flag.** GLM's projections put +Y up; Vulkan's
framebuffer origin is top-left with +Y down. A point above the eye projects to
`ndc.y = +0.35` when Vulkan wants `-0.35`, so geometry renders vertically
mirrored. Two accepted fixes, and the choice is not yet made here:

- `proj[1][1] *= -1.0f` after building the projection. Simple, but every
  projection site has to remember it.
- A negative-height `VkViewport` (core since Vulkan 1.1). Keeps the projection
  matrix conventional and moves the fix to one place, at the cost of a
  viewport that reads oddly.

**Quaternion component order is asymmetric.** `qua`'s constructor takes
`(w, x, y, z)` but the default memory layout is `x, y, z, w` — `f32quat{1,2,3,4}`
gives `w=1` and stores `2 3 4 1`. Constructing is w-first, uploading is w-last.
`GLM_FORCE_QUAT_DATA_WXYZ` would make them agree, at the cost of a memory order
that no longer matches a shader-side `vec4`.
- **No GLM type is `constexpr`-constructible.** `GLM_HAS_CONSTEXPR` is forced to
  0 whenever the SIMD arch bit is set (`detail/setup.hpp:297`), which
  `GLM_FORCE_AVX2` does. `GLM_CONFIG_CONSTEXP` is `GLM_DISABLE` here. Compile-
  time GLM constants are not available; use plain arrays or scalars instead.
- **AVX2/FMA is a baseline assumption** (`-mavx2 -mfma` / `/arch:AVX2`) — the
  binary will not run on pre-Haswell CPUs.
- **C++23**, no compiler extensions, hidden visibility, PIC.

## Warnings

GCC/Clang builds enable `-Wconversion`, `-Wsign-conversion`, `-Wshadow`, and
`-Wold-style-cast` on top of `-Wall -Wextra -Wpedantic`. Narrowing and signed/
unsigned mixing must be spelled out with explicit casts, and C-style casts are
rejected. Warnings are not errors (`/WX` is commented out), but the build should
stay clean.
