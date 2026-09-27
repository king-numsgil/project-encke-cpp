#pragma once

#include <memory>

namespace encke
{
    class WorkerPool;
}

namespace encke::physics
{
    // Every rigid body in the scene, simulated by Jolt at a fixed 60 steps a
    // second, in f64 world positions (the library is built double-precision).
    // Gravity is each body's own: toward the centre of the nearest Body with
    // a surface gravity, falling off with the square of the distance.
    //
    // The ground is the terrain's, as collision meshes of chunks at one LOD,
    // config::kCollisionLod by default, built on the worker pool around each moving body
    // and kept. A body is held out of the simulation until the chunks around
    // it are built, and while any chunk a moving body needs is still being
    // built the world does not step at all: nothing falls through ground
    // that is on its way, and the simulation does not depend on how long the
    // pool took.
    //
    // Jolt's jobs run on its own thread pool, one thread per logical
    // processor the physical cores leave over (the hyperthreads beside the
    // terrain workers), plus the main thread, which works while it waits for
    // a step.
    //
    // Main thread only; chunk completions run in the pool's drain().
    // What a walking character is asked to do over the next steps.
    struct CharacterInput
    {
        // The velocity wanted along the ground, world space, m/s; only its
        // part across the character's up counts.
        f64vec3 move{0.0};
        // Held: jumps whenever it stands on walkable ground.
        bool jump = false;
    };

    struct CharacterState
    {
        f64vec3 feet{0.0};       // between the last two steps, like a body's pose
        f64vec3 up{0.0, 1.0, 0.0};
        f64vec3 velocity{0.0};
        bool    on_ground = false;
        // Waiting for the ground around it to be built; it does not move.
        bool held = true;
    };

    class PhysicsWorld
    {
    public:
        PhysicsWorld();
        ~PhysicsWorld();

        PhysicsWorld(PhysicsWorld const&)            = delete;
        PhysicsWorld& operator=(PhysicsWorld const&) = delete;

        // Sets up Jolt with `threads` workers of its own; 0 steps on the
        // calling thread alone. Bodies collide with the terrain at
        // `collision_lod`.
        void init(u32 threads, u32 collision_lod);

        // A dynamic box on `entity`, which must be a root with a Transform:
        // its size is the Transform's scale, its pose the Transform's. Held
        // until the ground around it is built.
        void add_box(entt::registry& registry, entt::entity entity);

        // A dynamic sphere, its diameter the Transform's scale on x.
        void add_sphere(entt::registry& registry, entt::entity entity);

        // A static body for every StaticCollider in the registry, in the
        // registry's order, which is the same on every run. They never
        // move; built once, after the scene.
        void add_static_colliders(entt::registry const& registry);

        // Builds the ground bodies need and steps. With `fixed_frame`, one
        // step exactly, whatever `seconds` says, so a scenario steps the same
        // however fast frames come; otherwise as many steps as `seconds` of
        // wall clock hold, at most a few, and the Transforms are interpolated
        // between the last two. Before Scene::update, which composes the
        // Transforms it writes.
        void update(entt::registry& registry, WorkerPool& pool, f64 seconds, bool fixed_frame);

        // One walking character, standing with its feet at `feet`: a
        // capsule of config::kCharacterHeight moved by Jolt's
        // CharacterVirtual, kinematic, which walks slopes up to
        // kMaxSlopeDegrees, climbs steps and keeps to the ground going down,
        // and pushes bodies. Bodies bump into it through a kinematic capsule
        // inside it. Its up is away from the nearest pulling body, as
        // gravity is. Held, like a body, until the ground around it is
        // built. Replaces the one there was.
        void add_character(entt::registry const& registry, f64vec3 const& feet);
        void remove_character();
        bool has_character() const;

        // Moves it there at once, still, as a camera step does.
        void teleport_character(f64vec3 const& feet);

        // What the steps from now on are asked, until set again.
        void set_character_input(CharacterInput const& input);

        optional<CharacterState> character() const;

        // Away from the nearest pulling body at `point`: the up a character
        // there stands to.
        f64vec3 up_at(entt::registry const& registry, f64vec3 const& point) const;

        // Nothing held, no ground being built, every body asleep, and the
        // character, if there is one, standing still.
        bool idle() const;

        u32 bodies() const;
        u32 awake() const;

        // Steps taken since init; frames waiting on ground take none.
        u64 steps() const;

        // Collision chunks built or building, surface or not.
        u32 collision_chunks() const;

        // What a collision shape belongs to, for the debug view.
        enum class ShapeKind
        {
            Ground,   // a terrain collision chunk
            Static,   // a StaticCollider: the scene's props
            Held,     // a body waiting for its ground
            Awake,
            Asleep,
        };

        // Every body's collision shape as world-space triangles, three
        // points each, one call per body. Shapes come from Jolt's own
        // triangulation, so a sphere is a faceted one, and a box shows its
        // faces' diagonals.
        void collision_triangles(function<void(ShapeKind kind, span<f64vec3 const> triangles)> const& visit) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
