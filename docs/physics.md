# Physics and movement

`physics::PhysicsWorld` runs Jolt, built double precision and deterministic
(see `docs/dependencies.md`): a fixed `config::kPhysicsHz` (60) steps a
second, positions in f64 world space end to end. An entity with a
`RigidBody` is a root, or a ship's child while aboard, whose `Transform` the
world writes after every step, interpolated between the last two steps by
how far the wall clock is between them. `update` runs after the pool's drain
and before `Scene::update`.

## Stepping

- Live, wall-clock time goes into an accumulator and Jolt always takes fixed
  1/60 s steps, at most `kPhysicsMaxSteps` a frame; a variable step would
  break determinism and the solver's stability. Under a scenario it is one
  step a frame, whatever the frame took, so frame N is the same simulated
  instant on every run.
- Gravity is each body's own. The system's gravity is zero; before every
  step each awake body gets a force toward the centre of the `Body` whose
  surface is nearest, `Body::surface_gravity` at the radius and inverse
  square beyond. Nothing assumes a world up.
- Bodies are swept (`EMotionQuality::LinearCast`). Tested only where each
  step ends, a half-metre box dropped from 60 m passed a one-sided terrain
  triangle between two steps.
- Threads: Jolt's own `JobSystemThreadPool`, `logical - physical` threads,
  named `physicsN`, plus the main thread, which runs jobs while it waits.
  They are the hyperthreads beside the terrain workers; Jolt's step is
  pointer chasing and branches, which shares a core with the terrain's SIMD
  better than more SIMD would. With no SMT the main thread steps alone.
- Jolt allocates through mimalloc when it is on. The hooks are set before
  anything of Jolt's exists, broad-phase tables included, which is why
  `init` builds its `Impl` after setting them.

## Determinism

Jolt is deterministic only given the same calls in the same order, body
creation and removal included. Collision chunks come back from the pool in
whatever order they finish, so a built chunk becomes a body only once every
chunk asked for is back, and then all in grid order (`Impl::add_built`);
eviction is in grid order too. Everything else is created in registry order.
`boxes.json` twice is byte-identical with Jolt on four threads; the first
run without `add_built` differed. `physics_world_test` drops a box twice and
compares to the bit.

## The ground

The terrain at `config::kCollisionLod` (1: 0.5 m voxels, 16 m chunks):
collision chunks meshed on the worker pool by the same sampler and Surface
Nets as the drawn chunks, without morph targets or skirts, as static
`MeshShape` bodies. They are built around each held or awake body, its bound
plus `kCollisionMargin`, independent of the camera, as a server's would be.
LOD 2 visibly buried crates' edges in the drawn ground; LOD 1 and 0 did not.
The `collision_lod` scenario setting overrides it.

- A new body is held out of the simulation until the chunks around it are
  built, and while any chunk a moving body needs is still building, no space
  steps at all. So nothing falls through ground on its way, and the
  simulation does not depend on how long the pool took.
- Every body, asleep or not, keeps the chunks within its bound plus twice the
  margin; a chunk nobody has kept for `kCollisionKeepSteps` (five seconds) is
  removed.

## Static colliders

- `Scene::add` tags every cube and sphere it places with a `StaticCollider`
  (`physics/components`, Jolt-free). The Moon and the spinner are untagged;
  the helmet has none. Static props carry `kStaticProp` as user data, which
  tells them from ground chunks in the collision view.
