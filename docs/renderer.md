# Renderer

Clustered deferred, with a forward pass for transparency to come. Decided
2026-09-20 against Forward+ and the visibility buffer. The previous three
Enckes were Forward+, and not building the same renderer a fourth time is a
legitimate reason; do not "correct" it back on efficiency grounds. The
hardware case holds too: Pascal has no hardware ray tracing, so screen-space
techniques carry GI and reflections and want a G-buffer, and its weak async
compute blunts one of Forward+'s advantages.

The visibility buffer was set aside, not ignored: its bandwidth win is
modest at 1080p with hand-authored geometry. Its lasting advantage, no fixed
G-buffer layout constraining materials, is the reason to revisit it if the
material system starts fighting the layout. It costs mandatory bindless,
analytic UV derivatives and per-material binning.

## The frame

Orchestrated in `render/renderer.cpp`, then the UI:

| Pass | Kind | Does |
| --- | --- | --- |
| shadows | raster | depth only: the sun's cascades, then each chosen spot's map |
| G-buffer | raster | albedo/ao, octahedral normal, roughness/metallic, motion, depth; emissive seeds HDR |
| sky ambient | compute | with air: LUTs when it changed, the environment around the camera, the sky-view LUT |
| clusters | compute | one thread per froxel, every light against its view-space AABB |
| lighting | compute | view position from depth, shades against the froxel's lights, adds onto HDR |
| atmosphere | compute | with air: sky where nothing was drawn, aerial perspective over what was |
| TAA | compute | resolves HDR into the history, which exposure and tonemap read |
| exposure | compute | luminance histogram, then one group meters and adapts EV100 |
| debug views | compute | one image per open debug window; skipped when none is open |
| tonemap | raster | full-screen triangle, exposure + curve, into the sRGB swapchain |
| debug draw | raster | when on: wireframe and debug lines, depth-tested |
| overlay | raster | the UI, its own rendering scope on the UI view, `LOAD` |

Every capacity and tuning constant is in `render/config.hpp`, compile-time;
shaders read counts from the Frame buffer, so none is duplicated in Slang.
16x9x24 froxels, logarithmic in depth between 10 cm and 400 m, 64 lights a
froxel: most slices land in the first tens of metres, where a ship interior
or a landing site has its lights.

## Geometry and draws

- Every mesh lives in one `GeometryPool`: one vertex and one index buffer,
  carved by VMA virtual blocks counting in elements, so a range's offset is
  directly a `vertexOffset` or `firstIndex`. Mesh ids are dense and reused
  after `release`, which frees ranges only when the releasing frame's slot
  comes round. Meshes upload through the staging arena and draw from the
  frame they are staged in. Beside it, a storage buffer of `TerrainVertex`
  indexed like the vertices, which only terrain fills.
- Each raster pass is one indirect draw: `upload()` writes a
  `VkDrawIndexedIndirectCommand` per object, `record()` binds the pool once.
  The object index is `firstInstance`, read as **`SV_VulkanInstanceID`**;
  `SV_InstanceID` is lowered to `InstanceIndex - BaseInstance` and reads 0.
  Draw lists are built on the CPU, capped at `kMaxObjects` (4096).
- G-buffer draws are frustum-culled and sorted front to back on the CPU, by
  each object's bounding sphere in f64 (`sphere_in_view`), terrain spheres
  grown by a parent voxel for the morph.
- The G-buffer is lean: octahedral normals in RG16_UNORM, motion vectors
  RG16F storing `uv_previous - uv_current` through `ndc_to_uv`. Motion is
  per-frame displacement, so it scales with frame time; uncapped it sits
  near fp16's smallest normal, which is expected.

## Frames in flight and uploads

- Two frames in flight. `image_available` semaphores and fences are per
  frame; `render_finished` semaphores are per swapchain image, since a frame
  index maps to different images over time and signalling a semaphore with
  a pending wait is invalid.
