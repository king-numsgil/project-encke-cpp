# Scene, assets and models

## The registry and the extract

`Scene::registry` (EnTT) holds every object and light as an entity. The
renderer is kept apart from it by `render/extract`, which copies what is
drawn into a flat `RenderList` once a frame; nothing in the renderer reads
the registry otherwise, so the sim can later run on its own threads.

- `Transform` is position (f64), rotation (f64 quat), scale (f32) and a
  parent, local to the parent. **Scale is not inherited**: it sizes the
  entity's own mesh, and a child's position is an unscaled offset in the
  parent's rotated frame, since inherited uneven scale under a rotated child
  is shear, which the triple cannot hold. Scaling an assembly means scaling
  each part and offset, as spawning a model does. Scale must be positive.
- `propagate_transforms` writes `WorldTransform` for every entity, each
  composed once a pass by walking up to the nearest ancestor done this pass.
  `Scene::update` runs it after animation. A chain deeper than 32 is cut as
  a cycle; a missing parent leaves children as roots.
- A spot light faces its entity's -Z (`look_rotation`). The camera is an
  entity too, a `Transform` and a `Camera`, looking down -Z; parented, it
  rides along. The extract copies its pose and lens into `CameraView`.
- Stars and bodies are entities (`world/bodies.hpp`). A `Star` is luminous
  intensity and colour, not drawn. A `Body` is a radius, its centre in the
  entity's frame, its environment and its surface gravity. The extract picks
  the brightest star and the nearest body at the camera.
