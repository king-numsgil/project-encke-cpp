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
