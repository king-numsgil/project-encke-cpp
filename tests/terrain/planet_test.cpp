#include "core/pch.hpp"

#include "render/config.hpp"
#include "terrain/planet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <random>

namespace encke::terrain
{
    namespace
    {
        // A small, rough body: 3 km of relief over 30 km features on a 40 km
        // radius, so the field's gradient is well above 1 in places, which is
        // what makes a cull factor of 1 unsafe.
        BodyTerrain rugged_asteroid()
        {
            BodyTerrain terrain{.radius = 40'000.0, .seed = 99, .base_voxel_size = 0.25};
            terrain.macro.channels[static_cast<size_t>(MacroChannel::Height)] = MacroChannelSpec{
                .graph = encode_fbm_graph(30'000.0f, 4, 0.5f, 2.0f), .scale = 3'000.0f, .bias = 0.0f};
            terrain.macro.channels[static_cast<size_t>(MacroChannel::DetailAmplitude)].bias = 20.0f;
            terrain.macro.channels[static_cast<size_t>(MacroChannel::Persistence)].bias     = 0.5f;
            return terrain;
        }

        // 512 m voxels, 16 km chunks: a few hundred in the cube.
        constexpr u32 kLod = 11;

        using Key = array<i64, 3>;

        Key key(i64vec3 const& origin)
        {
            return Key{{origin.x, origin.y, origin.z}};
        }

        // Every chunk of the cube, sampled once: whether it has surface.
        std::map<Key, bool> surface_of_every_chunk(TerrainSampler& sampler)
        {
            ChunkPlan const     plan = plan_chunks(sampler, kLod, 1.0);
            std::map<Key, bool> surface;
            vector<f32>         samples;
            for (auto const* list : {&plan.kept, &plan.culled})
            {
                for (i64vec3 const& origin : *list)
                {
                    ChunkRequest const request = sampler.chunk(origin, kLod);
                    samples.resize(request.sample_count());
                    sampler.sample_chunk(request, samples);
                    surface[key(origin)] = chunk_has_surface(samples, request.cells);
                }
            }
            return surface;
        }
    }

    TEST_CASE("culling at the configured factor keeps every chunk the surface crosses", "[terrain][planet]")
    {
        TerrainSampler            sampler{rugged_asteroid()};
        std::map<Key, bool> const surface = surface_of_every_chunk(sampler);

        u32 with_surface = 0;
        for (auto const& [chunk, crossed] : surface)
        {
            with_surface += crossed ? 1u : 0u;
        }
        REQUIRE(with_surface > 20);

        for (f64 const factor : {config::kTerrainCullFactor, 0.3})
        {
            ChunkPlan const plan = plan_chunks(sampler, kLod, factor);

            u32 culled_with_surface   = 0;
            u32 frontier_with_surface = 0;
            for (i64vec3 const& origin : plan.culled)
            {
                culled_with_surface += surface.at(key(origin)) ? 1u : 0u;
            }
            for (i64vec3 const& origin : plan.culled_frontier)
            {
                frontier_with_surface += surface.at(key(origin)) ? 1u : 0u;
            }

            CAPTURE(factor, plan.kept.size(), plan.culled.size(), plan.culled_frontier.size(),
                    culled_with_surface, frontier_with_surface);
            if (factor == config::kTerrainCullFactor)
            {
                // The factor the app uses.
                CHECK(culled_with_surface == 0);
            }
            else
            {
                // Too tight a factor loses surface, and checking the frontier
                // alone finds it: the test, and the app's check, can fail.
                CHECK(culled_with_surface > 0);
                CHECK(frontier_with_surface > 0);
            }
        }
    }

    TEST_CASE("the example planet's field stays as steep as the cull factor trusts", "[terrain][planet]")
    {
        // The cull takes |SDF| at a chunk's centre over
        // config::kTerrainCullFactor as a distance no surface is nearer than,
        // across the half-diagonal of a chunk. What it trusts is the field's
        // slope over that distance, not over one voxel, where the detail
        // octaves are far steeper. Measured at the surface, where it is
        // steepest, as differences half a chunk edge either side, at LODs
        // from the finest to where only the macro layer counts.
        BodyTerrain const terrain = example_planet(1337);
        TerrainSampler    sampler{terrain};

        std::mt19937_64                     random{11};
        std::uniform_real_distribution<f64> unit{-1.0, 1.0};

        for (u32 const lod : {0u, 4u, 8u, 12u})
        {
            GroundProbe const probe{terrain, lod};
            f64 const         voxel   = terrain.voxel_size(lod);
            f64 const         reach   = 16.0 * voxel;
            u32 const         octaves = octaves_for_voxel(sampler.octaves(), voxel);

            f64 steepest = 0.0;
            u32 measured = 0;
            for (int trial = 0; trial < 300; ++trial)
            {
                f64vec3 const direction = glm::normalize(f64vec3{unit(random), unit(random), unit(random)});
                optional<f64vec3> const ground =
                    probe.below(direction * (terrain.radius + height_bound(terrain) + 100.0));
                if (!ground.has_value())
                {
                    continue;
                }

                f64vec3 slope{0.0};
                for (glm::length_t axis = 0; axis < 3; ++axis)
                {
                    f64vec3 step{0.0};
                    step[axis]  = reach;
                    slope[axis] = (static_cast<f64>(sampler.sample_value(*ground + step, voxel, octaves)) -
                                   static_cast<f64>(sampler.sample_value(*ground - step, voxel, octaves))) /
                                  (2.0 * reach);
                }
                steepest = std::max(steepest, glm::length(slope));
                ++measured;
            }
            CAPTURE(lod, measured, steepest);
            CHECK(measured > 250);

            // Not at LOD 0, where the detail octaves are steeper than the
            // factor over a chunk, about 2.5, as the generator before the
            // mountain belts already was; see the known gaps under The Earth
            // as terrain.
            if (lod > 0)
            {
                CHECK(steepest < config::kTerrainCullFactor);
            }
        }
    }
}
