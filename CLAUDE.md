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

`encke` is a Vulkan renderer with a working clustered deferred pipeline: a
G-buffer pass, compute light clustering, compute lighting and a tonemap pass,
drawing a procedural ship corridor lit by a few dozen point lights. No textures,
shadows or asset loading yet — all geometry is generated in code from one cube
mesh, and every material is flat. A Dear ImGui overlay shows frame and per-pass
GPU timings, graphed with ImPlot.

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
    context.{hpp,cpp}    volk, instance, validation, surface
    device.{hpp,cpp}     device selection, queues, submit_immediate
    allocator.{hpp,cpp}  VMA lifetime; the only VMA_IMPLEMENTATION
    bindless.{hpp,cpp}   the one global descriptor set
    buffer.{hpp,cpp}     device-local (staged) and host-mapped buffers
    image.{hpp,cpp}      any screen-sized target: G-buffer, HDR, depth
    swapchain.{hpp,cpp}  swapchain, images, views (scene and UI), recreation
    timestamps.{hpp,cpp} GPU timestamp queries, one range per frame in flight
  ui/
    imgui_layer.{hpp,cpp}  ImGui + ImPlot contexts, backends; the renderer's Overlay
    imgui_vulkan.{hpp,cpp} forked ImGui Vulkan backend: VMA, bindless, Slang
    stats_window.{hpp,cpp} frame timing history and the window graphing it
  render/
    camera.{hpp,cpp}     f64 camera, infinite reversed-Z projection
    scene.{hpp,cpp}      f64 world: test corridor, objects, lights
    mesh.{hpp,cpp}       Vertex, Mesh, procedural cube
    gpu_types.hpp        structs shared with the shaders
    pipeline.{hpp,cpp}   graphics and compute pipeline construction
    renderer.{hpp,cpp}   the four passes, barriers, per-frame upload
shaders/
  gbuffer.slang          geometry -> G-buffer + emissive into HDR
  cluster_build.slang    compute: lights -> froxels
  lighting.slang         compute: shade from the cluster's lights
  tonemap.slang          HDR -> swapchain
  imgui.slang            ImGui draw lists; decodes sRGB vertex colour, optionally re-encodes
  lib/
    bindless.slang       the descriptor arrays; mirrors vulkan/bindless.hpp
    gpu_types.slang      mirrors render/gpu_types.hpp
    cluster.slang        cluster addressing, shared by build and lighting
    pbr.slang            GGX / Smith / Schlick
    normal.slang         octahedral encode and decode
    screen.slang         fragment/NDC/UV conversions and the Y conventions
    colour.slang         sRGB <-> linear, luminance
```

**Includes are always full paths from `src/`** — `#include "vulkan/device.hpp"`,
never `"device.hpp"`, even between files in the same directory. `src` is the
only include root, so a bare name would be ambiguous about where it lives.
Headers sit beside their `.cpp`; there is no separate `include/` tree, because
nothing here is consumed as a library.

`App`'s members are declared window-first so they destruct in reverse: UI,
renderer, swapchain, device, instance, window. `~App` calls `wait_idle()` before any of
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
  the pipeline. The attachment formats do, since dynamic rendering bakes them
  in.
- **Depth is REVERSED-Z on `VK_FORMAT_D32_SFLOAT`.** Near maps to 1.0, far to
  0.0. Four things must agree and all four are load-bearing:

  | | |
  | --- | --- |
  | projection | `glm::perspective` with **near and far swapped** |
  | clear value | `0.0` (`DepthTarget::kClearDepth`) |
  | compare op | `VK_COMPARE_OP_GREATER_OR_EQUAL` |
  | format | `D32_SFLOAT` — float depth is the point |

  Float exponent bits bunch near zero, and a conventional 0..1 mapping spends
  that precision at the far plane where it does nothing. Reversing moves it to
  the near plane. Break any one of the four and geometry vanishes or z-fights;
  a normal projection with a `GREATER` compare draws the far surfaces instead
  of the near ones, which back-face culling can disguise on convex meshes.
