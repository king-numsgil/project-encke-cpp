#include "core/pch.hpp"

#include "physics/components.hpp"
#include "physics/physics_world.hpp"
#include "platform/worker_pool.hpp"
#include "render/config.hpp"
#include "terrain/planet.hpp"
#include "world/bodies.hpp"
#include "world/transform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace encke::physics
{
    namespace
    {
        constexpr i32 kSeed = 1337;

        struct Drop
        {
            f64vec3 ground{0.0};   // under the pole, as the collision LOD meshes it
            f64vec3 rest{0.0};
            u32     peak_chunks  = 0;
            u32     final_chunks = 0;
            bool    settled      = false;
        };

        // A half-metre box dropped from 60 m over the Earth's pole, the
        // planet at the origin, stepped a frame at a time until it has slept
        // for longer than chunks are kept.
        Drop drop_box()
        {
            auto const terrain = std::make_shared<terrain::BodyTerrain const>(terrain::example_planet(kSeed));

            entt::registry     registry;
            entt::entity const earth = registry.create();
            registry.emplace<Transform>(earth);
            registry.emplace<Body>(earth, Body{.radius = terrain->radius, .surface_gravity = 9.81});
            registry.emplace<terrain::PlanetTerrain>(earth, terrain::PlanetTerrain{.terrain = terrain});
            propagate_transforms(registry);

            terrain::GroundProbe const probe{*terrain, config::kCollisionLod};
            optional<f64vec3> const    ground =
                probe.below(f64vec3{0.0, terrain->radius + terrain::height_bound(*terrain) + 100.0, 0.0});
            REQUIRE(ground.has_value());

            // The world before the pool, so the pool's workers, whose jobs
            // use the world's samplers, stop first; as App orders them.
            PhysicsWorld physics;
            WorkerPool   pool;
            pool.start(3, "test");
            physics.init(0, config::kCollisionLod);

            entt::entity const box = registry.create();
            registry.emplace<Transform>(box, Transform{.position = *ground + f64vec3{0.0, 60.0, 0.0},
                                                       .scale    = f32vec3{0.5f}});
            physics.add_box(registry, box);

            Drop drop{.ground = *ground};

            // Until it has slept past the keep time, frames that wait on the
            // pool included; bounded by the wall clock, not frames.
            auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);   // takes seconds
            u32        asleep   = 0;
            while (asleep <= config::kCollisionKeepSteps + 10 && std::chrono::steady_clock::now() < deadline)
            {
                if (pool.drain() == 0 && pool.outstanding() > 0)
                {
                    std::this_thread::yield();
                }
                physics.update(registry, pool, 0.0, true);
                drop.peak_chunks = std::max(drop.peak_chunks, physics.collision_chunks());
                asleep           = physics.idle() ? asleep + 1 : 0;

            }

            drop.settled      = physics.idle();
            drop.rest         = registry.get<RigidBody>(box).position;
            drop.final_chunks = physics.collision_chunks();
            return drop;
        }
    }

    TEST_CASE("a character lands on the ground, walks along it and jumps", "[physics]")
    {
        auto const terrain = std::make_shared<terrain::BodyTerrain const>(terrain::example_planet(kSeed));

        entt::registry     registry;
        entt::entity const earth = registry.create();
        registry.emplace<Transform>(earth);
        registry.emplace<Body>(earth, Body{.radius = terrain->radius, .surface_gravity = 9.81});
        registry.emplace<terrain::PlanetTerrain>(earth, terrain::PlanetTerrain{.terrain = terrain});
        propagate_transforms(registry);

        terrain::GroundProbe const probe{*terrain, config::kCollisionLod};
        f64 const                  above = terrain->radius + terrain::height_bound(*terrain) + 100.0;
        optional<f64vec3> const    ground = probe.below(f64vec3{0.0, above, 0.0});
        REQUIRE(ground.has_value());

        // A wall across the way it walks, 15 m out, a metre thick and tall
        // enough to stand in the ground whatever its height there.
        constexpr f64      kWall = 15.0;
        entt::entity const wall  = registry.create();
        registry.emplace<Transform>(wall, Transform{.position = *ground + f64vec3{kWall, 0.0, 0.0},
                                                    .scale    = f32vec3{1.0f, 10.0f, 10.0f}});
        registry.emplace<StaticCollider>(wall);

        PhysicsWorld physics;
        WorkerPool   pool;
        pool.start(3, "test");
        physics.init(0, config::kCollisionLod);
        // Colliders stand at world transforms, which the wall has none of yet.
        propagate_transforms(registry);
        physics.add_static_colliders(registry);
        physics.add_character(registry, *ground + f64vec3{0.0, 2.0, 0.0});

        // Frames until `steps` more steps are taken, the pool drained
        // before each, so a frame waiting on ground takes none and the rest
        // one each.
        auto const run = [&](u64 steps) {
            u64 const until    = physics.steps() + steps;
            auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);   // takes seconds
            while (physics.steps() < until && std::chrono::steady_clock::now() < deadline)
            {
                while (pool.outstanding() > 0)
                {
                    if (pool.drain() == 0)
                    {
                        std::this_thread::yield();
                    }
                }
                physics.update(registry, pool, 0.0, true);
            }
            REQUIRE(physics.steps() >= until);
            return physics.character().value();
        };

        // Dropped from 2 m, it stands on the collision mesh, the probe's
        // surface to within the character's padding and the mesh's voxels.
        CharacterState const landed = run(120);
        CHECK(landed.on_ground);
        CHECK_FALSE(landed.held);
        CHECK(glm::length(landed.feet - *ground) < 0.1);
        CHECK(physics.idle());

        // Two seconds asked to walk along +x: at walking speed, less the
        // moment it takes to get going, over the pole's gentle ground.
        physics.set_character_input(CharacterInput{.move = f64vec3{config::kWalkSpeed, 0.0, 0.0}});
        CharacterState const walked = run(120);
        f64 const            along  = walked.feet.x - landed.feet.x;
        CAPTURE(along);
        CHECK(along > 0.8 * 2.0 * config::kWalkSpeed);
        CHECK(along < 2.0 * config::kWalkSpeed + 0.1);
        CHECK(walked.on_ground);

        // A jump from standing: up about v^2 / 2g, and down again.
        // Stopped, it stays put on the knoll's slope. Pulled by gravity
        // there, it crept a centimetre in two seconds.
        physics.set_character_input({});
        CharacterState const stood = run(60);
        CharacterState const later = run(120);
        CAPTURE(glm::length(later.feet - stood.feet));
        CHECK(glm::length(later.feet - stood.feet) < 1e-4);
        physics.set_character_input(CharacterInput{.jump = true});
        run(1);
        physics.set_character_input({});
        f64 highest = 0.0;
        for (int step = 0; step < 90; ++step)
        {
            highest = std::max(highest, glm::dot(run(1).feet - stood.feet, stood.up));
        }
        f64 const expected = config::kJumpSpeed * config::kJumpSpeed / (2.0 * 9.81);
        CAPTURE(highest, expected);
        CHECK(std::abs(highest - expected) < 0.15);
        CHECK(run(30).on_ground);

        // Walked on into the wall: it stops with its capsule against the
        // wall's near face, where it would have gone twice as far.
        physics.set_character_input(CharacterInput{.move = f64vec3{config::kWalkSpeed, 0.0, 0.0}});
        f64 const            face    = ground->x + kWall - 0.5;
        CharacterState const stopped = run(180);
        CAPTURE(stopped.feet.x, face);
        CHECK(stopped.feet.x < face - config::kCharacterRadius + 0.05);
        CHECK(stopped.feet.x > face - config::kCharacterRadius - 0.1);
    }

    TEST_CASE("a ship's space holds what is aboard, and lets go of what leaves", "[physics]")
    {
        auto const terrain = std::make_shared<terrain::BodyTerrain const>(terrain::example_planet(kSeed));

        entt::registry     registry;
        entt::entity const earth = registry.create();
        registry.emplace<Transform>(earth);
        registry.emplace<Body>(earth, Body{.radius = terrain->radius, .surface_gravity = 9.81});
        registry.emplace<terrain::PlanetTerrain>(earth, terrain::PlanetTerrain{.terrain = terrain});

        terrain::GroundProbe const probe{*terrain, config::kCollisionLod};
        optional<f64vec3> const    ground =
            probe.below(f64vec3{0.0, terrain->radius + terrain::height_bound(*terrain) + 100.0, 0.0});
        REQUIRE(ground.has_value());

        // A ship hanging 20 m over the pole, turned about the up so its
        // frame is not the world's: a floor 6 m square whose top is 1 m up
        // its +Y, only in its own space, and a volume 3 m in radius.
        f64quat const      turn = glm::angleAxis(0.7, f64vec3{0.0, 1.0, 0.0});
        entt::entity const ship = registry.create();
        registry.emplace<Transform>(ship, Transform{.position = *ground + f64vec3{0.0, 20.0, 0.0}, .rotation = turn});
        registry.emplace<ShipSpace>(ship, ShipSpace{.radius = 3.0, .bottom = 0.0, .top = 5.0});
        entt::entity const floor = registry.create();
        registry.emplace<Transform>(floor, Transform{.position = f64vec3{0.0, 0.5, 0.0},
                                                     .scale    = f32vec3{6.0f, 1.0f, 6.0f},
                                                     .parent   = ship});
        registry.emplace<StaticCollider>(floor, StaticCollider{.shape = StaticCollider::Shape::Box,
                                                               .mesh  = nullptr,
                                                               .space = ship,
                                                               .world = false});
        propagate_transforms(registry);
        WorldTransform const ship_world = registry.get<WorldTransform>(ship);
        auto const           in_ship    = [&](f64vec3 const& local) { return ship_world.position + turn * local; };

        PhysicsWorld physics;
        WorkerPool   pool;
        pool.start(3, "test");
        physics.init(0, config::kCollisionLod);
        physics.add_static_colliders(registry);
        CHECK(physics.ship_spaces() == 1);

        // One box dropped inside the volume, one beside it, outside.
        auto const box = [&](f64vec3 const& local) {
            entt::entity const entity = registry.create();
            registry.emplace<Transform>(entity, Transform{.position = in_ship(local), .rotation = turn,
                                                          .scale    = f32vec3{0.5f}});
            physics.add_box(registry, entity);
            return entity;
        };
        entt::entity const aboard = box(f64vec3{1.0, 3.0, 0.5});
        entt::entity const beside = box(f64vec3{5.0, 3.0, 0.0});

        auto const run = [&](u64 steps) {
            u64 const  until    = physics.steps() + steps;
            auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);   // takes seconds
            while (physics.steps() < until && std::chrono::steady_clock::now() < deadline)
            {
                while (pool.outstanding() > 0)
                {
                    if (pool.drain() == 0)
                    {
                        std::this_thread::yield();
                    }
                }
                physics.update(registry, pool, 0.0, true);
            }
            REQUIRE(physics.steps() >= until);
            propagate_transforms(registry);
        };
        run(240);

        // Aboard: in the ship's space, a child of the ship, resting on the
        // floor a half-box above its top, less Jolt's 2 cm penetration
        // slop, where the ship's frame says.
        RigidBody const& held = registry.get<RigidBody>(aboard);
        CHECK(held.space == ship);
        CHECK(registry.get<Transform>(aboard).parent == ship);
        CAPTURE(held.position.x, held.position.y, held.position.z);
        CHECK(std::abs(held.position.y - 1.25) < 0.03);
        CHECK(glm::length(registry.get<WorldTransform>(aboard).position - in_ship(held.position)) < 1e-6);

        // Beside: the floor is not in the world, so it fell to the ground.
        RigidBody const& fell = registry.get<RigidBody>(beside);
        CHECK(fell.space == entt::entity{entt::null});
        CHECK(fell.position.y - ground->y < 1.0);

        // A character on the floor is aboard; walked out along the ship's
        // +X, it leaves the volume, the floor with it, and lands below.
        physics.add_character(registry, in_ship(f64vec3{-1.0, 1.0, -1.0}));
        run(30);
        CHECK(physics.character_aboard());
        CHECK(std::abs(glm::dot(physics.character()->feet - in_ship(f64vec3{0.0, 1.0, 0.0}), turn * f64vec3{0.0, 1.0, 0.0})) < 0.05);

        physics.set_character_input(CharacterInput{.move = turn * f64vec3{config::kWalkSpeed, 0.0, 0.0}});
        run(180);
        physics.set_character_input({});
        run(180);
        CHECK_FALSE(physics.character_aboard());
        CharacterState const landed = physics.character().value();
        CHECK(landed.on_ground);
        CHECK(landed.feet.y - ground->y < 1.0);
    }

    TEST_CASE("a box dropped from high up lands, and the ground it fell past is freed", "[physics]")
    {
        Drop const first = drop_box();

        REQUIRE(first.settled);

        // On the ground: the centre of a 0.5 m box within a few centimetres
        // of a half-box above the surface the probe found, tilted on the
        // slope, and not wandered far off, though it hits the knoll at
        // 32 m/s and bounces. It went straight through before bodies were
        // swept (EMotionQuality::LinearCast).
        CHECK(std::abs((first.rest.y - first.ground.y) - 0.25) < 0.1);
        CHECK(std::abs(first.rest.x) < 3.0);
        CHECK(std::abs(first.rest.z) < 3.0);

        // Built on the way down and kept only around where it lies now.
        CHECK(first.final_chunks < first.peak_chunks);

        // The same drop again, to the bit: eviction removes bodies, which
        // Jolt's determinism counts like adding them.
        Drop const second = drop_box();
        CHECK(bit_cast<u64>(first.rest.x) == bit_cast<u64>(second.rest.x));
        CHECK(bit_cast<u64>(first.rest.y) == bit_cast<u64>(second.rest.y));
        CHECK(bit_cast<u64>(first.rest.z) == bit_cast<u64>(second.rest.z));
    }
}
