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

        // Metres, for comparisons that are not bit-exact: the oracle, and
        // point queries, which split about the point rather than the grid.
        // The field is detail noise scaled by up to about 100 m; they differ
        // by f32 rounding in the noise and the last bit of the output.
        constexpr f64 kMetres = 1e-3;

        // A chunk origin on this LOD's chunk grid, near the surface in a
        // direction off every axis.
        i64vec3 surface_chunk(BodyTerrain const& terrain, u32 lod, f64vec3 const& direction)
        {
            f64 const     extent = static_cast<f64>(i64{32} << lod);
            f64vec3 const cell   = glm::normalize(direction) * (terrain.radius / terrain.base_voxel_size);
            return i64vec3{glm::floor(cell / extent) * extent};
        }

        f64vec3 position(BodyTerrain const& terrain, i64vec3 const& grid)
        {
            return f64vec3{grid} * terrain.base_voxel_size;
        }

        struct Chunk
        {
            ChunkRequest    request;
            vector<f32>     samples;
            vector<f32>     coarse_values;
            vector<f32vec3> coarse_gradients;

            f32 at(u32 i, u32 j, u32 k) const
            {
                u32 const axis = request.samples_per_axis();
                return samples[(static_cast<size_t>(k) * axis + j) * axis + i];
            }

            size_t coarse_index(u32 i, u32 j, u32 k) const
            {
                u32 const axis = request.coarse_per_axis();
                return (static_cast<size_t>(k) * axis + j) * axis + i;
            }

            // The grid point of sample (i, j, k).
            i64vec3 grid(u32 i, u32 j, u32 k) const
            {
                return request.origin + (i64vec3{i, j, k} - i64vec3{2}) * (i64{1} << request.lod);
            }
        };

        Chunk sample(TerrainSampler& sampler, ChunkRequest const& request, bool coarse = false)
        {
            Chunk chunk{request, vector<f32>(request.sample_count()), {}, {}};
            if (coarse)
            {
                chunk.coarse_values.resize(request.coarse_count());
                chunk.coarse_gradients.resize(request.coarse_count());
                CoarseSamples const output{chunk.coarse_values, chunk.coarse_gradients};
                sampler.sample_chunk(request, chunk.samples, &output);
            }
            else
            {
                sampler.sample_chunk(request, chunk.samples);
            }
            return chunk;
        }

        bool same_bits(f32 a, f32 b)
        {
            return bit_cast<u32>(a) == bit_cast<u32>(b);
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
        Chunk const    chunk = sample(sampler, sampler.chunk(surface_chunk(terrain, 0, f64vec3{0.53, 0.71, -0.46}), 0));

        u32 const axis  = chunk.request.samples_per_axis();
        f64       worst = 0.0;
        for (u32 k = 0; k < axis; k += 3)
        {
            for (u32 j = 0; j < axis; j += 3)
            {
                for (u32 i = 0; i < axis; i += 3)
                {
                    f64vec3 const p = position(terrain, chunk.grid(i, j, k));

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

    TEST_CASE("neighbouring chunks agree bit for bit on their shared face", "[terrain]")
    {
        TerrainSampler sampler{example_planet(kSeed)};

        for (u32 const lod : {0u, 4u, 9u})
        {
            i64vec3 const origin = surface_chunk(sampler.terrain(), lod, f64vec3{0.2, 1.0, 0.3});
            i64 const     extent = i64{32} << lod;

            for (glm::length_t axis_index = 0; axis_index < 3; ++axis_index)
            {
                i64vec3 step{0};
                step[axis_index] = extent;

                Chunk const a = sample(sampler, sampler.chunk(origin, lod));
                Chunk const b = sample(sampler, sampler.chunk(origin + step, lod));

                // a's samples 32 to 35 along the axis are b's 0 to 3.
                u32 const axis       = a.request.samples_per_axis();
                u32       mismatches = 0;
                for (u32 u = 0; u < axis; ++u)
                {
                    for (u32 v = 0; v < axis; ++v)
                    {
                        for (u32 layer = 0; layer < 4; ++layer)
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

                            REQUIRE(a.grid(in_a.x, in_a.y, in_a.z) == b.grid(in_b.x, in_b.y, in_b.z));
                            if (!same_bits(a.at(in_a.x, in_a.y, in_a.z), b.at(in_b.x, in_b.y, in_b.z)))
                            {
                                ++mismatches;
                            }
                        }
                    }
                }
                CAPTURE(lod, axis_index);
                CHECK(mismatches == 0);
            }
        }
    }

    TEST_CASE("chunks at adjacent LODs agree bit for bit at shared lattice points", "[terrain]")
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
            ChunkRequest fine = sampler.chunk(coarse.origin + i64vec3{i64{32} << coarse_lod, 0, 0}, fine_lod);
            fine.detail_octaves = coarse.detail_octaves;

            Chunk const a = sample(sampler, coarse);
            Chunk const b = sample(sampler, fine);

            // Coarse (34, j, k) and fine (2, 2j - 2, 2k - 2) are one point,
            // wherever the fine chunk reaches.
            u32 mismatches = 0;
            for (u32 j = 1; j <= 18; ++j)
            {
                for (u32 k = 1; k <= 18; ++k)
                {
                    REQUIRE(a.grid(34, j, k) == b.grid(2, 2 * j - 2, 2 * k - 2));
                    if (!same_bits(a.at(34, j, k), b.at(2, 2 * j - 2, 2 * k - 2)))
                    {
                        ++mismatches;
                    }
                }
            }
            CAPTURE(fine_lod);
            CHECK(mismatches == 0);

            // Left to its own octave count the fine chunk would carry more
            // detail than the coarse one there, which is the geomorph's to
            // hide, so the override above is doing something.
            REQUIRE(sampler.chunk(fine.origin, fine_lod).detail_octaves > coarse.detail_octaves);
        }
    }

    TEST_CASE("coarse partial sums equal the parent chunk's own samples bit for bit", "[terrain]")
    {
        TerrainSampler sampler{example_planet(kSeed)};

        for (u32 const fine_lod : {0u, 4u, 8u})
        {
            u32 const    coarse_lod = fine_lod + 1;
            ChunkRequest parent     = sampler.chunk(surface_chunk(sampler.terrain(), coarse_lod, f64vec3{-0.4, 1.0, 0.7}), coarse_lod);
            Chunk const  p          = sample(sampler, parent);
            f64 const    voxel      = sampler.terrain().voxel_size(coarse_lod);

            // A child in the parent's lowest corner: every coarse point and
            // both its neighbours on each axis are parent samples.
            Chunk const child = sample(sampler, sampler.chunk(parent.origin, fine_lod), true);
            REQUIRE(child.request.coarse_octaves == parent.detail_octaves);

            u32 const n                   = child.request.coarse_per_axis();
            u32       value_mismatches    = 0;
            u32       gradient_mismatches = 0;
            for (u32 c = 0; c < n; ++c)
            {
                for (u32 b = 0; b < n; ++b)
                {
                    for (u32 a = 0; a < n; ++a)
                    {
                        // Coarse point a is the parent's sample a + 2.
                        REQUIRE(child.grid(2 * a + 2, 2 * b + 2, 2 * c + 2) == p.grid(a + 2, b + 2, c + 2));

                        size_t const  index    = child.coarse_index(a, b, c);
                        f32vec3 const gradient = child.coarse_gradients[index];
                        f32vec3 const expected{
                            central_difference(p.at(a + 1, b + 2, c + 2), p.at(a + 3, b + 2, c + 2), voxel),
                            central_difference(p.at(a + 2, b + 1, c + 2), p.at(a + 2, b + 3, c + 2), voxel),
                            central_difference(p.at(a + 2, b + 2, c + 1), p.at(a + 2, b + 2, c + 3), voxel),
                        };

                        if (!same_bits(child.coarse_values[index], p.at(a + 2, b + 2, c + 2)))
                        {
                            ++value_mismatches;
                        }
                        if (!same_bits(gradient.x, expected.x) || !same_bits(gradient.y, expected.y) ||
                            !same_bits(gradient.z, expected.z))
                        {
                            ++gradient_mismatches;
                        }
                    }
                }
            }
            CAPTURE(fine_lod);
            CHECK(value_mismatches == 0);
            CHECK(gradient_mismatches == 0);

            // A child across the parent's +x face, as at an LOD transition:
            // its coarse points on the face are the parent's apron samples.
            Chunk const neighbour = sample(sampler, sampler.chunk(parent.origin + i64vec3{i64{32} << coarse_lod, 0, 0}, fine_lod), true);
            u32         face_mismatches = 0;
            for (u32 c = 0; c < n; ++c)
            {
                for (u32 b = 0; b < n; ++b)
                {
                    REQUIRE(neighbour.grid(2, 2 * b + 2, 2 * c + 2) == p.grid(34, b + 2, c + 2));
                    if (!same_bits(neighbour.coarse_values[neighbour.coarse_index(0, b, c)], p.at(34, b + 2, c + 2)))
                    {
                        ++face_mismatches;
                    }
                }
            }
            CHECK(face_mismatches == 0);

            // The coarse output is the truncated sum, not the chunk's own
            // field: somewhere they must differ.
            bool differs = false;
            for (u32 a = 0; a < n && !differs; ++a)
            {
                differs = !same_bits(child.coarse_values[child.coarse_index(a, a, a)], child.at(2 * a + 2, 2 * a + 2, 2 * a + 2));
            }
            CHECK(differs);
        }
    }

    TEST_CASE("point queries agree with the chunk path", "[terrain]")
    {
        TerrainSampler sampler{example_planet(kSeed)};

        std::mt19937_64                     random{7};
        std::uniform_real_distribution<f64> direction{-1.0, 1.0};
        std::uniform_int_distribution<u32>  interior{1, 34};

        for (u32 const lod : {0u, 2u, 4u, 9u})
        {
            f64 worst_value    = 0.0;
            f64 worst_gradient = 0.0;
            for (int trial = 0; trial < 6; ++trial)
            {
                f64vec3 const facing{direction(random), direction(random), direction(random)};
                Chunk const   chunk = sample(sampler, sampler.chunk(surface_chunk(sampler.terrain(), lod, facing), lod));
                f64 const     voxel = sampler.terrain().voxel_size(lod);

                for (int point = 0; point < 16; ++point)
                {
                    u32 const i = interior(random);
                    u32 const j = interior(random);
                    u32 const k = interior(random);

                    f64vec3 const     p      = position(sampler.terrain(), chunk.grid(i, j, k));
                    PointSample const sample = sampler.sample_point(p, voxel, chunk.request.detail_octaves);

                    f64vec3 const gradient{
                        central_difference(chunk.at(i - 1, j, k), chunk.at(i + 1, j, k), voxel),
                        central_difference(chunk.at(i, j - 1, k), chunk.at(i, j + 1, k), voxel),
                        central_difference(chunk.at(i, j, k - 1), chunk.at(i, j, k + 1), voxel),
                    };

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