- **All device memory goes through VMA.** Nothing calls `vkAllocateMemory`.
  `vulkan/allocator.cpp` is the single `VMA_IMPLEMENTATION`, and VMA is given
  `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` because volk means there are no
  linked Vulkan symbols to find.
- **Buffers are device-local, filled once through a staging copy** driven by
  `VulkanDevice::submit_immediate`, which blocks on `vkQueueWaitIdle`. That is
  fine for startup and wrong for anything per-frame.
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
- **MAILBOX present mode where offered**, FIFO (vsync) otherwise. FIFO is the
  only mode guaranteed to exist.
- **The UI is colour-correct on either view; the view decides the blend
  space.** `shaders/imgui.slang` decodes ImGui's sRGB vertex colours to linear,
  treats every texture as linear (ImGui's own are created `_SRGB` so the sampler
  decodes them), and multiplies. Written to an `_SRGB` target that goes out
  as-is; written to a UNORM target, `push.ui_encode_srgb` has the shader encode
  it. Only blending differs: sRGB-space through UNORM, linear-space through
  sRGB.
- **The UI prefers a UNORM view of the sRGB swapchain,** because ImGui's styles
  and anti-aliasing were tuned for sRGB-space blending. The swapchain is created
  with `VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR` and a format list, and
  `ui_view()` / `ui_format()` hand out the UNORM twin. The extension
  (`VK_KHR_swapchain_mutable_format`) is optional: without it the UI uses the
  sRGB view and blends in linear space, which lightens translucent elements
  slightly. `ENCKE_UI_SRGB` forces that path so it stays tested on hardware that
  has the extension.
- **The clear colour is linear.** The swapchain is `B8G8R8A8_SRGB`, so the
  hardware encodes on write. Linear `(0.03, 0.12, 0.18)` lands as sRGB
  `(48, 97, 118)` on screen — verified by screen capture, not assumed.
- The swapchain is only rebuilt when the window size actually differs, since a
  resize event also fires once at startup for the initial size.

The frame loop reports throughput once a second. Treat it as a relative signal
within a single session, never as a figure worth recording.

## Clustered deferred — built, first draft

Four passes per frame, orchestrated in `render/renderer.cpp`, then the UI:

| Pass | Kind | Does |
| --- | --- | --- |
| G-buffer | raster | albedo/ao, octahedral normal, roughness/metallic, motion, depth; emissive seeds the HDR target |
| clusters | compute | one thread per froxel, tests every light against its view-space AABB |
| lighting | compute | rebuilds view position from depth, shades against that froxel's lights, adds onto HDR |
| tonemap | raster | full-screen triangle, exposure + ACES, into the sRGB swapchain |
| overlay | raster | caller-recorded UI, its own rendering scope on the UI view, `LOAD` |

The overlay is an interface (`Renderer::Overlay`) so the renderer never includes
ImGui. It has two phases: `prepare(command, slot)` outside any rendering scope,
for texture uploads and vertex data, then `record(command)` inside. The scope
is separate from tonemap's because it may use a different view of the same
image, and a colour-attachment barrier precedes it because its `LOAD` reads what
tonemap stored.

### The ImGui renderer backend is a fork

`ui/imgui_vulkan` is `backends/imgui_impl_vulkan.cpp` from ImGui
`v1.92.9b-docking`, forked and version-locked. It carries ImGui's MIT notice in
both files, which the licence requires, and it stays there. Upgrading ImGui
means reading upstream's changes to that file and porting what applies, not
replacing ours.

What changed, and what depends on it:

- **Memory is VMA** through `Buffer` and `Image`. No `vkAllocateMemory`.
- **Textures are bindless.** `ImTextureID` is a sampled-image handle plus one,
  because ImGui reserves 0 and slot 0 is live; build it with
  `ImGuiVulkan::texture_id(handle)`. Any renderer target registered in the set
  can be drawn with `ImGui::Image`. Its layout must be `READ_ONLY_OPTIMAL` when
  the overlay runs, *and* the barrier that put it there must include
  `FRAGMENT_SHADER` in its destination. The G-buffer barriers currently name
  compute only, so a debug window showing them needs that widened, and the next
  frame's entry barriers need `FRAGMENT_SHADER` in their source for the
  write-after-read.
- **Uploads are recorded in `prepare()`**, not submitted separately with
  `vkQueueWaitIdle`. Staging buffers and destroyed textures go on the slot's
  retire list and are freed when that slot next comes round. That is safe
  whatever ImGui's `UnusedFrames` says, because every frame submitted before it
  has retired by then.
- **It shares the pipeline layout.** The UI's push fields live in `gpu::Push`
  (`ui_transform`, `ui_texture`, `ui_sampler`, `ui_encode_srgb`), following the
  one-struct-for-every-pass convention.
- **Multi-viewport is gone,** along with the `ImGui_ImplVulkanH_*` window
  helpers. Secondary OS windows would need a swapchain each, ported onto
  `VulkanSwapchain`. Docking alone needs nothing from the renderer.

`BindlessSet::release_sampled_image` exists for it: ImGui can destroy and
recreate textures when the font atlas rebuilds, and sampled-image slots now go
on a free list for reuse.

### GPU timings

`vulkan/timestamps` stamps the frame as a chain: `begin()` at the start, then
`mark("label")` after each pass closes the section since the previous stamp.
Every stamp is written at `ALL_COMMANDS`, so the sections tile the frame with no
overlap — which also means they hide any overlap the GPU would have found. The
sum is the frame's GPU time. Results are read after the slot's fence, so they
trail the recorded frame by `kFramesInFlight`.

The G-buffer section includes any wait on the acquire semaphore, because that
semaphore gates `COLOR_ATTACHMENT_OUTPUT` and the G-buffer is the first pass to
reach it. A G-buffer figure that swings with present mode is that wait, not
rasterisation.

"CPU busy" in the stats window is frame time minus what `draw()` spent blocked
in the fence wait, acquire and present.

### The invariants that make it correct

- **`shaders/lib/cluster.slang` is the single source of cluster addressing.**
  The build pass and the lighting pass must agree on which froxel a point falls
  in. If they disagree nothing errors — lighting just goes missing at froxel
  boundaries. Never inline that math into a pass.
- **`DebugView::BruteForce` (key 2) is the correctness test.** It shades every
  light with no clusters at all. Clustered and brute force must be
  byte-identical; they were verified so over a full frame. Any divergence is a
  cluster assignment bug, and this is far easier than eyeballing it.
- **`DebugView::ClusterHeat` (key 3) proves the clusters are doing work.**
  Identical output would also happen if every froxel held every light. The heat
  map must show variation — it currently runs 6 to 16 of 29 lights per froxel.
- **`ENCKE_FIXED_TIME` pins animation** so two captures can be compared. The
  camera animates on wall-clock, so without it a comparison across runs is
  comparing camera positions, not renderer changes.
- **`ENCKE_NO_UI` (or F1) hides the overlay**, and a byte comparison needs it:
  the stats window's numbers change every frame, so two captures with it
  showing never match.

### Parameters, and why these

16x9x24 froxels, logarithmic in depth between 10 cm and 400 m, 64 lights per
froxel. The 16:9 tiling matches the screen; the log depth distribution puts
most slices inside the first tens of metres, which is where a corridor or a
bridge has lights. Beyond 400 m everything shares the last slice — those are
exterior lights and want different treatment anyway.

### Known gaps, deliberately

- The cluster pass is brute force, every light against every froxel. Correct,
  and fine for dozens of lights; it will not survive thousands.