- A glTF model collides if its `ModelSpawn` asks: `Collision::Mesh` gives
  each part a static `MeshShape` of its own triangles, kept on the CPU
  (`ModelPart::collision`) after the vertices go to the GPU;
  `Collision::Hulls` gives each part the convex hull of its vertices (the
  torchship's seats). As meshes, nodes with a `joint` extra
  (`ModelNode::moving`: doors, hatches, view domes, the ladder) are left out
  with their subtrees, and so are nodes named by `no_collision`.
- Colliders sit on child entities, so `add_static_colliders` reads
  `WorldTransform`. It runs after startup's `propagate_transforms` and again
  after every `Scene::update`, making bodies for each `StaticCollider`
  without a `StaticBody` yet. Shapes are cached by mesh and scale.

## Ships: a physics space aboard

An entity with a `ShipSpace` gets a second Jolt `PhysicsSystem`, fixed to its
frame, for what is aboard. The world's space is the first of
`Impl::spaces`; all step one after another each step, sharing the job system
and temp allocator. Aboard, positions are small numbers whatever the ship
does.

- A ship's gravity is one vector in its frame: the world's at the ship less
  the ship's acceleration, set on its system before each step. Landed, it is
  the planet's; under thrust or in free fall it follows with no other code.
- Its shapes come from its model. Nodes with a `physics` extra become
  `ShipShape`s (`ModelSpawn::shapes_ship` names the ship). `VOL_` nodes are
  its volume, each a convex piece kept as planes in the ship's frame. The
  rest are its hull, each a convex hull or box, which `take_shapes` makes
  into one `StaticCompoundShape` and a dynamic body on the ship's root of
  `ShipSpace::mass` (270 t for the torchship, wet), inertia scaled to it.
- With a body, the ship's frame follows it step by step (`follow_ships`):
  pose, velocity, spin, and acceleration from the change in velocity. A ship
  without hull pieces holds still where its Transform is, and its frame is
  read from that across updates (`Impl::sync`), as the unit test's is. The
  interior is parented to the ship's root, so it goes where the body goes.
- Crossing is by sphere against the volume (`Impl::transfer`, before every
  step). A body, by its bound, or the character, by its capsule's middle and
  radius, enters a ship as soon as it reaches into the volume, and leaves once
  clear of it by `kShipSpaceHysteresis`. It is made again in the other space
  with its world pose and velocity, its frame's velocity and spin taken off
  or added, and its Transform reparented to the ship or back to the world.
  The ship itself never crosses; terrain is built only for the world's space.
- The torchship's door volumes stand out past its closed leaves, so a body
  pressed against a closed door from outside reaches the door's volume before
  the solid hull stops it. A capsule held off the door by its radius stops
  short of the volume with its centre, which is why crossing is by sphere.
  The door volumes are the portals.
- A `StaticCollider` names the space it is fixed in and whether the world
  has it too. The torchship's decks and seats are in the ship's alone; its
  render hull collides nowhere, the `COL_` compound standing in for it.
- `physics_world_test` hangs a turned ship over the pole with a floor only in
  its space, drops one box inside the volume and one beside it, and walks a
  character off the floor and out. `scenarios/torchship.json` drops crates on
  D2 and walks out the cargo door, byte-identical across runs.

## The walking character

A capsule of `config::kCharacterHeight` standing on its feet, Jolt's
`CharacterVirtual` in `PhysicsWorld` (`add_character`), in whichever space it
stands; `render/walk_camera` puts the camera at `kEyeHeight` above its feet.
Every tuning number is in `render/config.hpp`.

- Kinematic, stepped once per physics step before Jolt's. `step_character`
  follows Jolt's CharacterVirtual sample with the up generalised: on walkable
  ground it takes the ground's velocity plus its own, easing toward what is
  asked at `kGroundResponse`, and jumps at `kJumpSpeed`; in the air it keeps
  its fall and eases across at `kAirResponse`. `ExtendedUpdate` climbs
  `kStepUp` and keeps to the ground `kStepDown` going down.
- No gravity while standing on walkable ground. With it, a stopped character
  slid down any slope; with only its part along the normal it still crept a
  centimetre in two seconds. Without it, it holds to a tenth of a millimetre.
- Its up is gravity's, per step, set as Jolt's up and the capsule's rotation.
  The walk camera carries its heading onto each new up by the least turn, so
  it stays level walking round a planet.
- Its ground is built like a body's, and it is held until that is built.
- Bodies bump into it through a kinematic inner capsule (`mInnerBodyShape`),
  and it pushes them by its `kCharacterMass`. The inner body is left out of
  the gravity loop (Jolt asserts on a kinematic body's inverse mass) and of
  the awake count; `idle()` asks instead that the character stand still on
  the ground, asked nothing.
- A `camera` step, walking, teleports the character under the new eye. The
  `walk` step counts physics steps, not frames: a frame waiting on ground
  takes no step, and counting frames made a long walk end elsewhere when the
  pool ran slower.
- `physics_world_test` drops it 2 m onto the pole, walks it, stops it on a
  slope, jumps it to v²/2g within 15 cm, and walks it into a static wall.

## The collision view

Shapes come from Jolt's own triangulation, relative to each body's centre of
mass in f32 and placed in f64, and a ship's space is carried into the world.
Only leaf shapes triangulate, so a compound is taken apart first
(`CollectTransformedShapes`); the character's offset inner capsule, a
`RotatedTranslatedShape`, hit a trap the first time the view was on while
walking.

## Camera control

X toggles between on foot, the default outside scenarios, and the jetpack. A
scenario starts on the jetpack and walks with `"movement": "walk"`.

The mouse is held in both movements (`App::capture_mouse`): relative mouse
mode, and the UI gets `ImGuiConfigFlags_NoMouse | NoKeyboard`. Alt, or losing
focus, lets go of it. Space is safe only because the UI is blocked while the
mouse is held: ImGui's keyboard navigation activates the focused widget on
Space. Under a scenario it is never held.

The jetpack (`render/fly_camera`) flies the camera entity's local
`Transform`, so a camera parented to a ship flies relative to it. Mouse yaws
and pitches about the camera's own axes, Q/E roll, WASD along the view,
Space/Ctrl along the camera's up, Shift 5x, C 0.2x, and the wheel scales the
base speed by 1.25 a notch between 2 m/s and 10,000 km/s.

- The time step is wall clock from one fixed point in the loop to the same
  point next iteration, clamped to 0.1 s. Measured from where the previous
  `draw()` returned, it left out the fence wait and present, and movement
  was slow and uneven.
- The camera is flown before `Scene::update`, which composes its world
  transform; flown after, it would draw a frame late.
- The camera has no up but its own; nothing in `FlyCamera` reads world +Y or
  a planet. The environment's up is the nearest body's (`surroundings_at`),
  switching, not blending, halfway between two.
- A scenario's `camera` step takes an optional `up`, defaulting to the test
  scene's +Y, the floor's up.

## Known gaps

- The props are static: the spinner and anything moved later keep no
  collider, and a model's moving parts have none in any pose.
- The ship's decks are joined by ladders and the character cannot climb. The
  floor hatches are modelled open and have no colliders, so the deck centres
  are holes down the ladder shaft.
- Ship spaces: spin enters only the velocity carried across the boundary,
  not the gravity aboard (no centrifugal or Coriolis term); nothing aboard
  pushes back on the ship's body; its door and turret boxes are fixed in the
  compound, doors shut; its centre of mass is the compound's at uniform
  density, where full tanks and the drive would put it lower; a ship whose
  entity is destroyed keeps its space. Nothing drives the ship yet.
- The ship's inside is drawn from outside; the hull does not hide it from
  the culler.
- Near the torchship the collision view passes `kMaxDebugLines` and drops
  shapes.
- LOD 1 collision lacks the octaves under 1 m that the drawn LOD 0 ground
  has; a resting body can sink or hover by the centimetres they add.
- Every dynamic body is swept, fast or not. Cheap for dozens; for thousands,
  sweep only the fast or the small.
- A body needing new ground halts every space while it builds, which shows
  as a hitch in a live run.
- Switching to the jetpack removes the character rather than leaving it
  standing.
- No stats-window row for the physics threads, and no step timing.
