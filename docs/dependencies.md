# Dependencies and the allocator

Most come from vcpkg manifest mode (`vcpkg.json`, pinned by a baseline in
`vcpkg-configuration.json`): volk, vulkan, vulkan-memory-allocator, sdl3,
sdl3-image, glm, fastgltf, cpuinfo, bshoshany-thread-pool, entt, glaze,
catch2, and fastnoise2 and joltphysics from overlay ports in `ports/`. Three
come from CPM instead: mimalloc, Dear ImGui and ImPlot.

## mimalloc

It backs `operator new`/`delete` (`mimalloc-new-delete.h`, included only in
`src/core/memory.cpp`), SDL3 (`SDL_SetMemoryFunctions`), ImGui and ImPlot,
Vulkan host allocations (`memory::vulkan_callbacks()`), and Jolt.
`ENCKE_USE_MIMALLOC` gates all of it; off, `vulkan_callbacks()` returns
`nullptr`, which Vulkan reads as its own allocator.

- **With it on, the C++ runtime links statically** (`-static-libstdc++
  -static-libgcc`). A replacement `operator new` does not reach into a DLL
  on Windows, so memory `libstdc++-6.dll` allocated reached mimalloc's `free`
  and corrupted its pages; `std::filesystem::path` did it first. A build with
  `-DMI_DEBUG_FULL=ON` named it at once; that is the tool for heap
  corruption. `objdump -p encke.exe` must show no `libstdc++-6.dll`.
- **Every `vkCreate*` and its `vkDestroy*` pass `vulkan_callbacks()`.**
  Vulkan requires matching callbacks; validation reports each mismatch.
- It is built by CPM because `MI_MINGW_UCRT64` must be defined or it aborts at
  startup (`mi_out_default == NULL`): upstream sets it only when
  `$ENV{MSYSTEM}` is `UCRT64`, which vcpkg scrubs. Do not remove the
  `if(MINGW)` block that applies it.
- Off under clang-sanitize, on purpose: mimalloc takes memory from its own
  arenas, so ASan cannot see a use-after-free in it (tested: it printed
  garbage and exited 0). ASan's own allocator is the stronger detector.
- ASan's container-overflow check is off (`src/core/sanitizers.cpp`): the
  vcpkg ports are uninstrumented, and FastNoise2's graph decoder grew a
  vector against our instrumented template instances, a false positive.
- What it is worth is unmeasured, and that is fine: there is almost no
  per-frame allocation yet. To measure, interleave configurations within one
  window on a workload that allocates; back-to-back runs here read drift.

## Jolt

`ports/joltphysics` builds v5.6.0 unpatched, double precision (f64 `RVec3`
positions) and cross-platform deterministic. `Jolt::Jolt` carries its `JPH_*`
defines to encke, and they must match the library's or class layouts
disagree (`JPH::VerifyJoltVersionID()` checks). Jolt ties some defines to the
build configuration and encke's release is RelWithDebInfo, which its lists
leave out, so the port turns those options off. **Every source including
Jolt builds with `-ffp-contract=off`**: its maths is inline and clang would
fuse multiply-adds. Its target adds `-mbmi -mlzcnt -mf16c`.

## Others

- **glaze** reads and writes aggregates by reflection. A reflected type must
  not be in an anonymous namespace (it needs an `extern` variable of the
  type). Hand-written files read with `.comments = true`; unknown keys are
  errors. `tests/core/json_test` pins both.
- **BS::thread_pool** is here for its native extensions only: thread priority
  and names, which standard C++ lacks and MinGW's `native_handle()` cannot
  give. Only `platform/thread.cpp` includes it (it pulls in `<windows.h>`),
  with `BS_THREAD_POOL_NATIVE_EXTENSIONS`. On Linux, lower workers' priority,
  never raise the main thread's. The job system is `platform/worker_pool`.
- **cpuinfo** sizes pools by physical core: hyperthreads share vector units.
  Only `platform/cpu.cpp` includes it; debug builds print its topology trace.
- **sdl3** needs its `vulkan` feature and **sdl3-image** its `jpeg` and `png`
  features requested explicitly; bare, they build without them.
- **Dear ImGui** is the docking branch (`v1.92.9b-docking`), with ImPlot
  `v1.0`, built into `encke_imgui` by CPM; the vcpkg port links
  `Vulkan::Vulkan`, which collides with volk. Docking and multi-viewport are
  not enabled. `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` cannot be set (ImPlot calls
  the old `AddPolyline`). Their headers are `SYSTEM` and their sources build
  with `-w`. `imgui.ini` lands beside the executable. Avoid
  `ImGuiTableFlags_SizingStretchProp` on a table with no measured content:
  UBSan catches the NaN it produces.
- **volk** owns every Vulkan entry point: `VK_NO_PROTOTYPES` is global and
  the target links `Vulkan::Headers`, never `Vulkan::Vulkan` (duplicate
  symbols). `volkInitialize()`, `vkCreateInstance`, `volkLoadInstance()`,
  and `volkLoadDevice()` once a device exists. The PCH includes `volk.h`
  before anything reaching `vulkan.h`, or SDL declares its own handles.
- **VMA** gets `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`, since volk
  leaves no linked Vulkan symbols; `vulkan/allocator.cpp` is the single
  `VMA_IMPLEMENTATION`.
