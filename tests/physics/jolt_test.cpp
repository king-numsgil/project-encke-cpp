#include "core/pch.hpp"

// Jolt.h must come first: it sets up the platform and configuration macros
// every other Jolt header reads.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <catch2/catch_test_macros.hpp>

// Jolt is built by the overlay port in ports/joltphysics with double-precision
// positions and cross-platform determinism. These tests prove the library
// links, that encke sees the same configuration it was built with, and that
// a body resting a planet's radius from the origin behaves.
namespace encke::physics
{
    namespace
    {
        // The Earth's radius: the test planet's pole sits this far from the
        // body's centre, and a physics bubble there sees positions this big.
        constexpr f64 kFar = 6'371'000.0;

        constexpr JPH::ObjectLayer kStatic = 0;
        constexpr JPH::ObjectLayer kMoving = 1;

        struct JoltRuntime
        {
            JoltRuntime()
            {
                JPH::RegisterDefaultAllocator();
                JPH::Factory::sInstance = new JPH::Factory();
                JPH::RegisterTypes();
            }

            ~JoltRuntime()
            {
                JPH::UnregisterTypes();
                delete JPH::Factory::sInstance;
                JPH::Factory::sInstance = nullptr;
            }

            JoltRuntime(JoltRuntime const&)            = delete;
            JoltRuntime& operator=(JoltRuntime const&) = delete;
        };

        struct Drop
        {
            JPH::RVec3 rest;
            bool       asleep = false;
        };

        // A 0.5 m sphere dropped from 2 m onto a floor whose top face is at
        // kFar on Y, stepped at 60 Hz for five seconds.
        Drop drop_sphere()
        {
            JPH::ObjectLayerPairFilterTable object_filter{2};
            object_filter.EnableCollision(kStatic, kMoving);
            object_filter.EnableCollision(kMoving, kMoving);

            JPH::BroadPhaseLayerInterfaceTable broad_phase{2, 2};
            broad_phase.MapObjectToBroadPhaseLayer(kStatic, JPH::BroadPhaseLayer{0});
            broad_phase.MapObjectToBroadPhaseLayer(kMoving, JPH::BroadPhaseLayer{1});

            JPH::ObjectVsBroadPhaseLayerFilterTable broad_filter{broad_phase, 2, object_filter, 2};

            JPH::PhysicsSystem system;
            system.Init(64, 0, 64, 64, broad_phase, broad_filter, object_filter);

            JPH::BodyInterface& bodies = system.GetBodyInterface();

            JPH::BodyCreationSettings floor{new JPH::BoxShape(JPH::Vec3(50.0f, 1.0f, 50.0f)),
                                            JPH::RVec3(0.0, kFar - 1.0, 0.0), JPH::Quat::sIdentity(),
                                            JPH::EMotionType::Static, kStatic};
            bodies.CreateAndAddBody(floor, JPH::EActivation::DontActivate);

            JPH::BodyCreationSettings ball{new JPH::SphereShape(0.5f),
                                           JPH::RVec3(0.25, kFar + 2.0, -0.125), JPH::Quat::sIdentity(),
                                           JPH::EMotionType::Dynamic, kMoving};
            JPH::BodyID const id = bodies.CreateAndAddBody(ball, JPH::EActivation::Activate);

            JPH::TempAllocatorImpl      temp{1 << 20};
            JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};

            for (int step = 0; step < 300; ++step)
            {
                system.Update(1.0f / 60.0f, 1, &temp, &jobs);
            }

            Drop result{bodies.GetCenterOfMassPosition(id), !bodies.IsActive(id)};
            bodies.RemoveBody(id);
            bodies.DestroyBody(id);
            return result;
        }
    }

    TEST_CASE("Jolt is built double-precision and deterministic, as encke is compiled against it", "[physics]")
    {
        JoltRuntime const runtime;

        CHECK(JPH::VerifyJoltVersionID());
        STATIC_CHECK(std::is_same_v<JPH::Real, double>);
#ifndef JPH_CROSS_PLATFORM_DETERMINISTIC
        FAIL("JPH_CROSS_PLATFORM_DETERMINISTIC is not defined");
#endif
    }

    TEST_CASE("A sphere dropped a planet's radius from the origin comes to rest on the floor", "[physics]")
    {
        JoltRuntime const runtime;

        Drop const first = drop_sphere();

        // Resting on the floor: its centre a radius above the top face, to
        // within Jolt's penetration slop, and not wandered off sideways.
        CHECK(first.asleep);
        CHECK(std::abs(first.rest.GetY() - (kFar + 0.5)) < 0.02);
        CHECK(std::abs(first.rest.GetX() - 0.25) < 1e-3);
        CHECK(std::abs(first.rest.GetZ() + 0.125) < 1e-3);

        // The same inputs give the same bits.
        Drop const second = drop_sphere();
        CHECK(bit_cast<u64>(first.rest.GetX()) == bit_cast<u64>(second.rest.GetX()));
        CHECK(bit_cast<u64>(first.rest.GetY()) == bit_cast<u64>(second.rest.GetY()));
        CHECK(bit_cast<u64>(first.rest.GetZ()) == bit_cast<u64>(second.rest.GetZ()));
    }
}