- MAILBOX where offered, FIFO otherwise. A CPU limiter holds 60 fps on top
  (`App::limit_frame_rate`), sleeping with `SDL_DelayNS` to a deadline that
  advances by whole periods; MinGW's `sleep_for` lands on the 15.6 ms tick.
- Startup buffers are device-local, filled through `submit_immediate`, which
  waits on the queue. Per-frame uploads go through `StagingArena` instead:
  one per frame in flight, rewound after the slot's fence. Its size,
  `kStagingBytesPerFrame` (80 MiB, a 4K RGBA8 map), is the upload budget.
  Staging happens after the acquire: a frame that returns OutOfDate reuses
  its slot and would rewind the arena over copies it never recorded.
- Every buffer the CPU writes is `HOST_COHERENT`, required, so nothing has to
  remember `vmaFlushAllocation`.
- The swapchain is rebuilt only when the size differs; a resize event fires
  once at startup for the initial size.

## Colour and the UI

- The swapchain is `B8G8R8A8_SRGB`: shaders write linear and the hardware
  encodes. The clear colour is linear. Author colours in sRGB and call
  `srgb_to_linear`.
- The UI prefers a UNORM view of the sRGB swapchain (mutable format,
  `VK_KHR_swapchain_mutable_format`, optional), since ImGui's styles were
  tuned for sRGB-space blending. `shaders/imgui.slang` decodes ImGui's sRGB
  vertex colours to linear and re-encodes when writing UNORM
  (`push.ui_encode_srgb`). Without the extension it blends in linear space;
  the `srgb_ui` scenario setting forces that path.
- `ui/imgui_vulkan` is a fork of ImGui `v1.92.9b-docking`'s Vulkan backend,
  version-locked, with ImGui's MIT notice kept in both files. Upgrading means
  porting upstream's changes. It allocates through VMA, uses bindless
  textures (`ImTextureID` is a sampled-image handle plus one, since 0 is
  reserved; build it with `ImGuiVulkan::texture_id`), records uploads in
  `prepare()` with retire lists per slot, shares `gpu::Push`, and has no
  multi-viewport. A target shown with `ImGui::Image` must be in
  `READ_ONLY_OPTIMAL` through a barrier naming `FRAGMENT_SHADER`; the
  G-buffer barriers name compute only.
- The overlay is an interface (`Renderer::Overlay`) so the renderer never
  includes ImGui: `prepare(command, slot)` outside any scope, `record` inside.

## Clustering

- `shaders/lib/cluster.slang` is the single source of cluster addressing. If
  build and lighting disagree on a point's froxel nothing errors; lighting
  goes missing at boundaries.
- View distance from depth is `distance_from_depth` there and nowhere else:
  the camera's near plane, read from the projection, over the depth sample.
  Dividing the cluster grid's near instead put every position at twice its
  distance, and brute force shared the mistake.
- Key 2, `DebugView::BruteForce`, shades every light without clusters; it
  and clustered must be byte-identical (`scenarios/debug_views.json`). It
  cannot catch what both share.
- Key 3's lights-per-cluster heat map must show variation, or every froxel
  holds every light.

## Debug views and drawing

- Keys 1 and 2 choose shading, 3 to 6 toggle a window each: lights per
  cluster, normals, motion, shadow cascades. `shaders/debug_views.slang`
  writes one RGBA16F image per open window, sampled by the UI. `App::draw_ui`
  says which are open after ImGui ran, since a window closed this frame must
  not be sampled from an image the frame did not draw. Digit keys are ignored
  only while ImGui has a text field active (`WantTextInput`);
  `WantCaptureKeyboard` is true whenever a window has focus and ate them.
- F4 cycles the wireframe, F5 shows collision shapes, F6 each drawn terrain
  chunk's box; scenarios set `wireframe`, `collision`, `octree`. One raster
  pass after tonemap, into the swapchain, so its colours are display sRGB;
  skipped when nothing is on. Depth-tested against the G-buffer's depth,
  bound read-only, never written. Unjittered: both vertex stages take
  `Frame::taa.xy` back out, and pull depth 0.4% toward the camera.
