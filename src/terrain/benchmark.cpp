#include "core/pch.hpp"

#include "terrain/benchmark.hpp"

#include "terrain/terrain_field.hpp"

#include <chrono>
#include <cmath>
#include <exception>

namespace encke::terrain
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr u32 kChunksPerLod = 64;

        void print_octaves(TerrainSampler const& sampler)
        {
            std::printf("detail octaves per LOD (wavelength >= 2 voxels):\n");
            for (u32 lod = 0; lod <= 12; ++lod)
            {
                f64 const voxel = sampler.terrain().voxel_size(lod);
                std::printf("  LOD %2u  voxel %8.2f m  %2u octaves\n",
                            lod, voxel, octaves_for_voxel(sampler.octaves(), voxel));
            }
        }

        void run_lod(TerrainSampler& sampler, u32 lod)
        {
            i64 const stride = i64{1} << lod;
            i64 const extent = 32 * stride;

            // Chunks along the surface at the north pole, whole chunks apart
            // as a chunk grid would place them.
            f64 const radius_cells = sampler.terrain().radius / sampler.terrain().base_voxel_size;
            i64 const pole         = static_cast<i64>(std::floor(radius_cells / static_cast<f64>(extent))) * extent;

            ChunkRequest request = sampler.chunk(i64vec3{0, pole - 16 * stride, 0}, lod);
            u32 const    axis    = request.samples_per_axis();

            // As many samples as the chunk without its apron, 33^3: what 32
            // cells need for values alone.
            ChunkRequest bare = request;
            bare.cells        = request.cells - 3;

            vector<f32>     samples(request.sample_count());
            vector<f32>     bare_samples(bare.sample_count());
            vector<f32>     coarse_values(request.coarse_count());
            vector<f32vec3> coarse_gradients(request.coarse_count());
            vector<f32vec3> gradients(static_cast<size_t>(axis - 2) * (axis - 2) * (axis - 2));
            CoarseSamples const coarse{coarse_values, coarse_gradients};

            // One untimed chunk of each kind, so first-touch allocation is not
            // measured.
            sampler.sample_chunk(request, samples, &coarse);
            sampler.sample_chunk(bare, bare_samples);

            LayerTiming plain{};
            LayerTiming with_coarse{};
            f64         bare_seconds     = 0.0;
            f64         gradient_seconds = 0.0;
            f64         checksum         = 0.0;
            for (u32 chunk = 0; chunk < kChunksPerLod; ++chunk)
            {
                request.origin.x = static_cast<i64>(chunk % 8) * extent;
                request.origin.z = static_cast<i64>(chunk / 8) * extent;
                bare.origin      = request.origin;

                sampler.sample_chunk(request, samples);
                plain.macro  += sampler.last_timing().macro;
                plain.detail += sampler.last_timing().detail;

                sampler.sample_chunk(request, samples, &coarse);
                with_coarse.macro  += sampler.last_timing().macro;
                with_coarse.detail += sampler.last_timing().detail;
                with_coarse.coarse += sampler.last_timing().coarse;

                sampler.sample_chunk(bare, bare_samples);
                bare_seconds += sampler.last_timing().macro + sampler.last_timing().detail;

                // What a mesher does with the apron: central differences at
                // every sample that has both neighbours.
                Clock::time_point const start = Clock::now();
                f64 const               voxel = sampler.terrain().voxel_size(lod);
                size_t                  n     = 0;
                auto const at = [&](u32 i, u32 j, u32 k) { return samples[(static_cast<size_t>(k) * axis + j) * axis + i]; };
                for (u32 k = 1; k + 1 < axis; ++k)
                {
                    for (u32 j = 1; j + 1 < axis; ++j)
                    {
                        for (u32 i = 1; i + 1 < axis; ++i)
                        {
                            gradients[n++] = f32vec3{
                                central_difference(at(i - 1, j, k), at(i + 1, j, k), voxel),
                                central_difference(at(i, j - 1, k), at(i, j + 1, k), voxel),
                                central_difference(at(i, j, k - 1), at(i, j, k + 1), voxel),
                            };
                        }
                    }
                }
                gradient_seconds += std::chrono::duration<f64>(Clock::now() - start).count();

                checksum += static_cast<f64>(samples[samples.size() / 2]) + static_cast<f64>(gradients[n / 2].y) +
                            static_cast<f64>(coarse_values[coarse_values.size() / 2]) + static_cast<f64>(bare_samples[0]);
            }

            f64 const count     = static_cast<f64>(request.sample_count()) * kChunksPerLod;
            f64 const per_chunk = 1000.0 / kChunksPerLod;
            std::printf("LOD %u: voxel %.2f m, %u detail octaves (%u coarse), %u^3 samples per chunk, %u chunks\n",
                        lod, sampler.terrain().voxel_size(lod), request.detail_octaves, request.coarse_octaves,
                        axis, kChunksPerLod);
            std::printf("  macro   %12.0f samples/s  %7.3f ms per chunk\n", count / plain.macro, plain.macro * per_chunk);
            std::printf("  detail  %12.0f samples/s  %7.3f ms per chunk\n", count / plain.detail, plain.detail * per_chunk);
            std::printf("  total   %12.0f samples/s  %7.3f ms per chunk\n",
                        count / (plain.macro + plain.detail), (plain.macro + plain.detail) * per_chunk);
            std::printf("  33^3 without the apron          %7.3f ms per chunk\n", bare_seconds * per_chunk);
            std::printf("  central differences, %u^3       %7.3f ms per chunk\n", axis - 2, gradient_seconds * per_chunk);
            std::printf("  with coarse output: own %7.3f ms + coarse %7.3f ms per chunk\n",
                        (with_coarse.macro + with_coarse.detail) * per_chunk, with_coarse.coarse * per_chunk);
            // Printed so the optimiser cannot drop the work.
            std::printf("  checksum %.6g\n", checksum);
        }
    }

    int run_benchmark()
    {
        try
        {
            TerrainSampler sampler{example_planet(1337)};
            std::printf("terrain noise, one thread\n");
            print_octaves(sampler);
            run_lod(sampler, 0);
            run_lod(sampler, 4);
            return EXIT_SUCCESS;
        }
        catch (std::exception const& error)
        {
            std::fprintf(stderr, "terrain benchmark failed: %s\n", error.what());
            return EXIT_FAILURE;
        }
    }
}
