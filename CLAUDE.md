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

`encke` is a Vulkan renderer. There is no physical device, swapchain or frame
loop yet — the surface is as far as it goes.

| File | Holds |
| --- | --- |
| `main.cpp` | Entry point. Constructs `App`, nothing else. |
| `app.hpp/.cpp` | Owns the window and the Vulkan context; runs the event loop. |
| `window.hpp/.cpp` | SDL3 init, window, and event pump. Returns `FrameEvents`. |
| `vulkan_context.hpp/.cpp` | volk, instance, validation, surface. |
| `log.hpp/.cpp` | Unbuffered stderr diagnostics with millisecond stamps. |
| `pch.hpp`, `types.hpp` | Precompiled header and the global type prelude. |

`App`'s members are declared window-first so they destruct Vulkan-first, which
is the order the surface requires. Every `shutdown()` checks its handle, so a
partially constructed `App` tears down correctly.

## Known issue: 10-second exit

`SDL_Quit()` takes **~10 seconds** on this machine. Measured, not guessed:
`SDL_QuitSubSystem(SDL_INIT_VIDEO)` is 10014 ms while events and audio quit in
under 2 ms, and a bare `SDL_Init(SDL_INIT_VIDEO)` + `SDL_Quit()` with no window
and no Vulkan reproduces it. All Vulkan teardown finishes in ~200 ms.

The cause is SDL's Windows video backend: `SDL_windowsgameinput.cpp:240` calls
`SDL_InitGameInput` unconditionally, with no hint guarding it, and the matching
`SDL_QuitGameInput` blocks in `IGameInput::Release()` / `FreeLibrary` on
`gameinput.dll` (0.2309.22621.4249 here). `SDL_HINT_WINDOWS_GAMEINPUT` does not
help — that hint only gates the *joystick* driver. Disabling the Steam implicit
layers and every `SDL_JOYSTICK_*` hint changes nothing either.

It is SDL's, not ours, and it costs nothing but wall-clock at exit. If it
becomes intolerable during development, the blunt fix is to skip `SDL_Quit()`
and `std::_Exit` after Vulkan teardown — at the cost of losing whatever a clean
SDL shutdown would have reported.

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
.claude\cmake.ps1                                  # gcc-debug, incremental
.claude\cmake.ps1 -Preset release -Run             # build, then execute
.claude\cmake.ps1 -Preset clang-sanitize -Clean    # full rebuild under ASan/UBSan
.claude\cmake.ps1 -Target encke                    # a specific target
.claude\cmake.ps1 -Configure                       # force a reconfigure (slow — see below)
```

Presets: `gcc-debug` (Debug, `-Og`), `release` (RelWithDebInfo), `clang-sanitize`
(ASan + UBSan). The script picks the toolchain directory per preset, auto-
configures when `build/<preset>/CMakeCache.txt` is missing, and exits nonzero on
any configure or build failure rather than falling through to `-Run`.

`.claude/` is gitignored — the script hardcodes machine-specific MSYS2 paths, so
it will not exist in a fresh clone. Recreate it or fall back to raw `cmake` with
`F:\msys2\ucrt64\bin` prepended to PATH manually.

Run directly: `build/<preset>/encke.exe`

`ctest` is enabled (`enable_testing()`) but no tests are registered yet. Once
tests exist: `ctest --test-dir build/gcc-debug -R <name>` for a single test.

### Do not force a reconfigure casually

`-Configure` (and deleting a build directory) re-runs `vcpkg install`, which
takes **~5.5 minutes** for this project's five dependencies even with a warm
binary cache. Incremental builds need no reconfigure — CMake re-runs itself
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