- The wireframe is the G-buffer's own draws in `VK_POLYGON_MODE_LINE`,
  morphed by the same `morph_vertex`; terrain coloured by LOD. Debug lines
  are f64 world-space segments (`DebugLines`) taken to view space in f64, at
  most `kMaxDebugLines`, past which the rest are dropped with one warning.
- GPU timings (`vulkan/timestamps`) are a chain of stamps at `ALL_COMMANDS`,
  so sections tile the frame and hide any overlap. Results trail by
  `kFramesInFlight`. The G-buffer section includes any wait on the acquire
  semaphore. Timings are a relative signal within a session, never figures
  worth recording.

## Shadows

The sun is directional with `kCascadeCount` cascades; local shadows are spot
lights, at most `kMaxShadowedSpots` a frame, one perspective map each. A
point light is a spot whose `cos_outer` is below -1. Spots over shadowed
point lights: one map instead of a cube of six.

- `render/shadows.cpp` plans in f64: the brightest `Star` at the camera gives
  the sun's direction and illuminance; each map's `world -> clip` is composed
  with every model matrix before narrowing.
- Cascades are bounding spheres of their frustum slice, snapped to whole
  texels in f64 world space against a fixed anchor, so turning does not
  resize the map and the grid does not slide.
- Maps are reversed-Z like everything else, so rasterisation bias is
  negative; the comparison sampler's border is depth 0, off the map reads
  lit. Depth clamp is on, so casters nearer the sun than the near plane still
  cast.
- The sampler binding is aliased as `SamplerState` and
  `SamplerComparisonState`; comparison belongs to the `VkSampler`, so this is
  legal, and slangc's warning 39001 is disabled.
- **A spot must not sit in the plane of a face beside it**: that face is
  edge-on in the map, and depth clamp flattens it into an occluder cutting
  the pool in half. The mast lights hang 0.2 m toward their targets.

## Atmosphere

Hillaire's EGSR 2020 model: Rayleigh and Mie scattering, Mie and ozone
absorption, single scattering marched and higher orders from a LUT.
`shaders/lib/atmosphere.slang` is the model, `shaders/atmosphere.slang` the
passes. The same march runs over whatever part of a ray is in the shell, from
the ground to orbit.

- An `Atmosphere` sits beside a `Body`; the extract takes the one whose top
  is nearest the camera. One a frame.
- Kilometres on the GPU. The camera's altitude comes from f64, and ray-sphere
  intersections from `squared_excess`: `|o|^2 - r^2` differenced in f32 from
  a centre millions of metres away is off by kilometres squared.
- The ground that shadows the air is `kGroundDepth` (10 km) inside the
  radius, since terrain rises and falls about the radius by kilometres.
- Transmittance and multiple-scattering LUTs are built when the atmosphere
  seen changes. Every frame: the ambient pass marches 64 directions for the
  environment's sky and ground radiance, and in the air, the sky-view LUT.
- Lighting takes the sun through the air per pixel from the transmittance
  LUT. The atmosphere pass runs after lighting, in place on HDR: the sky from
  the sky-view LUT inside the air or marched from outside, the sun's disk
  scaled to 60000 to fit RGBA16F, and aerial perspective over geometry in 4
  to 32 analytically integrated steps.
- Without an atmosphere the passes are skipped and the environment is the
  CPU's (`shade_environment` in `lib/pbr.slang`: sunlit ground below the
  horizon, sky above at `Body::sky_fill`).

## Exposure and tonemapping

`shaders/exposure.slang` bins every pixel's log2 luminance; bin 0 takes
everything below the range and every pixel where nothing was drawn, and is
ignored: metering the sky from 80 km burned the planet to white.
`adapt_main` averages between two percentiles, meters EV100 as
`log2(L) + 3`, and adapts over wall-clock time. Tonemap's exposure is
`1 / (1.2 * 2^EV100)`, like the CPU's fixed EV.

- The state is a 1x1 R32F image registered twice, storage and sampled, since
  a buffer lives in exactly one of the read-only and writable bindings.