- Per-froxel light lists are fixed size and silently drop past 64.
- The G-buffer and HDR targets are single-copy, so a frame's entry barriers
  wait on the previous frame's reads. Correct, and it serialises more than it
  needs to.
- The acquire semaphore is waited at `COLOR_ATTACHMENT_OUTPUT`, which stalls
  the G-buffer pass on the swapchain image even though only the tonemap needs
  it. Splitting the submission would fix it.
- No shadows, so the sun is switched off: a directional light with no occlusion
  lights the inside of a sealed corridor. The code path is wired and ready.
- Exposure is a hand-tuned constant, not an exposure model.
- Host-visible buffers are never flushed. `Buffer::init_mapped` lets VMA pick
  the memory type, and nothing calls `vmaFlushAllocation`, which is only
  correct while that type is `HOST_COHERENT`. It is on this NVIDIA driver; it
  is not guaranteed. The UI's vertex and staging buffers inherit this.

## Renderer architecture — the decision behind it

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
- **Octahedral normals in RG16_UNORM.** Not RGBA32F.
- **Motion vectors exist from the first version**, RG16F, storing
  `uv_previous - uv_current` so reprojection is `uv + motion`. Both NDC
  positions go through `ndc_to_uv`, which carries the Y flip.

  They are per-FRAME displacement, so their magnitude scales with frame time.
  Uncapped at a couple of thousand frames a second they are around 5e-5 UV,
  which looks like nothing in the debug view and sits near fp16's smallest
  normal. That is expected, not a bug — at a realistic frame rate they are
  twenty times larger. Verified nonzero and spatially varying by cranking the
  debug gain.

## World space is f64; the GPU only sees view space

A space sim spans instruments centimetres from the eye and bodies millions of
metres away. An f32 world position holds about 6 cm of precision at 1,000 km,
which is enough to make a ship interior visibly jitter.

**Every world -> view transform happens in f64 on the CPU.** `view * model` is
composed in f64, which cancels the large translations against each other, and
only the small view-space result is narrowed to f32. Lights are transformed to
view space on the CPU for the same reason, which is why they have to be
re-uploaded every frame rather than living in a static buffer.

A shader that receives a world-space position is a bug.

The test corridor is built at (1e6, 2.5e5, -7e5) metres on purpose. Set
`kWorldOrigin` in `render/scene.cpp` to zero and the render must not change.
Measured: a 1 mm offset 3 m ahead of a camera at 1,000 km survives exactly
camera-relative, and collapses to 0.000000 if world positions are narrowed to
f32 first.

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
- **Matrices are column-major on both sides.** slangc gets
  `-matrix-layout-column-major` because Slang otherwise follows HLSL's
  row-major convention and would silently transpose every matrix pushed from
  GLM. Nothing warns about this; it presents as geometry in the wrong place.
- **`SV_VertexID` requires `shaderDrawParameters`.** Slang lowers it to
  `gl_VertexIndex` and emits the SPIR-V `DrawParameters` capability. Device
  creation enables the feature and selection requires it; without it
  `vkCreateShaderModule` is a spec violation that drivers accept silently and
  validation rejects.
- **Shader colour output is LINEAR.** The swapchain is `_SRGB` so the hardware
  encodes on write, and values written look considerably lighter than the
  numbers suggest. Author in sRGB and call `srgb_to_linear` rather than
  hand-computing linear constants, which leaves the intent unreadable.

## Bindless

One global descriptor set, bound once per bind point per command buffer, shared
by every pipeline. Shaders index it with integer handles carried in push
constants. `vulkan/bindless.hpp` and `shaders/lib/bindless.slang` must agree on
binding numbers; nothing checks that but validation at draw time.

| Binding | Holds |
| --- | --- |
| 0 | sampled images |
| 1 | storage images (always accessed in `GENERAL`) |
| 2 | **read-only** storage buffers |
| 3 | samplers |
| 4 | **writable** storage buffers, compute only |