- An object's GPU slot is its index in this frame's `RenderList`, not stable
  across frames; per-object renderer state is keyed by entity
  (`Renderer::previous_models_`, last frame's model matrices for motion).
- Mesh bounds come from `MeshAsset`, kept after the vertices go to the GPU.
- EnTT's registry header is in the PCH.

## Assets

`AssetManager` owns every asset, a slot each, reached by typed generational
handles (`assets/handle.hpp`). It is Vulkan-free and main-thread only; loading
runs on the one `AssetWorker`, whose job returns a finishing closure that
`AssetManager::update()` runs on the main thread.

- CPU data reaches the renderer through ready queues (`take_ready_meshes`,
  `take_ready_textures`, after the acquire), is uploaded within the staging
  budget and freed. Residency is the renderer's, by handle index.
- A material has no data and no GPU state: up to four texture handles and
  tiling. Until every texture it names has landed, the object draws with its
  flat factors, and emission is held at zero (or its factor would light the
  whole surface). A material never samples a half-uploaded set.
- Loads are deduplicated: ambientCG sets by name, models by path, textures by
  a key naming what they decode from (a glTF image by file and index, an ORM
  by file, both sources and the occlusion strength).
- Models load on the worker and spawn deferred: `Scene::spawn` makes the root
  with a `ModelSpawn`, and `Scene::update` instantiates the nodes the frame
  the model is Ready. A model that fails leaves its root empty.
- One asset worker, on purpose: the cores are for terrain meshing, which is
  far heavier. A job that throws fails its asset; an exception leaving a
  `jthread` is `std::terminate`.
- Every texture streams, texture by texture in completion order within the
  budget. Slots are registered the frame their copies are recorded. Mips are
  blitted on the GPU; an `_SRGB` blit filters in linear space.
- One sampler for every material: trilinear, repeat, anisotropic
  (`kMaxAnisotropy`).

## Textures and UVs

Ten CC0 ambientCG sets in `assets/textures/`, 1K JPGs, credited in
`CREDITS.md`, read from the source tree through `ENCKE_ASSET_DIR`. Each set is
three RGBA8 textures: albedo `_SRGB`, normal `UNORM` (OpenGL convention),
ORM `UNORM` packed on the CPU (occlusion 1 and metalness 0 where the set has
no map). Factors multiply what is sampled; an untextured object
(`kNoTexture`) uses the factors as its material. Occlusion goes into the
G-buffer's albedo alpha and affects ambient only.

- `Vertex` is position, normal, tangent (w the bitangent sign), UV: 48 bytes.
  `cross(normal, tangent.xyz) * w` points to the top of the image, and v runs
  down it, which is what a `NormalGL` green channel means.
- Mesh UVs are metres at the mesh's unit scale; the G-buffer stretches them
  by the object's scale, and `Object::texture_scale` folds in the tile size,
  so a 12 m tower and a 12 cm leg texture at one density.
- Terrain UVs are a cube projection from the body's centre, less a whole
  number of 16 m periods (`kUvPeriod`) from the chunk's corner in f64. **The
  offset is load-bearing**: without it UVs reach thousands of kilometres,
  where f32 steps by a hundred texels, and the texture swam as the view
  turned. Tiles and period are powers of two.
- A glTF material is non-tiling (UVs 0..1 over an atlas); the renderer sends
  a texture scale of 1. `MaterialAsset::tiling` decides.
- Normal strength is hardcoded at 2 in `gbuffer.slang`, chosen by eye.

## glTF

`load_gltf` keeps the default scene's node tree. Each mesh is converted once,
in its own space, if a node uses it; spawning makes an entity per node,
parented as in the file under one root, and an entity per primitive with the
`Renderable`. A mesh used by eight nodes is eight draws from one range.

- glTF inherits scale and `Transform` does not, so each node's model-space
  matrix is composed the glTF way, split into position, rotation and scale,
  and made relative to its parent's again. Shear is logged and dropped.
- A mirroring node transform is logged and drawn with the wrong winding;
  fixing it needs `vkCmdSetFrontFace` and a separate draw.
- `doubleSided` is ignored, correctly for Blender exports, which set it on
  every material with culling off by default.
- Node extras read: `joint` (`ModelNode::moving`), and `physics`
  (`"convex_hull"`, or `"box"` with `half_extents`) as `ModelNode::shape`. A
  shape node is never drawn and is left out of the bounds.
- `ModelSpawn` can fit the model to a size, rest it on its root, scale its
  emission to a luminance, attach further models at named nodes (the
  torchship's seats), and say how its parts collide and which ship its shape
  nodes belong to; see `docs/physics.md`.
- A part's triangles stay on the CPU as `ModelPart::collision` after its
  vertices go to the GPU.

## The test scene

`Scene::build_test_planet`: the Earth is a terrain body
(`terrain::example_planet`) whose entity is a radius below `kWorldOrigin`, so
`kWorldOrigin` is its north pole. The field stands on a site 930 km from the
pole in a valley of sand and gravel (`kSiteEye`, `kSiteHeading`).

- The scene has a frame: `origin()` is the middle of the floor and
  `rotation()` takes +Y to the ground's radial up there. Everything is
  authored in it; scenario positions are in it.
- The floor is a `terrain::Flatten` 64 m across, blended over 48, at the mean
  height of the ground under it. Objects are stood on the ground under them
  (`Scene::grounded`).
- The Sun is fixed to the Earth, 12 degrees north of the equator, 20 degrees
  up over the floor. The Moon is at its real radius and distance, placed from
  the Sun at first quarter and 5.145 degrees north of the ecliptic.
- The helmet (Khronos DamagedHelmet) sits on the table. The torchship
  stands on its tail south of the field: its exterior and interior load at
  one root, the interior parented to it, the seats attached at their nodes.
  `assets/torchship/ENGINE.md` and `SCREENS.md` are the export's conventions.

## Known gaps

- Only meshes can be freed; textures, materials and models are never
  evicted or reference counted.
- `std::function` holds every job (clang64's libc++ has no
  `move_only_function`), so captures must be copyable; bulky data goes
  through a `shared_ptr`.
- A texture's data waits in memory until there is room to stage it, with no
  cap; a texture larger than the budget never lands (no mip-by-mip upload).
- Terrain chunks are the only entities destroyed; a destroyed parent's
  children becoming roots is untested. The render list is rebuilt and fully
  re-uploaded every frame.
- ambientCG sets and tile sizes are requested in code, not material files.