- The first frame, and every frame with animation pinned, jumps to the
  metered value, and the average is summed serially, so captures match.
- PBR Neutral is the default curve; T cycles ACES and AgX. Every curve
  desaturates bright colour, and exposure decides how much climbs the
  shoulder; Neutral kept the most saturation over the helmet and holds hue,
  where ACES skews toward yellow.
- The push constants are at 124 of the guaranteed 128 bytes.

## TAA

Karis 2014 and Playdead's INSIDE talk, in `shaders/taa.slang`.

- The jitter is a Halton (2, 3) point in the projection's third column,
  cycling through `kTaaJitterCount`. Only this frame's rasterising
  projection carries it; **`view_at` in `lib/cluster.slang` takes it back
  out**, so everything reconstructing a view position must go through it.
- Motion vectors are unjittered. The sky has none; it reprojects by the
  camera's rotation (`Frame::sky_reprojection`).
- The resolve takes motion from the nearest surface in 3x3, samples history
  Catmull-Rom in five taps, clips toward the neighbourhood mean within one
  standard deviation in YCoCg, blends a tenth, all on colour compressed as
  c / (1 + luma) after last frame's exposure, so glints leave no fireflies.
- Two history images ping-pong; the resolve patches `push.hdr_storage` and
  `hdr_sampled` so exposure and tonemap read the history in HDR's place.
- A frame with no history passes HDR through untouched.
- CAS (`lib/cas.slang`, AMD's, MIT notice kept) sharpens in the tonemap
  pass on display values, only with TAA on.

## Shaders and bindless

Slang, compiled to SPIR-V at build time by `encke_add_shader()`; the build
requires the Vulkan SDK. One `.slang` produces one `.spv` with every entry
point, names kept by `-fvk-use-entrypoint-name`, beside the executable in
`shaders/`. slangc writes a depfile, so editing a module rebuilds what
imports it.

- `-matrix-layout-column-major`, or Slang silently transposes every GLM
  matrix.
- Shared conventions live in `shaders/lib/`, imported as `lib.<name>`, never
  open-coded per shader: above all `lib/screen.slang`, the fragment, NDC and
  UV conversions and their Y rules, and `lib/colour.slang`.

One global descriptor set, bound once per bind point, indexed by handles in
push constants. `vulkan/bindless.hpp` and `shaders/lib/bindless.slang` must
agree; only validation checks.

| Binding | Holds |
| --- | --- |
| 0 | sampled images (`READ_ONLY_OPTIMAL`) |
| 1 | storage images (`GENERAL`) |
| 2 | read-only storage buffers |
| 3 | samplers |
| 4 | writable storage buffers, compute only |

- 2 and 4 are split so vertex and fragment stages only ever get the
  read-only view and `vertexPipelineStoresAndAtomics` stays off. A buffer is
  registered in one of them; HDR is registered twice, once per layout.
- Handles are stable across a resize: `update_*` rewrites slots in place,
  legal only because the caller waits for idle first.
- `BindlessSet::release_sampled_image` puts slots on a free list, for
  ImGui's atlas rebuilds.
- The Intel iGPU is rejected: format-less storage images need
  `shaderStorageImageReadWithoutFormat`.

## Known gaps

- The cluster pass is brute force, every light against every froxel; lists
  silently drop past 64 lights.
- G-buffer, HDR and shadow maps are single-copy, so each frame's entry
  barriers wait on the previous frame's reads.
- The acquire semaphore gates the G-buffer pass though only tonemap needs it.
- No cascade blending; caster culling is bounding spheres; a spot losing its
  slot switches its shadow off in one frame; spots are limited to about 120
  degrees of cone.
- No aerial-perspective volume; the sun's disk has no limb darkening; no
  clouds, stars or night light; one ambient environment a frame, at the
  camera, with no horizon occlusion, so grooves at grazing angles reflect
  ground they could not see.
- Metering averages without weighting: a few lamp heads against empty sky
  set the exposure.
- No specular antialiasing beyond TAA; no reactive mask for transparency.
- Motion vectors ignore the terrain geomorph.
