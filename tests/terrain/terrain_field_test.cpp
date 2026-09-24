#include "core/pch.hpp"

#include "terrain/terrain_field.hpp"

#include "perlin_reference.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>

namespace encke::terrain
{
    namespace
    {
        constexpr i32 kSeed = 1337;

        // Metres. The field is detail noise scaled by up to about 100 m, and
        // two samplers of one point differ by f32 rounding in the noise, on
        // the order of 1e-6 of that.
        constexpr f64 kMetres = 1e-3;

        size_t index(u32 axis, u32 i, u32 j, u32 k)
        {
            return (static_cast<size_t>(k) * axis + j) * axis + i;
        }

        // A chunk origin on the chunk grid of this LOD, near the surface
        // somewhere off every axis.
        f64vec3 surface_chunk(BodyTerrain const& terrain, u32 lod, f64vec3 const& direction)
        {
            f64 const     extent = 32.0 * terrain.voxel_size(lod);
            f64vec3 const point  = glm::normalize(direction) * terrain.radius;
            return glm::floor(point / extent) * extent;
        }

        struct Chunk
        {
            ChunkRequest request;
            vector<f32>  samples;

            f32 at(u32 i, u32 j, u32 k) const
            {
                return samples[index(request.samples_per_axis(), i, j, k)];
            }
        };

        Chunk sample(TerrainSampler& sampler, ChunkRequest const& request)
        {
            Chunk chunk{request, vector<f32>(request.sample_count())};
            sampler.sample_chunk(request, chunk.samples);
            return chunk;
        }
    }

    TEST_CASE("the chunk field matches the f64 reference at the surface", "[terrain]")
    {
        // Constant macro channels, so the field is exactly |p| - R - fBm and
        // the oracle can rebuild it.
        BodyTerrain terrain{.radius = 6'371'000.0, .seed = kSeed};
        terrain.macro.channels[static_cast<size_t>(MacroChannel::DetailAmplitude)].bias = 50.0f;
        terrain.macro.channels[static_cast<size_t>(MacroChannel::Persistence)].bias     = 0.5f;

        TerrainSampler sampler{terrain};
        f64vec3 const  origin = surface_chunk(terrain, 0, f64vec3{0.53, 0.71, -0.46});
        Chunk const    chunk  = sample(sampler, sampler.chunk(origin, 0));

        u32 const axis  = chunk.request.samples_per_axis();
        f64       worst = 0.0;
        for (u32 k = 0; k < axis; k += 3)
        {
            for (u32 j = 0; j < axis; j += 3)
            {
                for (u32 i = 0; i < axis; i += 3)
                {
                    f64vec3 const p = origin + (f64vec3{i, j, k} - 1.0) * chunk.request.voxel_size;

                    f64 fbm    = 0.0;
                    f64 weight = 1.0;
                    for (u32 octave = 0; octave < chunk.request.detail_octaves; ++octave)
                    {
                        fbm    += weight * reference::octave(sampler.octaves()[octave], p);
                        weight *= 0.5;
                    }
                    f64 const expected = glm::length(p) - terrain.radius - 50.0 * fbm;
                    worst = std::max(worst, std::abs(static_cast<f64>(chunk.at(i, j, k)) - expected));
                }
            }
        }
        CAPTURE(worst);
        CHECK(worst < kMetres);
    }

    TEST_CASE("neighbouring chunks agree on their shared face", "[terrain]")
    {
        TerrainSampler sampler{example_planet(kSeed)};

        for (u32 const lod : {0u, 4u, 9u})
        {
            f64vec3 const origin = surface_chunk(sampler.terrain(), lod, f64vec3{0.2, 1.0, 0.3});
            f64 const     extent = 32.0 * sampler.terrain().voxel_size(lod);

            for (glm::length_t axis_index = 0; axis_index < 3; ++axis_index)
            {
                f64vec3 step{0.0};
                step[axis_index] = extent;

                Chunk const a = sample(sampler, sampler.chunk(origin, lod));
                Chunk const b = sample(sampler, sampler.chunk(origin + step, lod));

                // a's samples 32 and 33 along the axis are b's 0 and 1.
                u32 const axis  = a.request.samples_per_axis();
                f64       worst = 0.0;
                for (u32 u = 0; u < axis; ++u)
                {
                    for (u32 v = 0; v < axis; ++v)
                    {
                        for (u32 layer = 0; layer < 2; ++layer)
                        {
                            // The stepped axis takes `along`; u and v run
                            // over the other two.
                            auto const at = [&](u32 along) {
                                u32vec3 cell{along, u, v};
                                if (axis_index == 1) { cell = u32vec3{u, along, v}; }
                                if (axis_index == 2) { cell = u32vec3{u, v, along}; }
                                return cell;
                            };
                            u32vec3 const in_a = at(32 + layer);
                            u32vec3 const in_b = at(layer);

                            f64 const diff = std::abs(static_cast<f64>(a.at(in_a.x, in_a.y, in_a.z)) -
                                                      static_cast<f64>(b.at(in_b.x, in_b.y, in_b.z)));
                            worst = std::max(worst, diff);
                        }
                    }
                }
                CAPTURE(lod, axis_index, worst);
                CHECK(worst < kMetres);
            }
        }
    }

