# Terrain

`src/terrain` is the procedural field for smooth-voxel terrain, for planets
at 1:1 and asteroids, meshed with Surface Nets in an implicit octree and
geomorphed between LODs. The field is `|p| - radius - height(p)`,
body-relative f64 metres, negative inside. `encke --headless` benchmarks it;
`encke --sweep [n]` measures every LOD and prints a hash of every output bit,
which an optimisation must leave alone.

## Noise

Two layers. **Macro** is FastNoise2 node graphs from encoded strings, one per
channel (height, detail amplitude, ridge blend, persistence, and the climate
channels), each mapped `bias + scale * graph`. Graphs see f32 body-relative
positions, about half a metre of resolution at Earth's radius, so they carry
wavelengths of a kilometre and up. **Detail** is fBm summed by hand, one
Perlin evaluation per octave, not FastNoise2's FBm node: each octave has its
own lattice split, and floor(o * f * L) is not L * floor(o * f).

- The lattice split: per octave, `rotation * origin * frequency` in f64 is
  split into an int32 cell and an f32 remainder. The cell goes to FastNoise2
  as `Perlin::SetLatticeOffset` (our port patch), added to the floored
  coordinate before the prime multiply, so the hash sees absolute cells and
  the interpolation sees small floats. Only Perlin 3D is patched.
- One `TerrainSampler` per thread: it owns FastNoise2 nodes, and the lattice
  offset is state it changes per octave.
- Chunk samples are bit-exact per grid point. Chunks are addressed in integer
  grid coordinates, and every LOD's samples are grid points. Each octave cuts
  the grid into blocks (`anchor_block`, 8 to 16 wavelengths wide) and every
  sample splits about its block's corner, so the f32 offset FastNoise2 sees
  depends on the grid coordinate alone. The seam tests compare bits.
  Anchoring to each chunk's origin left seams an ulp or two apart.
- Point queries split about the point; they agree with chunks to f32
  rounding of the output.
- The macro lattice is fixed to the body, `lattice_spacing` apart or one
  voxel where voxels are larger. **Voxel sizes and the spacing must be powers
  of two**, so a coarse LOD's samples are nodes of the finer lattice, and
  grid positions are exact in f32 and f64.
- Boxes of grid points go through per-axis paths (`MacroField::sample_grid`
  interpolates separably, `box_octave` copies a block's row segments),
  because per-sample `i64` arithmetic and scatters kept the loops scalar.
- Octaves under two voxels are skipped, so adjacent LODs carry different
  detail; a finer chunk may pass a coarser one's `detail_octaves` in its
  `ChunkRequest` to match it bit for bit.
- The coarse output feeds the geomorph: with a `CoarseSamples`, a chunk also
  writes, at the parent LOD's grid points from corner -1 to cells / 2, the
  field cut at the parent's octave count and its gradient across the
  parent's voxel. Both equal the parent chunk's own, bit for bit.
- A chunk is sampled on (N+4)^3, 36^3 for N = 32, sample s at corner s - 2.
  It follows from the mesher's ownership rule (a chunk owns corners 0 to
  N - 1 and the quads of edges running +x, +y, +z from them, so vertices land
  in cells -1 to N - 1, whose corners need central differences from -2 to
  N + 1). Change the rule and this changes with it.

### Determinism

- Floating point in the accumulation is order-sensitive: bit-exactness holds
  because every chunk runs the same operations in the same order per sample.
  Reordering octaves, vectorising some chunks differently, or letting the
  compiler contract multiply-adds breaks the seam tests, which is their
  purpose. The terrain sources build with `-ffp-contract=off`.
- The port builds FastNoise2 with `FASTNOISE2_STRICT_FP` and AVX2 as its only
  feature set; nodes are created at `kFeatureSet`. Octave rotations are
  integer quaternions divided once and frequencies come from repeated
  multiplication, so no libm call decides a bit.
- The oracle, `tests/terrain/perlin_reference`, is FastNoise2's Perlin 3D in
  scalar f64 from the absolute position. The SIMD path matches it near the
  origin and at 6.4e6 m; the test also requires plain f32 positions there to
  disagree, proving it can see the precision bug.
- Bit-exactness holds within one build; GCC and clang builds of FastNoise2
  are not known to agree.

### The FastNoise2 port

`ports/fastnoise2` is a vcpkg overlay port, FastNoise2 v1.1.1 (so a released
Node Editor encodes graphs this library decodes), with two patches:
`lattice-offset.patch` (`SetLatticeOffset`, and `FASTNOISE2_FEATURE_SETS`
passed to FastSIMD) and `fastsimd-mingw-clang.patch` (FastSIMD's
`-Wa,-muse-unaligned-vector-move` kept GCC-only; clang's integrated
assembler rejects it).