**Bindings 2 and 4 are split for a reason.** A writable storage buffer in the
vertex or fragment stage requires `vertexPipelineStoresAndAtomics` /
`fragmentStoresAndAtomics`. Those stages only ever read, so they get the
read-only view and the features stay off. Each buffer is registered in exactly
one of the two.

Descriptors bake in an image layout, so a sampled slot is always
`READ_ONLY_OPTIMAL` and a storage slot always `GENERAL`, and the frame's
barriers must land exactly there. The HDR target is registered twice, once per
layout.

Handles are stable across a resize: `update_*` rewrites the slot in place. That
is only legal because the caller waits for idle first — `UPDATE_UNUSED_WHILE_PENDING`
permits rewriting a slot no pending command buffer uses, not one in flight.

This is also why the Intel iGPU is rejected at selection: format-less bindless
storage images need `shaderStorageImageReadWithoutFormat`, which it lacks.

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
it), SDL3 through `SDL_SetMemoryFunctions`, Dear ImGui and ImPlot through
`ImGui::SetAllocatorFunctions`, and Vulkan host allocations through
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
vendored at `cmake/CPM.cmake` and sources cached in `.cpm/` (gitignored). Dear
ImGui and ImPlot are the only other CPM dependencies, for a reason of the same
kind (see *Dependencies*); everything else stays in the vcpkg manifest.

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

Most deps come from vcpkg manifest mode (`vcpkg.json`, pinned via a baseline in
`vcpkg-configuration.json`): `volk`, `vulkan`, `vulkan-memory-allocator`,
`sdl3`, `glm`. Three come from CPM instead: mimalloc (see *Allocator*), and
Dear ImGui and ImPlot.

- **Dear ImGui is the docking branch** (`v1.92.9b-docking`), built from bare
  sources into the `encke_imgui` static library alongside ImPlot `v1.0`.
  Docking and multi-viewport are *not* enabled; the branch is taken so turning
  them on later is a flag, not a dependency change. The vcpkg port was the
  obvious route and is unusable: its `vulkan-binding` feature links
  `Vulkan::Vulkan`, which collides with volk exactly as described below. Only
  the SDL3 platform backend is built from upstream; the Vulkan renderer backend
  is our fork (see *The ImGui renderer backend is a fork*). ImPlot follows ImGui
  into CPM because it must compile against the same ImGui.
- **`IMGUI_DISABLE_OBSOLETE_FUNCTIONS` cannot be set.** ImPlot v1.0 still calls
  the pre-1.92 `AddPolyline(flags, thickness)` order, which that define deletes.
- Their headers are included `SYSTEM`, so this project's warning set does not
  fire inside them, and their sources build with `-w`.
- **`imgui.ini` is written beside the executable** (`SDL_GetBasePath()`), inside
  the gitignored build tree, not in the working directory.
- **ImGui tables: avoid `ImGuiTableFlags_SizingStretchProp`** on a table that
  appears with no measured content. Its first-frame weights come out 0/0, and
  UBSan catches the NaN in ImGui's window content-size maths a frame later.

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
  `GLM_FORCE_AVX2` does. `GLM_CONFIG_CONSTEXP` is `GLM_DISABLE` here. A
  `constexpr f32vec3` table is a compile error — use `const`. This one is easy
  to write by reflex and has already cost one build.
- **AVX2/FMA is a baseline assumption** (`-mavx2 -mfma` / `/arch:AVX2`) — the
  binary will not run on pre-Haswell CPUs.
- **C++23**, no compiler extensions, hidden visibility, PIC.

## Warnings

GCC/Clang builds enable `-Wconversion`, `-Wsign-conversion`, `-Wshadow`, and
`-Wold-style-cast` on top of `-Wall -Wextra -Wpedantic`. Narrowing and signed/
unsigned mixing must be spelled out with explicit casts, and C-style casts are
rejected. Warnings are not errors (`/WX` is commented out), but the build should
stay clean.