    TEST_CASE("chunks at adjacent LODs agree at shared lattice points", "[terrain]")
    {
        TerrainSampler sampler{example_planet(kSeed)};

        // LOD 0/1 has the macro lattice at its own spacing; 8/9 has it at
        // the voxels, where each LOD's lattice differs.
        for (u32 const fine_lod : {0u, 8u})
        {
            u32 const    coarse_lod = fine_lod + 1;
            ChunkRequest coarse     = sampler.chunk(surface_chunk(sampler.terrain(), coarse_lod, f64vec3{0.2, 1.0, 0.3}), coarse_lod);

            // The fine chunk against the coarse one's +x face, sampling the
            // coarse chunk's octaves as a geomorphed boundary would.
            ChunkRequest fine = sampler.chunk(coarse.origin + f64vec3{32.0 * coarse.voxel_size, 0.0, 0.0}, fine_lod);
            fine.detail_octaves = coarse.detail_octaves;

            Chunk const a = sample(sampler, coarse);
            Chunk const b = sample(sampler, fine);

            // Coarse (33, j, k) and fine (1, 2j - 1, 2k - 1) are one point,
            // wherever the fine chunk reaches.
            f64 worst = 0.0;
            for (u32 j = 1; j <= 17; ++j)
            {
                for (u32 k = 1; k <= 17; ++k)
                {
                    f64 const diff = std::abs(static_cast<f64>(a.at(33, j, k)) -
                                              static_cast<f64>(b.at(1, 2 * j - 1, 2 * k - 1)));
                    worst = std::max(worst, diff);
                }
            }
            CAPTURE(fine_lod, worst);
            CHECK(worst < kMetres);

            // Left to its own octave count the fine chunk would carry more
            // detail than the coarse one there, which is the geomorph's to
            // hide, so the override above is doing something.
            ChunkRequest own = sampler.chunk(fine.origin, fine_lod);
            REQUIRE(own.detail_octaves > coarse.detail_octaves);
        }
    }

    TEST_CASE("point queries agree with the chunk path", "[terrain]")
    {
        TerrainSampler sampler{example_planet(kSeed)};

        std::mt19937_64                    random{7};
        std::uniform_real_distribution<f64> direction{-1.0, 1.0};
        std::uniform_int_distribution<u32>  interior{1, 32};

        for (u32 const lod : {0u, 2u, 4u, 9u})
        {
            f64 worst_value    = 0.0;
            f64 worst_gradient = 0.0;
            for (int trial = 0; trial < 6; ++trial)
            {
                f64vec3 const facing{direction(random), direction(random), direction(random)};
                Chunk const   chunk = sample(sampler, sampler.chunk(surface_chunk(sampler.terrain(), lod, facing), lod));
                f64 const     voxel = chunk.request.voxel_size;

                for (int point = 0; point < 16; ++point)
                {
                    u32 const i = interior(random);
                    u32 const j = interior(random);
                    u32 const k = interior(random);

                    f64vec3 const     p      = chunk.request.origin + (f64vec3{i, j, k} - 1.0) * voxel;
                    PointSample const sample = sampler.sample_point(p, voxel, chunk.request.detail_octaves);

                    f64vec3 const gradient = f64vec3{
                        chunk.at(i + 1, j, k) - chunk.at(i - 1, j, k),
                        chunk.at(i, j + 1, k) - chunk.at(i, j - 1, k),
                        chunk.at(i, j, k + 1) - chunk.at(i, j, k - 1),
                    } / (2.0 * voxel);

                    worst_value    = std::max(worst_value, std::abs(static_cast<f64>(sample.value - chunk.at(i, j, k))));
                    // A difference of values over two voxels.
                    worst_gradient = std::max(worst_gradient, glm::length(f64vec3{sample.gradient} - gradient) * 2.0 * voxel);
                }
            }
            CAPTURE(lod, worst_value, worst_gradient);
            CHECK(worst_value < kMetres);
            CHECK(worst_gradient < 2.0 * kMetres);
        }
    }
}
