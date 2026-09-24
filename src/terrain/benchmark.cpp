#include "core/pch.hpp"

#include "terrain/benchmark.hpp"

#include "terrain/terrain_field.hpp"

#include <cmath>
#include <exception>

namespace encke::terrain
{
    namespace
    {
        constexpr u32 kChunksPerLod = 64;

        void run_lod(TerrainSampler& sampler, u32 lod)
        {
            f64 const voxel = sampler.terrain().voxel_size(lod);
            f64 const extent = 32.0 * voxel;

            // Chunks along the surface at the north pole, the first straddling
            // it, whole chunks apart as a chunk grid would place them.
            f64 const pole = std::floor(sampler.terrain().radius / extent) * extent;

            ChunkRequest request = sampler.chunk(f64vec3{0.0, pole - 16.0 * voxel, 0.0}, lod);
            vector<f32>  samples(request.sample_count());

            // One untimed chunk, so first-touch allocation is not measured.
            sampler.sample_chunk(request, samples);

            LayerTiming total{};
            f64         checksum = 0.0;
            for (u32 chunk = 0; chunk < kChunksPerLod; ++chunk)
            {
                request.origin.x = static_cast<f64>(chunk % 8) * extent;
                request.origin.z = static_cast<f64>(chunk / 8) * extent;
                sampler.sample_chunk(request, samples);

                total.macro  += sampler.last_timing().macro;
                total.detail += sampler.last_timing().detail;
                checksum     += static_cast<f64>(samples[samples.size() / 2]);
            }

            f64 const count = static_cast<f64>(request.sample_count()) * kChunksPerLod;
            std::printf("LOD %u: voxel %.2f m, %u detail octaves, %u^3 samples per chunk, %u chunks\n",
                        lod, voxel, request.detail_octaves, request.samples_per_axis(), kChunksPerLod);
            std::printf("  macro   %12.0f samples/s\n", count / total.macro);
            std::printf("  detail  %12.0f samples/s\n", count / total.detail);
            std::printf("  total   %12.0f samples/s  (%.3f ms per chunk)\n",
                        count / (total.macro + total.detail),
                        1000.0 * (total.macro + total.detail) / kChunksPerLod);
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