- Its debug library builds at `-O2 -g`: FastSIMD is intrinsic wrappers that
  vanish only once inlined, and at `-O0` clang-sanitize meshed the planet
  several times slower.
- A port must not download during its build, so the portfile fetches the
  pinned FastSIMD commit and hands it to CPM as `CPM_FastSIMD_SOURCE`.
  Upgrading means re-pinning both commits and re-applying the patches.
- **Editing an overlay port needs `-Configure`** and a bumped
  `port-version`; CMake re-runs `vcpkg install` only when the manifest files
  change.
- FastNoise2 pairs `std::malloc`/`std::free` for node pools and uses `new`
  for the rest, which is mimalloc's here; the allocator tests build and free
  node trees across threads and passed under `MI_DEBUG=FULL`.

## The example planet

`terrain::example_planet` builds its graphs in code with
`terrain::GraphBuilder`, which encodes nodes as the Node Editor would, so an
authored graph can replace any of them.

- Height: domain-warped continents, a few kilometres of relief, plus
  mountain belts: a low-frequency mask times ridged noise raised to 2.5 (the
  ridged fractal alone is a plateau with gullies; the power leaves the
  crests). Peaks reach about 4.5 km. Every term stays in [-1, 1], which
  `height_bound` assumes.
- The detail layer is rougher and more ridged in the belts, through the same
  mask rebuilt with the same seed offset.
- Climate is two more macro channels, Temperature and Moisture, which the
  field never samples (`MacroField::sample` takes a channel range);
  `TerrainSampler::sample_climate` reads them at mesh vertices.

## The octree

`terrain::TerrainOctree` meshes every entity with a `PlanetTerrain` as an
implicit octree chosen each frame from the camera. Nothing stores the tree:
a node is a LOD and a grid corner (`NodeKey`).

- Selection (`select_leaves`) descends from eight roots and splits a node
  while the camera is within `kTerrainSplitFactor` of its edges of it, down
  to `kTerrainFinestLod`. A node counts as no nearer than
  `surface_clearance`, the camera's field value over the cull's Lipschitz
  bound, so chunks are not meshed only to be morphed away.
- Culling (`chunk_may_have_surface`) skips a node when |SDF| at its centre is
  more than `kTerrainCullFactor` half-diagonals plus the most the octaves
  its LOD leaves out could move the field. The field's gradient is 1 plus
  the slope, so the factor is the Lipschitz bound the cull trusts; 1 is not
  safe. It is 2.0: the mountain belts reach a slope of about 1.9 at LOD 4.
  `planet_test` checks every culled chunk of a rough asteroid, and that 0.3
  does lose surface. Results are cached per node.
- Meshing runs on `WorkerPool`: one `std::jthread` per physical core less
  one, at background priority. A free worker takes the queued job with the
  lowest priority, rewritten every frame as the chunk's distance to the
  camera, so the ground underfoot comes first. A node the camera leaves
  before its job starts is skipped by a flag the job reads. A job first
  probes its chunk four voxels apart (`certainly_empty`); if every probe is
  on one side of the surface by 5 half-diagonals, the chunk is empty. Most
  jobs mesh nothing. `TerrainOctree::update` runs after the pool's drain and
  before `Scene::update`.
- Swaps leave no holes: a node on screen stays until what replaces it is
  ready (its ancestor when merging, every leaf inside it when splitting),
  and then goes in the same frame. Nodes on screen never overlap.
  `octree_test` checks that every frame of a flight onto an asteroid and off.
- Chunks are small near the camera, so their f32 vertices, metres from the
  chunk's corner, are small numbers there. Under a uniform LOD the ground
  popped by millimetres as the camera yawed.
- Mesh release: a hidden node's mesh is released, and the renderer takes
  released handles before this frame's additions and hands the pool ids to
  `GeometryPool::release` after `stage()`; released before it, they would be
  freed under the previous frame, which may still draw them.
- Surface Nets (`terrain/surface_nets`) puts one vertex per crossed cell at
  the average of its edge crossings, normals from central-difference
  gradients interpolated trilinearly. Cell -1 vertices repeat the
  neighbour's, from the same samples, which closes the seams.
  `surface_nets_test` merges chunks of an analytic sphere and requires every
  directed edge once each way, outward winding, and normals within 1.5
  degrees.
