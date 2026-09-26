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

`encke` is a Vulkan renderer with a working clustered deferred pipeline: shadow
maps, a G-buffer pass, compute light clustering, compute lighting and a tonemap
pass. The test scene is the north pole of an Earth-sized planet, strewn with
boxes and spheres, lit by a real-magnitude Sun low on the horizon (four shadow
cascades), shadowed spot lights on masts and a ring of point lamps, with the
Moon overhead at its real distance. The Earth is procedural terrain, meshed
with Surface Nets on a worker pool in an implicit octree that follows the
camera, geomorphed between LODs so they meet, under a physically based
atmosphere that holds from the ground to orbit. The other built-in
geometry is generated in code (cube, UV sphere); glTF models load through
fastgltf, and the Khronos DamagedHelmet sits on the table. About half the
objects carry CC0 PBR textures from ambientCG; the rest keep flat materials. A
Dear ImGui overlay shows frame and per-pass GPU timings, graphed with ImPlot.

```
src/
  main.cpp            entry point; constructs App and nothing else
  app.{hpp,cpp}       composition root: owns everything, runs the frame loop
  core/
    pch.hpp           precompiled header
    types.hpp         global type prelude
    log.{hpp,cpp}     unbuffered stderr diagnostics, millisecond stamps
    sanitizers.cpp    ASan's default options for this binary, under ASan only
  platform/
    window.{hpp,cpp}  SDL3 init, window, event pump -> FrameEvents
    cpu.{hpp,cpp}     physical and logical core counts, via pytorch/cpuinfo
    thread.{hpp,cpp}  calling thread's OS priority and name, via BS::thread_pool
    worker_pool.{hpp,cpp} jthreads at background priority; jobs return main-thread completions
    thread_load.hpp   a worker's busy time, for the stats window's load dots
  vulkan/
    context.{hpp,cpp}    volk, instance, validation, surface
    device.{hpp,cpp}     device selection, queues, submit_immediate
    allocator.{hpp,cpp}  VMA lifetime; the only VMA_IMPLEMENTATION
    bindless.{hpp,cpp}   the one global descriptor set
    buffer.{hpp,cpp}     device-local (staged) and host-mapped buffers
    image.{hpp,cpp}      any screen-sized target: G-buffer, HDR, depth
    texture.{hpp,cpp}    immutable sampled image with a blitted mip chain
    staging.{hpp,cpp}    per-frame upload arena, one per frame in flight: the upload budget
    swapchain.{hpp,cpp}  swapchain, images, views (scene and UI), recreation
    timestamps.{hpp,cpp} GPU timestamp queries, one range per frame in flight
  ui/
    image_window.{hpp,cpp} a window showing one bindless image, aspect kept
    imgui_layer.{hpp,cpp}  ImGui + ImPlot contexts, backends; the renderer's Overlay
    imgui_vulkan.{hpp,cpp} forked ImGui Vulkan backend: VMA, bindless, Slang
    stats_window.{hpp,cpp} frame timing history and the window graphing it; worker load dots
  world/
    transform.{hpp,cpp}  Transform and WorldTransform components, propagation
    bodies.{hpp,cpp}     Star, Body and Atmosphere components; the starlight, environment and air at a point
  assets/
    handle.hpp           typed generational handles: mesh, texture, material, model
    asset_manager.{hpp,cpp} every asset's handle and CPU state; ready queues for the renderer
    worker.{hpp,cpp}     the one asset thread: work there, finish on the main thread
    model.hpp            a loaded model: node tree over mesh and material handles
  render/
    camera.{hpp,cpp}     f64 camera, infinite reversed-Z projection
    fly_camera.{hpp,cpp} right-mouse fly control: mouse look, WASD, speed on the wheel
    scene.{hpp,cpp}      the EnTT registry and active camera; builds the test planet
    components.hpp       Renderable and Light, the components the renderer reads
    extract.{hpp,cpp}    registry -> RenderList once a frame: the renderer's only view of it
    material.{hpp,cpp}   an ambientCG set's colour, normal and packed ORM, via SDL3_image
    gltf.{hpp,cpp}       glTF -> CPU meshes, nodes, materials; image decoding, via fastgltf
    pixels.{hpp,cpp}     SDL3_image decode to RGBA8, from a file or bytes; asset paths
    mesh.{hpp,cpp}       Vertex, TerrainVertex, procedural cube and sphere
    geometry_pool.{hpp,cpp} every mesh in one vertex and one index buffer, VMA virtual blocks
    config.hpp           every renderer capacity and tuning constant
    shadows.{hpp,cpp}    cascade fitting and spot selection, f64, CPU only
    gpu_types.hpp        structs shared with the shaders
    pipeline.{hpp,cpp}   graphics and compute pipeline construction
    renderer.{hpp,cpp}   the passes, barriers, per-frame upload
  terrain/
    fastnoise.hpp        FastNoise2 include and the pinned feature set; terrain sources only
    detail_noise.{hpp,cpp} lattice-split Perlin fBm octaves, f64 origin + f32 offsets
    macro_field.{hpp,cpp}  FastNoise2 graphs per channel on a body-fixed lattice, trilinear
    terrain_field.{hpp,cpp} BodyTerrain and TerrainSampler: chunk and point queries
    surface_nets.{hpp,cpp}  a chunk's samples -> vertices and quads, under the ownership rule
    planet.{hpp,cpp}     PlanetTerrain, culling, GroundProbe, TerrainOctree: the chunks the camera wants
    benchmark.{hpp,cpp}  `encke --headless`: octaves per LOD; samples/s per layer, apron, gradients, LOD 0 and 4;
                         `encke --sweep [n]`: every LOD, n random-seed chunks, against FastNoise2's own cost
shaders/
  shadow_depth.slang     depth only: one shadow map, cascade or spot
  gbuffer.slang          geometry -> G-buffer + emissive into HDR
  cluster_build.slang    compute: lights -> froxels
  lighting.slang         compute: shade from the cluster's lights, shadowed
  debug_views.slang      compute: cluster heat, normals, motion, cascades, into window images
  atmosphere.slang       compute: atmosphere LUTs, ambient, sky-view LUT; sky and aerial perspective
  taa.slang              compute: temporal antialiasing resolve into the history
  exposure.slang         compute: luminance histogram, then metered and adapted EV100
  tonemap.slang          HDR -> swapchain, at the adapted exposure, CAS-sharpened with TAA on
  imgui.slang            ImGui draw lists; decodes sRGB vertex colour, optionally re-encodes
  lib/
    bindless.slang       the descriptor arrays; mirrors vulkan/bindless.hpp
    gpu_types.slang      mirrors render/gpu_types.hpp
    cluster.slang        cluster addressing and depth -> view position, shared
    shadow.slang         cascade choice, PCF lookups, sun and spot visibility
    pbr.slang            GGX / Smith / Schlick, spot cone
    normal.slang         octahedral encode and decode
    morph.slang          terrain geomorph: target, distance, neighbour masks
    atmosphere.slang     the air: medium, phases, LUT parameterisations, the view-ray march
    cas.slang            AMD's Contrast Adaptive Sharpening, ported; MIT notice kept
    screen.slang         fragment/NDC/UV conversions and the Y conventions
    colour.slang         sRGB <-> linear, luminance
tests/                 Catch2, mirroring src/: camera projection and view, transform propagation,
                       which atmosphere a point sees, terrain noise against an f64 oracle
                       (terrain/perlin_reference), meshing and morph targets, octree balance,
                       FastNoise2 under mimalloc
ports/fastnoise2/      vcpkg overlay port: FastNoise2 v1.1.1 plus two patches
assets/
  textures/            one directory per ambientCG set; CREDITS.md says where each came from
  models/              glTF files; CREDITS.md holds their licences
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
- **Per-frame uploads go through `StagingArena`**, never `submit_immediate`.
  One arena per frame in flight, bump-allocated and rewound once that slot's
  fence has signalled, which also proves the GPU finished copying out of it.
  Its size, `config::kStagingBytesPerFrame`, is the per-frame upload budget:
  what does not fit waits for a later frame. The copies are recorded first in
  the frame's command buffer, under the "uploads" timestamp. Staging happens
  after the acquire, never before: a frame that returns OutOfDate reuses its
  slot, and would rewind the arena over copies it never recorded. 80 MiB per
  frame, sized for a 4K RGBA8 map; materials stage map by map and go live
  with their last one.
- **Every mesh lives in one `GeometryPool`**: one vertex buffer and one index
  buffer, carved up by VMA virtual blocks that count in elements, so a range's
  offset is directly a `vertexOffset` or `firstIndex`. A mesh id is dense and
  reused after `release`, which frees the ranges only when the releasing
  frame's slot comes round again. Meshes upload through the staging arena
  like textures and draw from the frame they are staged in; until then
  they are left out of the draw lists. Terrain chunks are released as the
  octree swaps them; see *The Earth as terrain* for the ordering it needs.
  Beside the vertex buffer is a storage buffer of `TerrainVertex`, indexed
  like it, which only terrain chunks fill: each vertex's morph target and its
  ground materials; see *Geomorph and seams* and *Ground materials*.
- **Each raster pass is one indirect draw.** `upload()` writes one
  `VkDrawIndexedIndirectCommand` per object into a per-frame buffer (the
  G-buffer's first, then each shadow view's casters), and `record()` binds
  the pool once and issues one `vkCmdDrawIndexedIndirect` per pass or map.
  The object index is the command's `firstInstance`, read as
  `SV_VulkanInstanceID`: plain `SV_InstanceID` is lowered to
  `InstanceIndex - BaseInstance` and would always read 0. The G-buffer
  passes it to the fragment stage flat. `multiDrawIndirect` and
  `drawIndirectFirstInstance` are required at device selection. The draw
  lists are still built on the CPU and still capped at `kMaxObjects`, which
  is 4096 so a planet's terrain chunks fit beside the scene.
- **The G-buffer's draws are frustum-culled and sorted front to back** on
  the CPU, by each object's bounding sphere in f64 (`sphere_in_view` in
  `render/shadows`), terrain spheres grown by a parent voxel for the morph.
  Before this every chunk around the camera, most of them behind it, was
  vertex-shaded and morphed, and hills were shaded before the hills in
  front of them.
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
- **A CPU frame limiter holds 60 fps on top of MAILBOX**, on by default and
  toggled in the stats window. `App::limit_frame_rate` sleeps at the top of
  the loop, before input is polled, until a deadline that advances by whole
  periods, so one oversleep is made up by the next frame. It sleeps with
  `SDL_DelayNS`, which uses a high-resolution waitable timer on Windows;
  MinGW's `sleep_for` lands on the default 15.6 ms tick. The sleep is
  reported to the stats as blocked time, so it does not show as CPU busy.
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

Frame and per-pass timings live in the stats window, not the log. Treat them as
a relative signal within a single session, never as figures worth recording.

## Clustered deferred — built, first draft

The passes per frame, orchestrated in `render/renderer.cpp`, then the UI:

| Pass | Kind | Does |
| --- | --- | --- |
| shadows | raster | depth only: the sun's cascades, then each chosen spot's map |
| G-buffer | raster | albedo/ao, octahedral normal, roughness/metallic, motion, depth; emissive seeds the HDR target |
| sky ambient | compute | with air: its LUTs when it changed, the environment around the camera, the sky-view LUT |
| clusters | compute | one thread per froxel, tests every light against its view-space AABB |
| lighting | compute | rebuilds view position from depth, shades against that froxel's lights, adds onto HDR |
| atmosphere | compute | with air: the sky where nothing was drawn, aerial perspective over what was |
| TAA | compute | resolves HDR into the history, which exposure and tonemap then read |
| exposure | compute | luminance histogram of HDR, then one group meters and adapts EV100 |
| debug views | compute | one visualisation image per open debug window; skipped when none is open |
| tonemap | raster | full-screen triangle, exposure + ACES, AgX or PBR Neutral, into the sRGB swapchain |
| overlay | raster | caller-recorded UI, its own rendering scope on the UI view, `LOAD` |

### Debug views

Keys 1 and 2 choose how the frame is shaded: clustered or brute force
(`DebugView`). Keys 3 to 6 toggle a UI window each — lights per cluster,
normals, motion vectors, shadow cascades (`DebugWindow`) — and opening one
while the UI is hidden brings the UI back. The cascade window tints each pixel
by the cascade it reads and darkens it where the sun is shadowed. `ENCKE_DEBUG_VIEW` uses the same numbering from
zero, so `ENCKE_DEBUG_VIEW=2` starts with the heat map open.

The windows do not show render targets directly. `shaders/debug_views.slang`
runs once per open window after lighting and writes a display-ready linear
RGBA16F image, which the window samples through the bindless set. Only open
windows are drawn: `App::draw_ui` tells the renderer which are open *after*
ImGui has run, because a window closed this frame must not be sampled from an
image the frame did not draw. The motion window's gain is a slider, since
per-frame motion scales with frame rate.

The images' barriers are the ones a sampled-in-UI image needs: `UNDEFINED ->
GENERAL` waiting on the previous frame's `FRAGMENT_SHADER` reads, then `GENERAL
-> READ_ONLY_OPTIMAL` into `FRAGMENT_SHADER` sampled reads. The "debug views"
timestamp is written every frame, open windows or not, so the stats history
does not reset on every toggle.

Digit keys are ignored only while ImGui has a text field active
(`WantTextInput`). `WantCaptureKeyboard` is the wrong test: with keyboard
navigation on, it is true whenever an ImGui window has focus, which the first
window gets on appearing, and it silently ate every digit key.

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
  can be drawn with `ImGui::Image` (`ui/image_window` wraps that). Its layout
  must be `READ_ONLY_OPTIMAL` when the overlay runs, *and* the barrier that put
  it there must include `FRAGMENT_SHADER` in its destination. The debug images
  do. The G-buffer barriers name compute only, so showing a raw G-buffer
  target needs them widened, and the next frame's entry barriers need
  `FRAGMENT_SHADER` in their source for the write-after-read.
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
  cluster assignment bug, and this is far easier than eyeballing it. It cannot
  catch anything both paths share, such as position reconstruction.
- **View distance from depth is `distance_from_depth` in `lib/cluster.slang`,
  and nowhere else.** It divides the *camera's* near plane, read from the
  projection, by the depth sample. Lighting once divided the cluster grid's
  near distance instead — 0.1 m against the camera's 0.05 m — which put every
  reconstructed position at twice its distance. Brute force matched because it
  shared the mistake; what exposed it was the sun's shadow, where receivers on
  the ground landed a camera height below it and the ground shadowed itself.
- **The lights-per-cluster window (key 3) proves the clusters are doing work.**
  Identical output would also happen if every froxel held every light. The heat
  map must show variation.
- **A byte comparison captures the client area only.** Grabbing the window
  rectangle picks up Windows 11's invisible resize border, and even the client
  rectangle shows the desktop through the rounded bottom corners, so a few
  corner pixels differ between runs whatever the renderer does. Last verified
  with the debug views split out: identical apart from a 4x4 corner.
- **`ENCKE_FIXED_TIME` pins animation** so two captures can be compared. The
  spinning showpiece animates on wall-clock, so without it a comparison across
  runs compares its rotation, not renderer changes. The camera starts at the
  same pose every run and only moves when flown, so a capture must not touch
  the mouse.
- **`ENCKE_NO_UI` (or F1) hides the overlay**, and a byte comparison needs it:
  the stats window's numbers change every frame, so two captures with it
  showing never match.
- **`ENCKE_CAPTURE=path.png` is the way to capture.** The renderer copies the
  finished swapchain image (UI included, so pair it with `ENCKE_NO_UI`) into
  host memory on frame `ENCKE_CAPTURE_FRAME` (default 10), the app writes it
  as a PNG and quits. Nothing touches the desktop, so other windows, the
  compositor and the user's own use of the machine cannot get into it; a
  desktop grab once captured a browser because Windows refused the
  foreground switch. Two runs with the same settings are byte-identical.
  Once the frame count is reached and streaming has settled, the app
  discards TAA's history and captures `config::kTaaJitterCount` frames later,
  one jitter cycle, so the history is the same on every run.
- **`ENCKE_NO_TAA`** (or the stats window's checkbox) turns temporal
  antialiasing off.
- **F2 freezes the terrain octree** (`TerrainOctree::set_frozen`): nothing
  is selected, meshed or swapped, so what is on screen stays, masks and
  all, while the camera flies. `ENCKE_CAPTURE_MOVE`, a pose in
  `ENCKE_CAMERA`'s format, does the same for a capture: settle at the start
  pose, freeze, move there, capture. `ENCKE_CAPTURE_FLY="vx vy vz"` instead
  flies at that velocity once settled, 60 fixed steps a second with the
  octree live, and captures after `ENCKE_CAPTURE_FRAME` frames of it; what
  has swapped by then depends on timing, so those runs differ.
- **F3 copies the camera's pose** to the clipboard and the log, as
  `ENCKE_CAMERA="..."`; with the terrain frozen, as `ENCKE_CAMERA` at the
  pose F2 froze it and `ENCKE_CAPTURE_MOVE` at the camera, which replays
  the view on a settled octree. The pose is metres from the scene's origin
  with a target 10 m ahead and the camera's up.
- **`ENCKE_CAMERA="px py pz tx ty tz"`** starts the camera at p looking at t,
  metres from the pole. The helmet: `"-2.55 1.33 -1.62 -3 1.17 -2"`.
- **`ENCKE_TONEMAP` (0 ACES, 1 AgX, 2 PBR Neutral, T cycles) and `ENCKE_EV100`**
  pick the curve and pin the exposure it is applied at. Metering still runs
  under a pinned EV, so the two separate cleanly. PBR Neutral is the
  default.

### Parameters, and why these

Every capacity and tuning constant lives in `render/config.hpp`, compile-time,
so it can be edited and rebuilt while testing. Shaders read the counts they
need from the Frame buffer, so none of them is duplicated in Slang.

16x9x24 froxels, logarithmic in depth between 10 cm and 400 m, 64 lights per
froxel. The 16:9 tiling matches the screen; the log depth distribution puts
most slices inside the first tens of metres, which is where a ship interior or
a landing site has lights. Beyond 400 m everything shares the last slice —
those are exterior lights and want different treatment anyway.

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

## Auto-exposure

`shaders/exposure.slang` runs after the atmosphere. `histogram_main` bins every
pixel's log2 luminance into `config::kExposureBins` bins; bin 0 takes
everything below the range, and every pixel where nothing was drawn, and is
ignored: exposure follows what is drawn and the sky is backdrop. Metering the
sky too, the dim but not empty sky seen from 80 km took half the frame and
burned the planet below to white; the atmosphere pinned at EV100 14 looked
right, which is how the fault was placed in metering. A frame with nothing
drawn counts nothing and holds its exposure. `adapt_main` is one group: it averages
log luminance between two percentiles, meters EV100 as `log2(L) + 3` (the
ISO 100, K = 12.5 convention), clamps it, moves the stored EV100 toward it
over wall-clock time, faster when the scene brightens, and zeroes the
histogram for the next frame. Tonemap's exposure is `1 / (1.2 * 2^EV100)`,
the same formula the CPU uses for a fixed EV. Every knob is in
`render/config.hpp`; `kAutoExposure = false` falls back to `Scene::ev100`.

- **The state is a 1x1 R32F image, registered twice like HDR**: storage for
  the adapt pass in `GENERAL`, sampled for tonemap in `READ_ONLY_OPTIMAL`.
  A buffer would need the read-only binding in the fragment stage and the
  writable one in compute, and a buffer lives in exactly one of those. It
  keeps its contents across frames, so after the first frame its transition
  comes from `READ_ONLY_OPTIMAL`, never `UNDEFINED`. `push.exposure_image` is
  patched per pass: the storage handle for the exposure passes, the sampled
  one for tonemap.
- **The first frame, and every frame under `ENCKE_FIXED_TIME`, jumps straight
  to the metered value**, starting from `Scene::ev100` if nothing was metered.
  Pinned-time captures therefore match across runs, and the average is summed
  serially in a fixed order for the same reason.
- **Metering averages; it does not weight.** With only a couple of emissive
  lamp heads on screen against empty sky, the camera exposes for the lamps and
  anything dimmer darkens. Centre weighting would change that; there is none.

Known gaps, seen 2026-09-22 and not yet worked on:

- **Bright frames wash colour out, most visibly on the glTF helmet.** Looking
  at it from a mostly shaded view (the table in the tower's shadow), the meter
  raises exposure and sunlit colour goes pale; from a mostly dark view the
  same helmet looks right. sRGB was checked and ruled out: every colour map
  and target is `_SRGB` where it should be, and ORM and normals are `UNORM`.
- **Tested 2026-09-23: exposure is the main cause, the curve decides how
  much colour survives it.** Every display curve desaturates as a colour
  climbs its shoulder, since a channel at 1.0 can only get brighter by the
  others rising to meet it. Over the helmet at the metered exposure, PBR
  Neutral kept the most saturation, ACES less and AgX least, and Neutral
  still led at matched brightness. ACES also skews hue toward yellow as
  values rise (red, gold, wood); Neutral and AgX hold hue. The captures are
  the proof, not the eye: the paleness is in the pixels.
- **Ambient is a two-colour environment, first draft.** `shade_environment`
  in `lib/pbr.slang` replaces the old flat ambient: sunlit ground below the
  horizon (its radiance worked out per frame from the star and the nearest
  body's `Body::ground_albedo`), sky above it at `Body::sky_fill` of that.
  With an atmosphere drawn, both come from the GPU instead (see *Atmosphere*):
  the sky is the air's own radiance and the ground is lit through it. Diffuse
  is exact for it; specular reads it along the reflected ray with the horizon
  softened by roughness, scaled by Karis's analytic environment BRDF. It
  gives metal its shape (dark above, lit below). It knows nothing of nearby
  objects, and there is no horizon occlusion, so normal-mapped grooves seen at
  a grazing angle reflect "ground" they could not see and draw bright lines
  (the planks show it).

The push constants are at 124 of the guaranteed 128 bytes.

## Atmosphere — built, first draft

Hillaire's "A Scalable and Production Ready Sky and Atmosphere Rendering
Technique" (EGSR 2020): Rayleigh and Mie scattering, Mie and ozone
absorption, single scattering marched and every higher order read from a
LUT. `shaders/lib/atmosphere.slang` is the model, `shaders/atmosphere.slang`
the passes. It works from the ground, from altitude and from space without
switching models: the same march runs over whatever part of a view ray is
inside the shell.

- **An `Atmosphere` component sits beside a `Body`** (`world/bodies.hpp`),
  SI units, the Earth's values from the paper by default; the Earth has
  one. The extract takes `atmosphere_at` the camera: the one whose top is
  nearest, which is the one it is inside if any. One per frame: from the
  Moon the Earth's air is drawn, and a second atmosphere in view is not.
- **Kilometres on the GPU**, in `gpu::Atmosphere`, a per-frame buffer the
  Frame points at (`Frame::atmosphere`). The body's centre goes through the
  view in f64 like everything else. **The camera's altitude comes from f64
  too**, and the shaders work out altitudes near the camera from it
  (`altitude_at`), and the view ray's intersections with the ground and the
  top from `squared_excess`: `|o|^2 - r^2` from a centre millions of metres
  away, differenced in f32, is off by kilometres squared, enough to put the
  horizon in the wrong place from the ground.
- **The ground that shadows the air is 10 km inside the radius**
  (`kGroundDepth`). The radius is a reference sphere the terrain rises and
  falls about, by kilometres; when it was written the pole's ground was
  778 m below it, and against the sphere itself the whole scene sat in the
  planet's shadow. The air between is at
  sea-level density, which leaves a grazing ray dark anyway.
- **Transmittance and multiple scattering are LUTs built once**, when the
  atmosphere the camera sees changes (`lut_atmosphere_`), and kept in
  `READ_ONLY_OPTIMAL` between. Transmittance is Bruneton's parameterisation,
  multiple scattering Hillaire's section 5.5 (64 directions, ground bounce,
  1 / (1 - f_ms)).
- **Every frame, in the "sky ambient" section before clustering:** the
  ambient pass marches 64 cosine-weighted directions around the nearest
  body's up and writes the environment's sky and ground radiance to a
  small writable buffer, which lighting reads instead of the Frame's CPU
  values. Then, while the camera is in the air, the sky-view LUT
  (Hillaire's, 192x108): the sky's luminance by azimuth from the sun and
  zenith angle, the rows squeezed toward the ground sphere's horizon.
- **Lighting takes the sun through the air per pixel**: the transmittance
  LUT at the pixel's own altitude and sun angle, zero where the body is in
  the way. Low sun is warm and dim, and from space the terminator reddens,
  with no CPU-side sunlight change.
- **The atmosphere pass runs after lighting, in place on HDR.** Where
  nothing was drawn, the sky: from the sky-view LUT inside the air, marched
  per pixel from outside, plus the sun's disk through it. The disk's
  luminance, illuminance over its solid angle, is past RGBA16F's range and
  is scaled down whole to 60000 so it keeps its colour. Where something was
  drawn, aerial perspective: its colour times the transmittance of the air
  between, plus the air's in-scattering, marched in 4 to 32 steps by the
  segment's length. Each step integrates its segment analytically, as
  Hillaire does, so few steps keep their energy.
- **Without an atmosphere nothing changes**: the passes are skipped, the
  sky keeps the clear colour, and the environment is the CPU's.

Verified by capture: the ground view (blue sky, warm horizon band, blue in
the shadows), toward the sun, 10 km, 80 km, and from three Earth radii,
where the planet shows its limb and a reddened terminator. Captures at
99.5 and 100.5 km, either side of the switch from the sky-view LUT to the
per-pixel march, match. Per-sky-pixel marching was most of the pass's cost
before the sky-view LUT.

Known gaps:

- No aerial-perspective volume: geometry is marched per pixel, which is
  cheap for the metres to nearby things and costs up to 32 steps for
  distant terrain.
- The sun's disk has no limb darkening, and the Star has no radius: the
  Sun's angular size is `config::kSunAngularRadius`.
- No clouds, no stars, no night-sky light, no moonlight.
- The ambient is one environment for the whole frame, at the camera.

## Shadows — built, first draft

The sun is a directional light with `config::kCascadeCount` cascades; local
shadows are spot lights, at most `config::kMaxShadowedSpots` per frame, each
with one perspective map and a range past which it is unshadowed. Spots go
through the same clustered path as point lights — a point light is a spot
whose `cos_outer` is below -1, so the cone never cuts off. Spots were chosen
over shadowed point lights because a spot is one map, one view and one
culling pass where a point light is a cube of six.

`render/shadows.cpp` plans each frame on the CPU in f64: it fits the cascades
and picks the spots, and the renderer composes each map's `world -> clip` with
every model matrix, and with the camera's inverse view for the lighting
lookup, before narrowing. The GPU never sees a world position here either.

- **The sun's direction and illuminance come from the brightest `Star`
  entity** at the camera's position, per frame. Flying across a system, or
  moving the star, needs nothing else changed.
- **Cascades are bounding spheres of their frustum slice, snapped to whole
  texels in f64 world space.** The sphere's radius depends only on the split
  distances and the field of view, so turning the camera does not resize the
  map, and snapping against a fixed world anchor stops the texel grid sliding
  with the camera. Snapping in camera-relative space would not work: that space
  moves with the camera.
- **Shadow maps are reversed-Z like everything else**, D32, cleared to 0,
  compared `GREATER_OR_EQUAL`. Rasterisation bias is therefore *negative*.
  The comparison sampler's border is transparent black, depth 0, so a lookup
  off the map reads as lit.
- **Maps render through the same negative-height viewport as the frame**, so
  lookups use `ndc_to_uv` like any screen-space read.
- **Depth clamp is on for the shadow pass** (`depthClamp` is required at device
  selection), so a caster nearer the sun than a cascade's near plane flattens
  onto it and still casts instead of being clipped away.
- **The sampler binding is aliased** as `SamplerState` and
  `SamplerComparisonState` in `lib/bindless.slang`. Comparison is a property of
  the `VkSampler`, not of the descriptor type, so this is legal; slangc's
  overlap warning 39001 is disabled for it.
- **Spot selection:** shadow-casting spots within their `shadow_range` of the
  camera whose reach is in view, nearest first. The strength fades over the
  last `kShadowFadeFraction` of the range.

Known gaps:

- No blending between cascades; the seam can show as a change in softness.
- Caster culling is a bounding-sphere test against each map. Every terrain
  chunk near the camera passes it, coarse ones included, since a leaf's
  sphere reaches well past its surface.
- When a spot loses its slot to a nearer one, its shadow switches off in one
  frame. Only the range limit fades.
- Shadowed spots are limited to about 120 degrees of cone; one map cannot
  cover wider without stretching badly.
- **A spot must not sit in the plane of a face beside it.** Such a face is
  edge-on in the spot's map, and depth clamp flattens it onto the near plane
  as a solid occluder covering everything on one side of a straight line.
  The mast lights once sat on their poles' axes at exactly the height of the
  top face, and each pool was cut in half, but only within shadow range.
  They now hang 0.2 m toward their targets, 5 cm above the pole tops.
- Shadow maps are single-copy, like the G-buffer: each frame's entry barrier
  waits on the previous frame's lighting reads.

## Scene: an EnTT registry, and the extract

`Scene::registry` holds every object and light as an entity. EnTT is meant
for the sim; the renderer is kept apart from it by one boundary,
`render/extract`, which copies what is drawn into a flat `RenderList` once a
frame. Nothing in the renderer reads the registry otherwise, so the sim can
later run on its own threads without the renderer seeing half an update.

- **`Transform` is position (f64), rotation (f64 quat), scale (f32) and a
  parent entity**, local to the parent. **Scale is not inherited**: it sizes
  the entity's own mesh, and a child's position is an unscaled offset in the
  parent's rotated frame. Inherited uneven scale under a rotated child is
  shear, which the triple cannot hold. Scaling an assembly means scaling
  each part and each offset, as spawning a model does. Scale must be
  positive; propagation warns once otherwise.
- **`propagate_transforms` writes `WorldTransform`** for every entity with a
  `Transform`, adding it where missing. Each is composed once per pass,
  parents first, by walking up to the nearest ancestor already done this
  pass (a pass stamp in `WorldTransform`); no ordering of the pools is kept.
  `Scene::update` runs it after animation, so world transforms are valid
  from then until the next update. A chain deeper than 32 is treated as a
  cycle and cut; a missing parent leaves its children as roots. Both warn
  once.
- **A spot light faces its entity's -Z** (`look_rotation` builds one from a
  direction). The mast lights are children of their lamp heads, the ring
  lamps' of their bulbs.
- **The camera is an entity too**: a `Transform` and a `Camera` (field of
  view, near plane), with `Scene::camera` naming the active one. It looks
  down its -Z, and parented to something it rides along: hung off the
  spinning cube, the cube held still in frame while the world turned. The
  extract copies its world pose and lens into `RenderList::camera`, a
  `CameraView`, which is all the renderer and shadow planning read.
- **Stars and bodies are entities** (`world/bodies.hpp`). A `Star` is
  luminous intensity and colour at its entity's position, not drawn. A
  `Body` is a radius, its centre in the entity's frame (the Earth's entity
  sits at its centre, since its terrain is meshed about it) and its environment: ground albedo
  and sky fill. The extract picks, at the camera, the star giving the most
  illuminance and the body whose surface is nearest, into
  `RenderList::star` and `surroundings`; with neither, nothing lights the
  frame from outside. The Earth, the Moon and the Sun are one of each.
- **An object's GPU slot is its index in this frame's `RenderList`**, and so
  its indirect draw's `firstInstance`; a light's likewise. Neither is stable
  across frames. Per-object renderer state is keyed by entity instead:
  `Renderer::previous_models_`, an `entt::storage<f64mat4>` outside the
  registry, holds last frame's model matrix for motion vectors, and the
  renderer keeps last frame's view and projection itself. A new entity's
  first frame has no motion.
- **Mesh bounds come from the mesh**: `MeshAsset` keeps its mesh-space box
  after the vertices have gone to the GPU, and the extract turns it into a
  world bounding sphere for shadow culling. Exact for a box at any scale,
  loose for the sphere.
- **EnTT's registry header is in the PCH**, along with everything it pulls
  in: entities, storages, views, groups.

Verified when it landed: default and wide captures byte-identical to the
flat scene's, clustered and brute force byte-identical, motion vectors on
the spinner alone.

Known gaps:

- Terrain chunks are the only entities ever destroyed, and they have no
  children, so a destroyed parent's children becoming roots is untested.
  `previous_models_` is rebuilt each frame from the objects drawn, so a
  destroyed entity's entry goes with it and a recycled id never meets a
  stale one.
- The render list is rebuilt and fully re-uploaded every frame, material
  data included. Camera-relative transforms change every frame anyway;
  static per-object data could move to persistent slots filled on
  `on_construct`.

## Assets

`AssetManager` owns every asset: a slot per mesh, texture, material and
model, reached by typed handles (`assets/handle.hpp`). A handle is a slot
index and the generation the slot was at, so once slots are freed a stale
handle stops resolving instead of reaching whatever reused the slot. Nothing
frees slots yet (no eviction), so every generation is 0.

It is Vulkan-free, and only the main thread calls it. Loading work runs on
the one `AssetWorker`: a job's work runs there and returns a finishing
closure, which `AssetManager::update()` runs on the main thread once a
frame. So nothing the manager owns is ever touched off the main thread.

- **CPU data goes to the renderer through ready queues.** Once a mesh or
  texture is Ready, its data waits until `Renderer::draw` takes it
  (`take_ready_meshes`, `take_ready_textures`, after the acquire), uploads it
  within the staging budget and frees it. The manager keeps what stays
  useful without the data: names, mesh bounds, texture encoding. Residency
  is the renderer's, tracked by handle index: meshes map to geometry pool
  ids, textures to bindless slots.
- **A material has no data and no GPU state.** It names up to four texture
  handles plus tiling. Each frame the renderer resolves it: until every
  texture it names has landed (or failed, which leaves it out), the object
  draws with its flat factors, and a material with an emission texture has
  emission held at zero. A material therefore never samples a half-uploaded
  set.
- **Loads are deduplicated**: an ambientCG set by name, a model by path, a
  texture by a key naming what it is decoded from. A glTF image is keyed by
  file and image index, a packed ORM by file, both source images and the
  occlusion strength, so images shared between materials, or between
  instances of one model, decode once. Verified with a two-material test
  file spawned twice: parsed once, its image decoded and uploaded once.
- **Models load on the worker and spawn deferred.** `load_model(path)`
  returns a handle at once; the worker parses the glTF, and its finish step
  registers the meshes, textures and materials and builds the node tree.
  `Scene::spawn` makes the root entity immediately, with a `ModelSpawn`
  component; `Scene::update` instantiates the nodes the frame the model is
  Ready and removes the component. A model that fails leaves its root empty,
  logged. `ModelSpawn` can fit the model to a size and rest it on the root,
  which the helmet uses.
- **The scene asks for what it uses.** The procedural meshes are
  `add_mesh`ed by `build_test_planet` and the ambientCG sets requested
  there with their tile sizes; there is no built-in list in the renderer.
- **Collision meshes will be separate assets**, kept on the CPU; a render
  mesh's vertices are freed once uploaded.

Verified at each step by capture: moving meshes and materials behind
handles, moving glTF parsing onto the worker with deferred spawns, and
splitting materials into deduplicated textures each rendered
byte-identically to the step before.

Known gaps:

- Only meshes can be freed (`release_mesh`: the slot's generation moves on
  and the slot is reused). Textures, materials and models have no eviction,
  and nothing is reference counted.
- `std::function` holds every job, since clang64's libc++ has no
  `std::move_only_function`, so everything a job captures must be copyable;
  bulky data is captured through a `shared_ptr`.
- A texture's data waits in memory until the renderer has room to stage it;
  there is no cap on how much can be queued.

## The test planet

`Scene::build_test_planet`: the Earth is a terrain body
(`terrain::example_planet`) whose entity is a radius below `kWorldOrigin`,
at its centre, so `kWorldOrigin` is the pole of the sphere; see *The Earth as
terrain* for the meshing. The object field does not stand there. The
terrain's height puts the ground under the pole somewhere else -- 72 m above
the sphere with seed 1337, logged at startup with the pole's climate -- so
`origin()` is moved onto it. It is cold grassland and gravel there, on a
knoll the start pose looks across. The ground is
found by `terrain::GroundProbe`, which marches and bisects the point query at
the octree's finest LOD, the LOD drawn around the camera there; Surface Nets
at 0.25 m voxels sits within centimetres of it. The ground is not level: the
detail octaves make it hilly. So every object is also stood on the ground
under it (`Scene::grounded`, through the probe, kept for it):
alone, by the lowest ground under its footprint's corners, so no edge floats;
in an assembly (colonnade, gateway, table and helmet, stacked crates), by the
lowest ground under its supports, one offset for every part, so it stays in
one piece and its high side sinks a few centimetres. Objects stay upright
rather than tilting with the ground. `ENCKE_CAMERA` is
relative to the moved origin; the Moon and the Earth's entity are placed from
`kWorldOrigin`.

The Moon is a sphere of its real radius at its real distance straight up. It
is a few pixels across, as it should be; look straight up to find it.

## Textures

Ten CC0 sets from ambientCG live in `assets/textures/`, 1K JPGs, with their
source and licence in `CREDITS.md`. Five of them are the terrain's palette;
see *Ground materials*. They are read from the source tree through
`ENCKE_ASSET_DIR`, which CMake bakes in, rather than copied beside the
executable like SPIR-V: tens of megabytes that rarely change. A shipped build
would need that revisited.

`AssetManager::load_ambientcg` loads a set by name with its tile size, and
`Renderable::material` holds the handle. Each set becomes three RGBA8
texture assets, glTF's arrangement:

| Texture | Format | Holds |
| --- | --- | --- |
| albedo | `R8G8B8A8_SRGB` | colour |
| normal | `R8G8B8A8_UNORM` | tangent space, OpenGL convention (`NormalGL`) |
| ORM | `R8G8B8A8_UNORM` | R occlusion, G roughness, B metalness, packed on the CPU |

A set without an occlusion map packs 1 there; one without a metalness map
packs 0, since ambientCG leaves the map out of non-metals. The object's
albedo, roughness and metallic are glTF-style factors multiplied onto what is
sampled, so a textured object normally has all three at 1. An untextured
object (`gpu::kNoTexture` in `Object::textures.x`) skips the samples and the
factors are the material, exactly as before textures, so the untextured part
of the scene renders unchanged.

`Object::textures` is albedo, normal, ORM, emission. On a textured object
albedo and ORM are always sampled; normal and emission may be `kNoTexture`
and are then skipped. The sampler is `push.material_sampler`, one for all.

- **Every texture streams in, ambientCG and glTF alike.** Each is its own
  asset, decoded on the asset worker while the scene already draws: an
  ambientCG set's colour, normal and ORM through `load_ambientcg`, a glTF's
  images through `decode_gltf_image` and `decode_gltf_orm` over the bytes
  and paths `load_gltf` located but never decoded. Each frame the renderer
  takes what is finished and uploads it texture by texture, in completion
  order, while the staging budget allows. Until all of a material's
  textures have landed an object keeps `kNoTexture` and draws with its flat
  factors, which for a textured object means white; a material with an
  emission texture has its emission held at zero until then, or its
  emissive factor would light the whole surface. Slots are registered the
  frame the copies are recorded: fresh slots, so no pending command buffer
  can be reading them. `ENCKE_CAPTURE` waits for `AssetManager::idle()` and
  `Renderer::streaming_idle()`, so captures stay byte-identical. Only the
  1x1 whites use the blocking `Texture::init`.
- **One asset worker, on purpose.** Decoding is serial on a single thread,
  and that is a decision, not a gap: the cores are meant for the SDF workers,
  f64 field evaluation and meshing on the CPU, which will be far heavier.
  Asset decoding should not compete with them. A job that throws is caught,
  logged and fails its asset, since an exception leaving a `jthread` is
  `std::terminate`. The target links `Threads::Threads`, which older glibc
  needs for `std::jthread` and everything else ignores.
- **Mips are blitted on the GPU**, level from level, by
  `Texture::record_upload`, into a frame's command buffer when streamed or a
  blocking `submit_immediate` through `init`. An `_SRGB` blit filters in linear space, so
  the albedo mips need no special handling. The normal map's mips average to
  shorter vectors, which the shader's renormalise absorbs.
- **One sampler for every material**: trilinear, repeat, anisotropic up to
  `config::kMaxAnisotropy`. `samplerAnisotropy` is required at device
  selection. Without anisotropy the ground a few metres out blurs to mush.
- **Occlusion goes into the G-buffer's albedo alpha**, which was reserved for
  it. The lighting pass applies it to the ambient term only.

### UVs are metres, and the tangent frame is glTF's

`Vertex` is position, normal, tangent (xyz along +u, w the bitangent sign),
UV: 48 bytes. `cross(normal, tangent.xyz) * w` points to the *top* of the
image. Texture v runs down the image, because row 0 of the file is uploaded as
row 0 and Vulkan puts v = 0 there, so the top of the image is -v, and that is
the direction a `NormalGL` map's green channel means. Settled by experiment as
well as derivation: with albedo forced grey, the ground's pebbles read as
raised, lit on the side facing the sun.

Mesh UVs are metres of surface at the mesh's unit scale, not 0..1. The
G-buffer vertex shader stretches them by the object's scale along the tangent
and along the bitangent, and `Object::texture_scale` has already folded in the
material's tile size (`xyz` scale over tile width, `w` tile width over
height). One unit cube therefore textures a 12 m tower and a 12 cm table leg
at the same density. The stretch is exact wherever the tangent follows a
local axis or the scale is uniform, which covers every mesh here; a
non-uniformly scaled sphere would smear.

- **Cube**: each face maps the unit square, upright on the sides. Textures do
  not line up across edges.
- **Sphere**: u westward round the equator, v from the north pole, both arc
  length. The seam column is duplicated and each pole is a vertex per slice,
  so neither wraps nor pinches.
- **Terrain**: a cube projection from the body's centre. Each vertex takes
  the face its position points through, and u and v are its other two
  body-relative coordinates in metres, less a whole number of 16 m periods
  (`kUvPeriod`) taken from its chunk's corner on that face, in f64. **The
  offset is load-bearing.** Without it UVs reach thousands of kilometres,
  where f32 steps by a hundred texels, and the GPU's per-pixel interpolation
  rounds differently as the view turns: far from the pole the texture and its
  normal map swam, which read as the ground popping while the geometry was
  still to 0.0001 cm. Every palette material's tile divides the period, so the
  offset changes nothing; neighbours' offsets differ by whole periods, so it
  stays continuous across chunks; and a chunk's UVs are no bigger than the
  chunk. Tiles and period are powers of two, exact in f32 as in f64, so the
  offsets are whole tiles to the shader too. Triangles along the cube's edges, whose
  corners pick different faces, are smeared, and show from orbit as faint
  lines. The tangent is +u laid into the surface, with w chosen so the
  bitangent points to -v.

A glTF material is **non-tiling**: its UVs are 0..1 over an atlas, and the
renderer sends `texture_scale = (1, 1, 1, 1)`, which makes the stretch
exactly 1 at any object scale. `MaterialAsset::tiling` decides which.

### glTF nodes: instanced, hierarchy kept

`load_gltf` keeps the default scene's node tree rather than baking
transforms into vertices. Each glTF mesh is converted and uploaded once, in
its own space, and only if some node uses it; `Model::nodes` holds every
node's local transform and parent, parents first. Spawning it makes an
entity per node, parented as in the file under one root entity, and a
child entity per primitive carrying the `Renderable`: a primitive is one
mesh asset with one material, as fine-grained as the file allows, so every
part of a multi-node model stays addressable. A mesh used by eight nodes is
eight draws from one range of the geometry pool. Moving the root moves the
model. Node extras are not read.

- **glTF inherits scale and `Transform` does not**, so `load_gltf` converts:
  it composes each node's model-space matrix the glTF way, splits it into
  position, rotation and scale, and makes position and rotation relative to
  the parent's again. Exact unless an uneven scale sits above a rotated
  child, which is shear; that is logged and drawn without it. The helmet
  renders as before apart from f32 rounding: scattered single pixels of
  specular sparkle, nothing displaced.

- **A mirroring node transform is logged and drawn with the wrong winding.**
  A shared mesh cannot have its indices flipped for one instance; fixing it
  needs `vkCmdSetFrontFace` and a separate indirect draw for mirrored
  instances. No model here has one.
- **`doubleSided` is ignored, and for Blender exports that is right.** Blender
  sets it on every material whose backface culling is off, the default. A
  Blender-made ship (hull, decks, props; tested and removed 2026-09-23) was
  compared with culling disabled: no surface appeared, only back faces
  leaking through cracks at wall joins.

Known gaps:

- No specular antialiasing. Normal-mapped metal at a distance sparkles; that
  wants Toksvig or similar folded into roughness, or TAA.
- Streaming is whole textures only. A texture larger than the per-frame
  budget can never land (it is logged and left out); uploading mip by mip,
  smallest first, would fix that and give a blurry version sooner. Textures
  are never evicted either.
- ambientCG sets and their tile sizes are requested in code, in
  `render/scene.cpp`. No material description files.
- Normal strength is hardcoded at 2 in `shaders/gbuffer.slang` for every
  material, chosen by eye. It is meant to become a per-material parameter.

## Terrain noise

`src/terrain` is the procedural field for smooth-voxel terrain (Surface Nets,
with CDLOD geomorph to come), for planets at 1:1 and for asteroids. The
Earth is meshed from it (*The Earth as terrain*, below); `encke --headless` benchmarks
it. The field is `|p| - radius - height(p)`, body-relative f64 metres, negative
inside.

Two layers. **Macro** is FastNoise2 node graphs from encoded strings, one
graph per channel (height, detail amplitude, ridge blend, persistence), each
mapped `bias + scale * graph`. Graphs see f32 body-relative positions, which
resolve to about half a metre at Earth's radius, so they are for wavelengths
of a kilometre and up. **Detail** is fBm summed by hand, one Perlin evaluation
per octave, not FastNoise2's FBm node: each octave has its own lattice split,
and floor(o * f * L) is not L * floor(o * f).

- **The lattice split.** Per octave, `rotation * origin * frequency` in f64 is
  split into an int32 cell and an f32 remainder. The cell goes to FastNoise2
  as `Perlin::SetLatticeOffset`, which our port patch adds: it is added to the
  floored coordinate before the prime multiply, with wrapping int32 maths, so
  the hash sees absolute cells and the interpolation sees small floats. Only
  Perlin 3D is patched. Simplex would need the offset in skewed space.
- **One `TerrainSampler` per thread.** It owns FastNoise2 nodes, and the
  lattice offset is a member the sampler changes per octave.
- **Chunk samples are bit-exact per grid point.** Chunks are addressed in
  integer grid coordinates, base voxels from the body's centre, and every LOD's
  samples are grid points. Each octave cuts the grid into blocks
  (`anchor_block`: a power of two of base voxels, 8 to 16 wavelengths wide)
  and every sample splits about its block's corner. So a sample's anchor, and
  the exact f32 offset FastNoise2 is handed, depend on its grid coordinate
  alone. A chunk evaluates each octave once per block it overlaps, which is a
  handful of calls, since a block spans at least sixteen samples at any LOD
  that keeps the octave. The seam tests compare bits. Anchoring to the
  chunk's origin instead, as the first version did, left seams one or two
  f32 ulps apart.
- **Point queries split about the point**, since it need not be a grid point.
  They share the macro, accumulation and composition code with chunks and
  agree to f32 rounding of the output.
- **The macro lattice is fixed to the body**, `lattice_spacing` apart, or one
  voxel apart where voxels are larger. Chunks and point queries interpolate the
  same nodes. **Voxel sizes and the spacing must be powers of two**: then a
  coarse LOD's samples are nodes of the finer lattice too, interpolated with
  weights of exactly zero, and grid positions are exact in both f32 and f64.
- **Boxes of grid points go through per-axis paths**, since everything
  about a grid point that is not noise depends on one coordinate at a time.
  `MacroField::sample_grid` finds lattice cells and weights per axis and
  interpolates separably (x on every node row, then y, then z), the same
  lerps on the same operands as `sample` does per point, so the same bits.
  `box_octave` works out block offsets per axis and copies a block's values
  back a row segment at a time. Per-sample `i64` arithmetic and index
  scatters were what kept these loops scalar: AVX2 has no packed `i64`
  conversion, multiply or min/max, and no scatter. `encke --sweep` prints a
  hash of every output bit, which a change of this kind must leave alone.
- **Octaves under two voxels are skipped.** Adjacent LODs therefore carry
  different detail. At a shared face the finer chunk may pass the coarser
  one's `detail_octaves` in its `ChunkRequest`, and then matches it bit for
  bit.
- **The coarse output is for geomorphing.** `sample_chunk` with a
  `CoarseSamples` also writes, at the parent LOD's grid points from corner -1
  to cells / 2 (the corners of every parent cell holding a cell the mesher
  places vertices in, apron included), the field with the detail cut at the
  parent's octave count, and its gradient across the parent's voxel. The
  value is a snapshot of the running sum taken after that octave; the
  gradient needs one parent voxel beyond those on every side, an outer layer
  evaluated at the parent's stride. Both equal what
  the parent chunk samples and differentiates, bit for bit, given consumers
  take differences through `central_difference`.
- **Floating point in the accumulation is order-sensitive.** Bit-exactness
  across chunks holds because every chunk runs the same operations in the same
  order per sample. Reordering the octave loop, vectorising the sum
  differently for some chunks, or letting a compiler contract it (hence
  `-ffp-contract=off`) would break the seam tests, which is what they are
  for.
- **A chunk is sampled on (N+4)^3, 36^3 for N = 32**, sample s at corner
  s - 2. It follows from the mesher's ownership rule: a chunk owns corners 0
  to N - 1 and the quad of every edge running +x, +y or +z from one; the four
  cells around such an edge lie in -1 to N - 1, so vertices are placed there,
  and those cells' corners run from -1 to N. A central difference at every one
  of them needs -2 to N + 1. Change the ownership rule and this changes with
  it.
- **Determinism.** The port builds FastNoise2 with `FASTNOISE2_STRICT_FP` and
  AVX2 as its only feature set, and nodes are created at `kFeatureSet` (AVX2).
  The octave rotations are integer quaternions, divided once, and the
  frequencies come from repeated multiplication, so no libm call decides a bit
  of either. The terrain sources build with `-ffp-contract=off`, since clang
  would otherwise fuse multiply-adds that GCC in ISO mode leaves apart.
- **The oracle** is `tests/terrain/perlin_reference`: FastNoise2's Perlin 3D
  (hash, gradient set, quintic, output scale) in scalar f64 from the absolute
  position. The SIMD path matches it near the origin and at 6.4e6 m. The test
  also feeds FastNoise2 plain f32 positions there and requires them to
  disagree, which proves the tests can see the precision bug.

### The FastNoise2 port

`ports/fastnoise2` is a vcpkg overlay port (`overlay-ports` in
`vcpkg-configuration.json`), not vendored, because the patches are small:

- `lattice-offset.patch`: `Perlin::SetLatticeOffset`, and a
  `FASTNOISE2_FEATURE_SETS` cache variable passed through to FastSIMD.
- `fastsimd-mingw-clang.patch`: FastSIMD adds `-Wa,-muse-unaligned-vector-move`
  on MinGW, a GNU `as` workaround for GCC's unaligned AVX spills. vcpkg builds
  ports with whichever compiler the preset's PATH provides, so under
  clang-sanitize that is clang64, whose integrated assembler rejects the flag.
  The patch keeps it GCC-only.

The port builds its debug library at `-O2 -g` (`OPTIONS_DEBUG` in the
portfile). FastSIMD is intrinsic wrappers that only vanish once inlined; at
`-O0` every SIMD operation is a call, and clang-sanitize meshed the planet
about nine times slower than it does now. It is still the debug configuration,
so its runtime settings match encke's debug builds.

**Editing an overlay port needs a reconfigure** (`-Configure`, which is the
slow path). CMake re-runs `vcpkg install` only when `vcpkg.json` or
`vcpkg-configuration.json` changes; a changed portfile or patch is otherwise
never picked up. Bump `port-version` with it.

FastNoise2 fetches FastSIMD through CPM. A port must not download during its
build, so the portfile fetches the pinned FastSIMD commit itself and hands it
to CPM as `CPM_FastSIMD_SOURCE`. The version is the v1.1.1 release, so that a
released Node Editor encodes graphs as this library decodes them. Upgrading
means re-pinning both commits and re-applying the patches.

FastNoise2 allocates node pools with `std::malloc`/`std::free` in pairs, and
everything else through `new`, which is mimalloc's in this binary. The
allocator tests build, use and free node trees across threads, freeing some on
threads other than the one that allocated them. They passed under
`MI_DEBUG=FULL`.

Known gaps:

- No Node Editor live link. The port builds without the editor tools, and
  its `NodeEditorIpc` library is not packaged.
- `example_planet` stands in for authored graphs; its channel graphs are built
  in code through `GraphBuilder`.
- Bit-exactness holds within one build. GCC and clang builds of FastNoise2
  are not known to agree with each other.

### The Earth as terrain: an implicit octree

`terrain::TerrainOctree` meshes every entity carrying a `PlanetTerrain` (its
`BodyTerrain`: radius, seed, graph set) as an implicit octree of chunks,
chosen each frame from the camera. Nothing stores the tree: a node is a LOD
and a grid corner (`NodeKey`), and its children are found by arithmetic.

- **Selection** (`select_leaves`) descends from eight roots at `root_lod`, the
  least LOD whose chunk reaches past the bounding radius, and splits a node
  while the camera is within `config::kTerrainSplitFactor` of its edges of
  it, down to `config::kTerrainFinestLod`. A leaf's voxel therefore covers
  about the same angle everywhere. Leaves are disjoint.
- **Culling** (`chunk_may_have_surface`) skips a node, and all inside it, when
  |SDF| at its centre is more than `config::kTerrainCullFactor` half-diagonals
  plus `detail_bound_from` the octaves its LOD leaves out, the most those
  could move the field; so a culled parent never hides surface its children
  would have. The field's gradient is 1 plus the terrain's slope, so the
  factor is the Lipschitz bound the cull trusts, and 1 is not safe.
  `planet_test` checks every culled chunk of a rough asteroid at one LOD, and
  that a factor of 0.3 does lose surface, which checking only the culled
  chunks next to kept ones finds too: the surface is closed and connected, so
  if it crossed a culled chunk it would cross one of those. Results are
  cached per node for the session. The factor is 2.0: `planet_test` also
  measures the example planet's slope at its surface, over half a chunk edge,
  and its mountain belts reach about 1.9 at LOD 4 and 1.6 at LOD 8, over the
  1.5 it used to be. The looser cull keeps about a third more candidate
  chunks near the surface, which settle has to sample.
- **Meshing** runs on `WorkerPool`: `std::jthread`s, like `AssetWorker`, one
  per physical core less one, at background priority. A free worker takes the
  queued job with the lowest priority, which the octree rewrites every frame
  as the chunk's distance to the camera, so the nearest chunk now is meshed
  next, however long ago it was queued; the ground under the camera comes
  first and the horizon last. A node the camera leaves before its job starts
  is skipped by a flag the job reads, its priority dropped below every
  distance so the skip clears the queue at once. A job first probes its
  chunk on a grid four voxels apart (`certainly_empty`, through
  `TerrainSampler::sample_grid`): if every probe is on one side of the
  surface by more than 5 times the half-diagonal between probes, the
  field's slope bound with room to spare, the chunk has no surface and is
  finished empty. Most jobs mesh nothing, and the probe is about a
  sixtieth of the samples. Otherwise it samples its chunk with
  its worker's own `TerrainSampler`, runs `surface_nets` and converts to
  `Vertex`; its completion, run by `WorkerPool::drain()` on the main thread,
  adds the mesh asset. `TerrainOctree::update` runs after the drain and before
  `Scene::update`.
- **Swaps leave no holes.** A node on screen that the camera no longer wants
  stays until what replaces it is ready -- its target ancestor when merging,
  every leaf inside it when splitting -- and then goes in the same frame they
  appear. Nodes on screen never overlap. A hidden node's entity is destroyed
  and its mesh released (`AssetManager::release_mesh`). `octree_test` flies a
  camera onto an asteroid and off again, checking every frame that nothing on
  screen overlaps, that the settled set is exactly the leaves, and that
  released meshes no longer resolve.
- **Chunks are small near the camera,** so their f32 vertices, metres from
  the chunk's corner, are small numbers there. That is what fixed the ground
  popping by millimetres under a yawing camera: under the uniform LOD 17 the
  triangle under the pole had vertices 78 km from their chunk's corner, where
  f32 steps 7.8 mm.
- **Mesh release** frees the pool ranges once no frame in flight can draw
  them: the renderer takes released handles before this frame's additions
  (a released slot may be reused in the same frame), and hands the pool ids
  to `GeometryPool::release` after `stage()`, since released before it they
  would be freed under the previous frame, which may still draw them.
- **Surface Nets** (`terrain/surface_nets`) puts one vertex per crossed cell at
  the average of its edge crossings, with the normal from the corners'
  central-difference gradients interpolated trilinearly. Vertices in cell -1
  repeat the neighbour's, computed from the same samples, which is what closes
  the seams. `surface_nets_test` merges chunks of an analytic sphere by global
  cell and requires every directed edge exactly once each way, outward
  winding, and normals within 1.5 degrees of the analytic.
- **Materials**: five blended per vertex by climate, altitude and slope; see
  *Ground materials*. UVs are described under *Textures*.
- **Captures** wait for `TerrainOctree::idle()`, every body showing exactly
  its leaves, as well as asset streaming. The camera must not move.
- The test scene's object field stands on the ground below the pole; see
  *The test planet*.

### Geomorph and seams

Every chunk morphs toward its parent LOD in the vertex shader, and the same
morph closes the seams between LODs. No chunk is ever remeshed because a
neighbour changed.

- **A vertex's target is its parent cell's vertex.** `surface_nets`, given
  the chunk's `CoarseSamples`, computes for each vertex the vertex the parent
  LOD's mesher places in the parent cell holding it, and its normal, through
  the same `cell_vertex` the fine cells use. The coarse values and gradients
  are bit-exact with the parent chunk's own samples, so the target is the
  parent's own vertex to f32 rounding of the chunk offset
  (`surface_nets_test` checks under 1e-5 m against real parent chunks). A
  fully morphed chunk collapses onto its parent's surface: quads whose cells
  fall in one parent cell go degenerate, and the rest become the parent's
  quads, wherever fine and coarse agree on which parent cells the surface
  crosses. Where the parent cell has no vertex, the target is the vertex
  itself.
- **The targets are a second vertex stream.** `TerrainVertex` (target
  position, packed octahedral normal, and the ground materials, 20 bytes)
  lives in `GeometryPool`'s terrain buffer, element for element beside the
  vertex buffer, uploaded with the mesh when it has one. The G-buffer and
  shadow passes load it (`load_terrain_vertex` in `shaders/lib/morph.slang`)
  at `SV_VulkanVertexID`, which is
  `gl_VertexIndex` and includes the draw's `vertexOffset`. **Not
  `SV_VertexID`**: Slang lowers that to `gl_VertexIndex - gl_BaseVertex`, as
  it does `SV_InstanceID`, and every chunk read the first mesh's targets,
  which drew the terrain as a fan of slivers.
- **The amount is by distance from the camera**, the vertex's view-space
  length, from `Geomorph::start` to `end` (`geomorph_for`). The octree merges
  a node's children once the camera is 2kE from the node, k the split factor
  and E the child's edge, and every child vertex is then at least that far
  less a voxel, so the morph ends a further voxel short of 2kE and a merge or
  a split swaps identical surfaces. It starts at (k + 1)E, or kE at the finest
  LOD, which has no finer neighbours. The root never morphs.
- **Seams are closed by masks, not by distance.** Each chunk's `Geomorph`
  carries which of its 26 neighbours on screen are one LOD coarser and which
  finer (`TerrainOctree::update_neighbours`, rerun for the neighbourhood of
  every node that comes on or goes off). Near a coarser neighbour a vertex is
  pulled fully onto its target, which is that neighbour's own vertex; near a
  finer one it is held at its own position, which is the finer neighbour's
  target. The weight is 1 within 1.25 voxels of the face, which covers every
  vertex two chunks share, and fades over 4 more; for an edge or corner
  neighbour it is the product over the faces between. Two chunks sharing a
  vertex therefore agree on where it goes. Where a coarser and a finer
  neighbour meet at a corner, coarser wins.
- **Touching leaves differ by at most one LOD**, which the masks assume.
  Split factors over sqrt(3) guarantee it; `octree_test` checks.
- **Shadows morph too.** `shadow_depth.slang` finds the object from its
  instance (`instance % counts.z`, the max object count in the Frame) and
  morphs by distance from the camera, not the light, so the terrain casts the
  surface it draws.
- **`ENCKE_NO_GEOMORPH`** draws every chunk at its own vertices, which is
  what the terrain looked like before: the comparison. Captured from 300 m
  looking straight down, off shows the old crack lines outlining each finer
  square and on shows none.
- **Skirts cover what the morph cannot close.** Where fine and coarse
  disagree on which parent cells the surface crosses, a fine boundary
  vertex has no coarse vertex to land on, and the seam kept holes a
  triangle wide that opened to the sky. `add_skirts` in
  `terrain/planet.cpp` hangs a strip from every edge only one of a
  chunk's triangles uses, `kSkirtVoxels` (4) of its voxels inward along
  each vertex's normal; the copies keep the same offset from their morph
  targets, so a skirt morphs with its edge. A hole now shows a short
  streak of stretched texture instead of sky. Found with the user's F3
  poses: the same view with `ENCKE_NO_GEOMORPH` showed the crack whose
  notches the holes were.
- **Folded triangles are not holes.** Morphing real chunks near the pole
  flips about one triangle in a thousand by halfway, but a folded
  triangle is always overlapped by its neighbours, so culling it leaves
  the ground covered twice there, never uncovered. Drawing terrain
  two-sided was tried and changed nothing but pixels.
- **Palette UVs come from the morphed position**, in the vertex shader:
  the chunk's cube face (`Geomorph::face`, from its centre) picks the two
  coordinates of mesh space plus `period_offset`, which are the mesh's own
  UVs up to whole periods. Baked per vertex, a vertex pulled toward its
  target dragged its texture with it, which showed as a mangled band along
  faces next to a coarser neighbour. A chunk straddling a cube edge now
  projects all of it on one face, where each vertex used to pick its own.
- **Detail follows the split factor now.** A chunk's box can reach much
  closer to the camera than the surface inside it, most of all from above,
  and before geomorph that slack showed as extra detail. Now a vertex past
  its LOD's `end` draws at the parent's detail whatever the octree chose, so
  from altitude the ground is softer than it was; `kTerrainSplitFactor` is
  the knob. `select_leaves` also counts a node as no nearer than
  `surface_clearance`, the camera's field value over the cull's Lipschitz
  bound, so those chunks are not meshed only to be morphed away; the bound
  is loose (1.5) and helps little below a few hundred metres.

Known gaps:

- Where fine and coarse disagree on which parent cells the surface crosses,
  collapse is not exact: a feature smaller than a coarse voxel folds onto
  the parent's vertices, and at a seam there the skirt shows through as a
  streak.
- Motion vectors ignore the morph: the previous frame's position is this
  frame's morphed vertex through last frame's matrices.
- The objects stood on the ground were placed on the finest LOD's surface;
  morphed ground more than 16 m away can sit a little above or below them.
- While meshing lags the camera, a node kept on screen past its time is
  morphed for the distance it is drawn at, not the one the octree meant, and
  can pop when it finally goes.
- Settling at the pole from a cold start meshes a few thousand chunks, most
  of them at LOD 0 with ten detail octaves; it takes seconds on the pool.
- The cull cache grows with where the camera has been; nothing prunes it.
- `GeometryPool::release` and the renderer's side of mesh release run only
  when the octree swaps, which no test drives: `octree_test` stops at the
  asset manager.
- The octree reads the camera's world transform from the previous
  `Scene::update`, a frame behind.
- At LOD 0 the detail octaves are steeper than the cull factor over a chunk,
  about 2.5, and were before the mountain belts too (2.4). A chunk whose
  centre is further from the surface than 2 half-diagonals, on ground that
  steep, is culled with surface in it. No hole has been seen; `planet_test`
  leaves LOD 0 out of its check rather than pretend otherwise.

### The example planet: continents, ranges and climate

`terrain::example_planet` builds its macro graphs in code with
`terrain::GraphBuilder`, which assembles FastNoise2 nodes (simplex, FBm,
ridged, domain warp, add, multiply, remap, power) and encodes them as the
Node Editor would, so an authored graph can replace any of them.

- **Height**: domain-warped continents, a few kilometres of relief over
  hundreds, plus mountain belts: a low-frequency mask, 0 across most of the
  planet, times ridged noise at 60 km down to spurs of 4 km. FastNoise2's
  ridged fractal is `1 - |n|`, high almost everywhere with narrow valleys, a
  plateau with gullies; raised to the power 2.5, most of it falls away and
  the crests stay, which is a range. Peaks reach about 4.5 km. Every term is
  bounded so the graph stays inside [-1, 1], which `height_bound` assumes.
- **The detail layer follows the belts**: rougher and more ridged in them,
  through the same mask built again in those channels' graphs with the same
  seed offset, which gives the same values.
- **Climate** is two more macro channels, Temperature and Moisture, which the
  field never samples (`MacroField::sample` takes a channel range; the field
  asks for the first four). `TerrainSampler::sample_climate` reads them at
  mesh vertices. They only perturb what latitude and altitude set; see
  *Ground materials*.

### Ground materials

Terrain chunks blend five ambientCG sets, `TerrainPalette` in
`render/components.hpp`, in this order: gravel (Ground110), rock (Rock051),
grass (Grass004), snow (Snow010A), sand (Ground093C).

- **Weights are per vertex, computed on the CPU** when a chunk is meshed
  (`ground_materials` in `terrain/planet.cpp`), and packed as four bytes in
  the chunk's `TerrainVertex` stream beside its morph target: rock, grass,
  snow, sand, with gravel whatever they leave. Rock is by slope against the
  body's radial up. Among what rock leaves: snow where it is cold, and more
  readily on wet ground, and only on the flat; sand where it is hot and dry;
  grass where it is mild and wet; gravel for the rest.
- **Climate is latitude and altitude, perturbed by noise.** 26°C at the
  equator, 20 degrees colder at a pole, 6.5 per kilometre up, plus the
  Temperature channel's few degrees. Moisture is the Moisture channel, less
  a dry belt around 28° and plus a wet one at the equator. So the tropics
  are green, the subtropics carry deserts, high ground is snowbound at any
  latitude, and the pole, near the radius, is cold grass and gravel.
- **The G-buffer samples only the two heaviest materials** of the five, with
  explicit UV gradients: which two varies across a quad, and implicit
  derivatives in that divergent flow are undefined. Each material has its
  own tile; the object's UVs arrive in metres (the renderer sends a texture
  scale of 1 for palette objects) and each sample divides by its tile. The
  second is skipped where its share of the two is under 5%, the share
  remapped so the blend stays continuous.
- **Rock is triplanar**, projected along the mesh axes, which are the
  body's, and blended by the mesh normal to the fourth power; a projection
  under 5% is skipped. Normals are whiteout-blended with the sign fixes
  from Ben Golus's triplanar article, plus a negated green because v runs
  down the image. The positions are mesh space plus
  `Geomorph::period_offset`, the chunk's corner less whole `kUvPeriod`s in
  f64, carried as `Object::period_offset`: continuous across chunks and
  small, like the UVs. The other four materials keep the cube projection;
  they lie on gentle slopes by construction. Checked by forcing every
  material through it at the pole, where the Y projection must and does
  match the cube projection's features and lighting.
- **Roughness is floored per material and grows with distance.** Each map's
  roughness is remapped into [floor, 1] (`TerrainPalette::roughness`,
  0.55 for snow to 0.8 for grass): Grass004's map averages 0.26, and under
  the low sun the ground glared like wet stone and foil. Then Toksvig: a
  mipped normal-map sample is shorter the more bump the distance has
  averaged away, and that variance is added to the GGX alpha, so far ground
  does not turn glossy as its relief flattens.
- **The palette is per frame** (`Frame::terrain`, a buffer of
  `gpu::TerrainMaterial`), each material resolved as an object's is: its
  maps once all have landed, its flat colour until then. `morph_masks.z` is
  the terrain object's flags: 1 morphs, 2 blends the palette.
- The scene logs the climate channels at the pole on startup, for tuning.

Known gaps:

- Blending is linear between the two heaviest materials, over the distance
  between vertices; no height-based blend, so transitions are soft washes.
- Only rock is triplanar. Snow, sand, grass or gravel on an overhang,
  which only a blend near rock produces, still stretches.
- The X and Z projections have not been seen on a real cliff; the pole
  has none near it.
- One palette for every body.

## Camera control

`render/fly_camera` flies the camera entity's local `Transform`, so a
camera parented to a ship flies relative to it, editor-style: everything happens while
the right mouse button is held. It is six-degrees-of-freedom: mouse yaws and
pitches about the camera's own axes, Q/E roll, WASD moves along the view,
Space/Ctrl strafe along the camera's up, Shift is 5x, Alt is 0.2x, and the
wheel scales the base speed by 1.25 per notch between 2 m/s and 10,000 km/s.
Position is f64 like the rest of the world.

The time step is wall clock from one fixed point in the loop to the same
point next iteration, clamped to 0.1 s. It was once measured from where the
previous `draw()` returned, which leaves out the fence wait, acquire and
present -- most of a frame -- and made movement slow and violently uneven.

- **Holding the button hands mouse and keyboard to the camera**: relative
  mouse mode hides the cursor, and the UI gets `ImGuiConfigFlags_NoMouse |
  NoKeyboard` until release. A right click that lands on a UI window stays
  with the UI.
- **Space is safe only because the UI is blocked while flying.** ImGui's
  keyboard navigation activates the focused widget on Space; with
  `NoKeyboard` set for as long as the button is held, it never sees it.
- **Losing focus counts as release**, since the button-up may never arrive.
- **The camera is flown before `Scene::update`**, which composes its world
  transform for the frame; flown after, it would draw a frame late. Last
  frame's view belongs to the renderer, recorded when a frame's buffers are
  written, so this order costs motion vectors nothing.
- **The camera has no up but its own.** "+Y is up" is the convention for
  matrices and model space, not the direction away from the ground, and the
  camera is not levelled against any body: it has to fly from one planet to
  another. Nothing in `FlyCamera` reads world +Y or a planet.
- **The environment's up is the nearest body's**: radial from the centre
  of whichever `Body` has its surface nearest the camera
  (`surroundings_at`). Flying to the Moon switches to its up and its
  regolith; it switches, rather than blends, halfway.
- **`ENCKE_CAMERA` takes an optional up**, `"px py pz tx ty tz ux uy uz"`,
  defaulting to the test scene's +Y. A look-at needs one: keeping the
  camera's previous up instead turned its starting pitch into roll.
- The test scene's own placements are still authored as translations from
  the pole, which only works because the pole's normal is world +Y; the SDF
  scenes will replace it.

## CPU-written buffers are host-coherent, required

Every buffer the CPU writes (`Buffer::init_mapped`, and the staging buffer in
`init_device`) is allocated with `requiredFlags = HOST_COHERENT`. On
non-coherent memory a CPU write can sit in a CPU cache the GPU does not see
until `vmaFlushAllocation`, and nothing here flushes. Requiring coherence means
no call site has to remember to. It cannot fail — the spec guarantees a
`HOST_VISIBLE | HOST_COHERENT` type — and it still lets VMA pick
write-combined or resizable-BAR memory where that memory is coherent, as it is
on every desktop driver. Before this the requirement was implicit and happened
to hold on this NVIDIA driver.

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

The test planet's pole is at (1e6, 2.5e5, -7e5) metres on purpose. Set
`kWorldOrigin` in `render/scene.cpp` to zero and the render must not change.
Measured: a 1 mm offset 3 m ahead of a camera at 1,000 km survives exactly
camera-relative, and collapses to 0.000000 if world positions are narrowed to
f32 first.

### Antialiasing: TAA, built, first draft

**TAA**, because it handles shading aliasing (specular, normal maps) that
MSAA cannot, and deferred makes MSAA expensive since the G-buffer would need
to be sample-rate. The same machinery later feeds temporal SSAO/SSR
denoising and FSR-style upscaling. `shaders/taa.slang`, after Karis (2014)
and Playdead's INSIDE talk (2016).

- **The jitter is in the projection's third column** (GLM `m[2][0]`,
  `m[2][1]`), a Halton (2, 3) point within the pixel, cycling through
  `config::kTaaJitterCount` and restarting whenever the history is
  discarded. Only this frame's rasterising projection carries it: the
  Frame's and every object's mvp. **`view_at` in `lib/cluster.slang` takes
  it back out**, reading those two terms, so lighting, clusters, the
  atmosphere and the debug views all reconstruct the surface the
  rasteriser drew. Anything that reconstructs a view position without
  `view_at` must do the same.
- **Motion vectors are unjittered.** Last frame's mvp is built from the
  unjittered projection, and the G-buffer takes this frame's jitter back out
  of its current position. The jitter is the camera's, not the scene's.
- **The sky has no motion vector**, so the resolve reprojects it by the
  camera's rotation alone, through `Frame::sky_reprojection`.
- **The resolve** takes motion from the nearest surface in the 3x3
  neighbourhood, samples the history Catmull-Rom in five bilinear taps,
  clips it toward the neighbourhood's mean within one standard deviation in
  YCoCg, and blends in a tenth of the new frame. Clipping and blending
  happen on colour compressed as c / (1 + luma) after last frame's
  exposure, so the sun's disk and specular glints do not leave fireflies.
  The history is stored linear and unexposed. The neighbourhood is cached
  in group-shared memory, a 10x10 tile per 8x8 group.
- **Two history images, ping-ponged.** The resolve reads one and writes the
  other, then patches `push.hdr_storage` and `push.hdr_sampled` to the one
  written, so exposure and tonemap read it in HDR's place with no copy. The
  history just written is handed to tonemap in `READ_ONLY_OPTIMAL` with
  compute in the barrier's destination too, where next frame's resolve
  samples it. For the same reason the EV100 image's handoff now names
  compute, and its transition back to storage waits on compute as well.
- **A frame with no history passes HDR through untouched**, not round-tripped
  through last frame's exposure. That exposure was metered from the history
  being discarded, and carried it into the captures that followed.
- Verified by capture: TAA captures byte-identical across runs; clustered
  and brute force byte-identical through it; TAA-off captures byte-identical
  across runs; edges on the hills, masts and lamp heads resolved; no ghosting
  on the spinning cube.

- **CAS gives back what the history blend softens.** AMD's Contrast
  Adaptive Sharpening, ported to Slang in `lib/cas.slang` (its MIT notice
  kept there, as the licence requires), runs in the tonemap pass on display
  values, which it needs in [0, 1]: each pixel takes its 3x3 neighbourhood
  through exposure and the curve, nine evaluations, and sharpens by
  `config::kCasSharpness` through `Frame::post`. Only with TAA on. Its
  strength follows local contrast, so the edges against the sky do not ring;
  on the ground texture TAA with CAS is back to about the crispness of no
  TAA.

Known gaps:

- Motion vectors ignore the geomorph (see *Geomorph and seams*); the clip
  absorbs it.
- No reactive mask: transparent or particle effects, when there are any,
  will need one.

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

**With mimalloc on, the C++ runtime is linked statically** (`-static-libstdc++
-static-libgcc`). A replacement `operator new` does not reach into a DLL on
Windows, so with `libstdc++-6.dll` anything the DLL allocates (it uses
`malloc`) and encke's inlined code frees goes to mimalloc's `free`. That
corrupts mimalloc's pages silently and crashes somewhere unrelated later, or
spins a core on a garbage lock. `std::filesystem::path` was the first thing to
do it; a build with `-DMI_DEBUG_FULL=ON` named it at once as
`mi_free_size: invalid pointer`, with a stack through `~path`. That is the
tool for any future heap corruption under mimalloc. Check with `objdump -p
encke.exe`: no `libstdc++-6.dll` among the imports.

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

**ASan's container-overflow check is off** (`__asan_default_options` in
`src/core/sanitizers.cpp`, which the runtime reads before main; it takes
effect under this MinGW toolchain). It depends on every piece of code that
touches a `std::vector` annotating it, and the vcpkg ports are built without
sanitizers: FastNoise2's graph decoder grows a vector in its own
uninstrumented code while the linker hands it some of our instrumented
template instances, and ASan aborted inside FastNoise2 on a false positive
once the macro graphs were deep enough to make it reallocate. Heap and stack
redzones still catch real overflows.

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

### Tests

Catch2 v3, from vcpkg. Every source but `main.cpp` builds into `encke_core`,
an OBJECT library that both `encke` and `encke_tests` link, so the tests run
the app's own objects under the same flags, defines and allocator. It is an
object library and not a static one because `core/memory.cpp` replaces
`operator new`: from an archive, that object would link only if something
happened to reference it. Tests live in `tests/`, mirroring `src/`, one
`<name>_test.cpp` per unit, each listed in `encke_tests`'s sources.

```powershell
.claude\cmake.ps1 -Test                            # build, then ctest
build\clang-sanitize\encke_tests.exe "[camera]"    # one tag, directly
```

`catch_discover_tests` runs in `PRE_TEST` mode, listing the tests when ctest
starts rather than after each build, so building never executes the binary.
Under clang-sanitize it needs `F:\msys2\clang64\bin` on PATH for the ASan
runtime DLL; `-Test` sets that, a bare `ctest` from a terminal does not.
`encke_tests` carries its own PCH, since clang rejects one built under another
target's include directories.

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
  appends. `release` and `clang-sanitize-base` each set their own, so a flag
  added to `gcc-debug` reaches neither.
- **`release` is `-O3` and has no `_GLIBCXX_ASSERTIONS`.** It once inherited
  both the define and RelWithDebInfo's `-O2`. The assertions put a bounds
  check on every `span` and `vector` subscript, which stops GCC vectorising
  the loop, and `-O2`'s cost model rejects most of the loops that remain; the
  terrain sampler's own loops compiled to no AVX2 at all. Only the two
  together vectorise them. `encke --sweep` measures it, and the terrain
  checksum is unchanged by it. Debug builds still get the assertions from
  `CMakeLists.txt`.
- Changing a preset's cache variables needs `-Configure`: CMake re-runs itself
  when `CMakeLists.txt` changes, but it does not re-read presets.
- `CMAKE_EXPORT_COMPILE_COMMANDS` is on, so `build/<preset>/compile_commands.json`
  drives clangd.

## Dependencies and build settings that constrain code

Most deps come from vcpkg manifest mode (`vcpkg.json`, pinned via a baseline in
`vcpkg-configuration.json`): `volk`, `vulkan`, `vulkan-memory-allocator`,
`sdl3`, `sdl3-image`, `glm`, `fastgltf`, `cpuinfo`, `bshoshany-thread-pool`,
`entt`, `catch2`, and `fastnoise2` from the overlay port in `ports/` (see
*Terrain noise*).
Three come from CPM instead: mimalloc (see *Allocator*), and Dear ImGui and
ImPlot.

- **BS::thread_pool is here for its native extensions only.** `WorkerPool`
  lowers its threads' priority and names them through them. Standard C++ has no thread priority, affinity or naming,
  and `native_handle()` does not help: under MinGW it is a winpthreads
  `pthread_t`, not a Win32 `HANDLE`, and Linux's per-thread nice value wants
  a kernel thread ID. `platform/thread` wraps the library's
  `BS::this_thread` calls, which act on the calling thread, so a `jthread`
  sets its own priority and name from inside its function. Only
  `platform/thread.cpp` includes the header, with
  `BS_THREAD_POOL_NATIVE_EXTENSIONS` defined, because on Windows it pulls in
  `<windows.h>`. The port installs no CMake package, hence the `find_path`.
  Verified once on MinGW with a temporary call from the asset worker, since
  removed: both priority and name succeed. Untested on Linux, where an
  unprivileged thread may lower its priority but never raise it: lower the
  workers, never raise the main thread. Its pool itself is unused: the job
  system is `platform/worker_pool`, on `std::jthread`.

- **cpuinfo (pytorch/cpuinfo) is for sizing worker pools by physical core.**
  Hyperthreads share a core's vector units and caches, so heavy SIMD workers
  count physical cores; the plan is one core left for the window and Vulkan
  thread. Only `platform/cpu.cpp` includes it. It builds on MinGW through
  the vcpkg port unpatched. Debug builds print its `Debug (cpuinfo)` topology
  trace to stdout at startup, because the port compiles the debug library
  with that log level baked in; release is quiet. It reports the hardware,
  not the process's affinity or container limits.

- **sdl3-image needs its format features explicitly.** The port has no
  default features and builds with its stb backend off, so a bare
  `"sdl3-image"` loads no JPG or PNG at all. The manifest requests `jpeg` and
  `png`, which bring libjpeg-turbo and libpng.

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
