#include "core/pch.hpp"

#include "terrain/planet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

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

        for (f64 const factor : {1.5, 0.3})
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
            if (factor == 1.5)
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
}