- Captures wait for `TerrainOctree::idle()`: every body showing exactly its
  leaves, and every surface map baked.
- F2 freezes the octree (`set_frozen`): nothing is selected, meshed or
  swapped while the camera flies.

## Geomorph and seams

Every chunk morphs toward its parent LOD in the vertex shader, and the same
morph closes the seams between LODs. No chunk is remeshed because a
neighbour changed.

- A vertex's target is its parent cell's vertex, computed by `surface_nets`
  from the `CoarseSamples` through the same `cell_vertex`, so it is the
  parent's own vertex to f32 rounding. A fully morphed chunk collapses onto
  its parent's surface wherever fine and coarse agree on which parent cells
  the surface crosses.
- The targets are a second vertex stream: `TerrainVertex` (target, packed
  normal, ground materials, 20 bytes) in `GeometryPool`'s terrain buffer,
  beside the vertex buffer. Shaders load it (`load_terrain_vertex` in
  `lib/morph.slang`) at **`SV_VulkanVertexID`**, which includes the draw's
  `vertexOffset`. `SV_VertexID` is lowered to `gl_VertexIndex -
  gl_BaseVertex`, and every chunk read the first mesh's targets.
- The amount is by the vertex's view-space distance, from `Geomorph::start`
  to `end`, chosen so a merge or split swaps identical surfaces: the octree
  merges children once the camera is 2kE away (k the split factor, E the
  child's edge), so the morph ends a voxel short of that, and starts at
  (k + 1)E, or kE at the finest LOD. The root never morphs.
- Seams are closed by masks, not distance. Each chunk's `Geomorph` carries
  which of its 26 neighbours on screen are one LOD coarser and which finer
  (`update_neighbours`). Near a coarser neighbour a vertex is pulled fully
  onto its target; near a finer one it is held. The weight is 1 within 1.25
  voxels of the face and fades over 4 more; for an edge or corner neighbour
  it is the product over the faces between. Coarser wins at a mixed corner.
- Touching leaves differ by at most one LOD, which the masks assume; split
  factors over sqrt(3) guarantee it and `octree_test` checks.
- Shadows morph too, by distance from the camera, not the light, so the
  terrain casts the surface it draws.
- Skirts cover what the morph cannot close: where fine and coarse disagree,
  seams kept holes a triangle wide. `add_skirts` hangs a strip from every
  boundary edge, `kSkirtVoxels` inward along the normals, morphing with its
  edge. A hole now shows as a streak of stretched texture instead of sky.
- Folded triangles are not holes: morphing flips about one in a thousand,
  always overlapped by its neighbours. Drawing terrain two-sided changed
  nothing.
- Palette UVs come from the morphed position, in the vertex shader, projected
  on the chunk's cube face (`Geomorph::face`). Baked per vertex, a vertex
  pulled toward its target dragged its texture with it.
- The `geomorph` setting, off, draws every chunk at its own vertices: the
  comparison.

## Terrain modifiers

`terrain/modifiers` holds edits in `BodyTerrain::modifiers`, applied in
`TerrainSampler::combine`'s `compose`, the one place the field is formed, in
f64. Every consumer sees them (chunks and coarse snapshots, point queries,
`certainly_empty`, the cull, collision chunks, the ground probe) except the
surface map, whose kilometre texels are coarser than any edit.

- An edit is a pure function of a sample's f64 position and its unedited
  value, which keeps chunk samples bit-exact. Each combine keeps only edits
  whose bound reaches its box; one left out would have changed the value by
  exactly nothing. `modifiers_test` compares a chunk with each point alone.
- `Flatten`, a level floor: within `radius` of the axis the field is the
  height above the plane, cutting and filling; over `blend` it eases back
  with a smoothstep; it holds `reach` above and below the plane, or the
  column would run through the planet.
- The blend steepens the field by up to 1.5 times the height difference over
  `blend`, and the cull and probe trust a gradient under 2. Keep `blend`
  several times the largest cut or fill.

## Ground materials

Chunks blend five ambientCG sets, `TerrainPalette` in
`render/components.hpp`: gravel (Ground110), rock (Rock051), grass
(Grass004), snow (Snow010A), sand (Ground093C).

- Weights are per vertex, computed on the CPU at meshing
  (`ground_materials` in `terrain/ground`) and packed as four bytes in the
  `TerrainVertex` stream: rock, grass, snow, sand, gravel the remainder.
  Rock is by slope against the radial up; among the rest, snow where cold
  (more readily wet, only flat), sand hot and dry, grass mild and wet.
- Climate: 26°C at the equator, 20 degrees colder at a pole, 6.5 per
  kilometre up, plus the Temperature channel; Moisture is its channel less a
  dry belt near 28° and plus a wet one at the equator.
- The G-buffer samples only the two heaviest materials, with explicit UV
  gradients: which two varies across a quad, and implicit derivatives in
  divergent flow are undefined. The second is skipped under a 5% share.
- Rock is triplanar along the mesh axes (the body's), blended by the normal
  to the fourth power; normals whiteout-blended with Ben Golus's sign fixes
  plus a negated green, since v runs down the image. Positions are mesh space
  plus `Object::period_offset` (the chunk's corner less whole `kUvPeriod`s),
  continuous and small.
- Roughness is floored per material (`TerrainPalette::roughness`, 0.55 snow
  to 0.8 grass; Grass004's map averages 0.26 and glared under the low sun),
  then Toksvig adds the variance a mipped normal map lost to the GGX alpha.
- The palette is per frame (`Frame::terrain`), each material resolved as an
  object's is. `morph_masks.z` holds flags: 1 morphs, 2 blends the palette,
  4 shades from the surface map.

## Surface maps and impostors

Each body's look from far off is baked once into a surface map
(`terrain/surface_map`), and past the distance its relief goes under a pixel
the body is an impostor: a sphere shaded from the map, nothing meshed. The
reason is scale: a system of a dozen bodies should cost a draw and a map each
until the camera nears one.

- A cube atlas, two RGBA8 textures three faces across and two down: albedo
  (sRGB), and tangent-frame normal, roughness and height (UNORM). Each tile
  carries a border of its own projection carried past the edge, so filtering
  and four mips never reach another tile. `kSurfaceMapFace` is 1024.
  `shaders/lib/surface_map.slang` mirrors the projection and the frame, which
  is right-handed about the outward direction, so its second axis is -t on
  negative faces; `surface_map_test` pins both.
- The bake is the chunks' own rules at the texel's scale: macro height at
  2x2 sub-samples a texel, each with its normal and `ground_materials`,
  climate once a texel, graphs evaluated at points
  (`MacroField::sample_points`). Each ground set's colour and roughness are
  what the G-buffer shows once its maps mip to a texel (`measure_ground`).
- Six jobs on the pool, one per face; the last lays out the atlas. Priority
  is the nearest a chunk shaded from the map can be (`map_priority`).
- Chunks from `kSurfaceMapLod` (16) up are shaded from the map once it has
  landed, so the root chunks and the impostor show the same colour, normal
  and roughness, and the handover changes only geometry.
- The impostor is an entity under the body with a `BodyMap` whose `impostor`
  is set, drawn by its own pipeline (`impostor_main`) after every other
  G-buffer draw, since it writes `SV_DepthLessEqual` and would cost the main
  pipeline its early depth test. Each pixel intersects its ray with the
  sphere from the ray's closest approach (not differencing huge squares),
  reads the map, lifts to the ground's height, and writes the whole G-buffer,
  motion included. It casts no shadow map.
- The handover is the octree's swap rule: past `impostor_distance` no root
  splits; past that times `kImpostorHysteresis` the impostor replaces every
  chunk; coming back, chunks replace it the frame the last is ready.
  `octree_test` checks that exactly one of the two is on screen.

## Known gaps

- Where fine and coarse disagree on which parent cells the surface crosses,
  collapse is not exact and the skirt can show as a streak.
- Motion vectors ignore the morph.
- Objects stood on the ground use the finest LOD's surface; morphed ground
  16 m out can sit a little above or below them.
- While meshing lags the camera, a node kept past its time is morphed for the
  distance it is drawn at and can pop when it goes.
- A cold start at the surface meshes thousands of chunks; it takes seconds.
- The cull cache grows with where the camera has been; nothing prunes it.
- `GeometryPool::release` and the renderer's side of mesh release are not
  driven by any test.
- The octree reads the camera from the previous `Scene::update`.
- At LOD 0 the detail octaves are steeper than the cull factor (about 2.5).
  No hole has been seen; `planet_test` leaves LOD 0 out of its check.
- Ground blending is linear between the two heaviest materials, no height
  blend; only rock is triplanar; one palette for every body.
- The surface map is magnified where map-shaded chunks are near, and texels
  show; it is baked every run and never cached.
- Impostors are traced at one height per pixel: no parallax, no limb relief,
  no mountain shadows. An impostor waits for its textures; if the upload
  budget is full when the octree switches, a frame can show neither.
- No Node Editor live link; `example_planet` stands in for authored graphs.
