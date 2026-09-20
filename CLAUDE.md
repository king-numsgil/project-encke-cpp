# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

`encke` is a Vulkan renderer at the scaffolding stage. `main.cpp` is still a
hello-world; all of the graphics dependencies are wired up in CMake but nothing
uses them yet. There is no architecture to preserve — when adding real code,
establish the structure rather than looking for an existing one.

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

- **volk + Vulkan::Vulkan are both linked.** volk is a meta-loader; the usual
  setup defines `VK_NO_PROTOTYPES` and calls `volkInitialize`/`volkLoadDevice`.
  That define is not set yet, so Vulkan entry points currently resolve against
  the static loader. Decide this deliberately when real Vulkan code lands.
- **GLM is configured strictly**: `GLM_FORCE_EXPLICIT_CTOR` (no implicit
  conversions between vector types), `GLM_FORCE_SIZE_T_LENGTH` (`.length()`
  returns `size_t`), `GLM_FORCE_AVX2`.
- **AVX2/FMA is a baseline assumption** (`-mavx2 -mfma` / `/arch:AVX2`) — the
  binary will not run on pre-Haswell CPUs.
- **C++23**, no compiler extensions, hidden visibility, PIC.

## Warnings

GCC/Clang builds enable `-Wconversion`, `-Wsign-conversion`, `-Wshadow`, and
`-Wold-style-cast` on top of `-Wall -Wextra -Wpedantic`. Narrowing and signed/
unsigned mixing must be spelled out with explicit casts, and C-style casts are
rejected. Warnings are not errors (`/WX` is commented out), but the build should
stay clean.
