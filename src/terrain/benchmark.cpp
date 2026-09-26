#include "core/pch.hpp"

#include "terrain/benchmark.hpp"

#include "platform/cpu.hpp"
#include "terrain/fastnoise.hpp"
#include "terrain/terrain_field.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <random>
#include <thread>

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

        // LOD 20's chunk is wider than the Earth; the octree's roots are
        // below it.
        constexpr u32 kSweepLods = 21;

        struct SweepResult
        {
            u32         lod            = 0;
            u32         detail_octaves = 0;
            u32         macro_nodes    = 0;
            f64         macro          = 0.0;
            f64         detail         = 0.0;
            f64         coarse         = 0.0;
            f64         setup          = 0.0;
            f64         perlin_floor   = 0.0;
            f64         graph_floor    = 0.0;
            f64         checksum       = 0.0;
            u64         hash           = 14695981039346656037ull;
            vector<f64> totals;
        };

        // FNV-1a over every byte of the values, so a change to any bit of
        // any output shows: the sweep doubles as a bit-exactness check.
        template <typename T>
        void hash_values(u64& hash, span<T const> values)
        {
            for (std::byte const byte : std::as_bytes(values))
            {
                hash = (hash ^ static_cast<u64>(byte)) * 1099511628211ull;
            }
        }

        // Nodes per axis of the macro lattice a chunk box of `samples` a
        // side spans, as MacroField::sample lays it out: at most, since the
        // box's corner decides whether it straddles one more.
        u32 macro_nodes_per_axis(BodyTerrain const& terrain, u32 lod, u32 samples)
        {
            f64 const voxel   = terrain.voxel_size(lod);
            f64 const spacing = std::max(terrain.macro.lattice_spacing, voxel);
            return static_cast<u32>(std::ceil(static_cast<f64>(samples - 1) * voxel / spacing)) + 2;
        }

        // What FastNoise2 alone costs for one chunk at this LOD: the Perlin
        // octaves over every sample, and the four field graphs over the macro
        // lattice. The rest of a chunk's time is ours.
        void measure_floors(BodyTerrain const& terrain, u32 lod, u32 detail_octaves, u32 reps, SweepResult& result)
        {
            u32 const    axis  = 36;
            size_t const count = static_cast<size_t>(axis) * axis * axis;

            // Lattice-space positions like an octave's: a few cells of
            // offset, spread over the block.
            std::mt19937                          rng{lod};
            std::uniform_real_distribution<f32>   cell{0.0f, 16.0f};
            vector<f32> x(count), y(count), z(count), out(count);
            for (size_t i = 0; i < count; ++i)
            {
                x[i] = cell(rng);
                y[i] = cell(rng);
                z[i] = cell(rng);
            }

            FastNoise::SmartNode<FastNoise::Perlin> perlin =FastNoise::New<FastNoise::Perlin>(kFeatureSet);
            perlin->SetScale(1.0f);

            f64 sink = 0.0;
            perlin->GenPositionArray3D(out.data(), static_cast<int>(count), x.data(), y.data(), z.data(),
                                       0.0f, 0.0f, 0.0f, 1);
            Clock::time_point const perlin_start = Clock::now();
            for (u32 rep = 0; rep < reps; ++rep)
            {
                for (u32 octave = 0; octave < detail_octaves; ++octave)
                {
                    perlin->GenPositionArray3D(out.data(), static_cast<int>(count), x.data(), y.data(), z.data(),
                                               0.3f, 0.7f, 0.1f, static_cast<int>(rep * 31 + octave));
                    sink += static_cast<f64>(out[count / 2]);
                }
            }
            result.perlin_floor = std::chrono::duration<f64>(Clock::now() - perlin_start).count() / reps;

            u32 const    nodes_axis = macro_nodes_per_axis(terrain, lod, axis);
            size_t const nodes      = static_cast<size_t>(nodes_axis) * nodes_axis * nodes_axis;
            f64 const    spacing    = std::max(terrain.macro.lattice_spacing, terrain.voxel_size(lod));
            vector<f32> nx(nodes), ny(nodes), nz(nodes), values(nodes);
            size_t n = 0;
            for (u32 k = 0; k < nodes_axis; ++k)
            {
                for (u32 j = 0; j < nodes_axis; ++j)
                {
                    for (u32 i = 0; i < nodes_axis; ++i)
                    {
                        nx[n] = static_cast<f32>(static_cast<f64>(i) * spacing);
                        ny[n] = static_cast<f32>(terrain.radius + static_cast<f64>(j) * spacing);
                        nz[n] = static_cast<f32>(static_cast<f64>(k) * spacing);
                        ++n;
                    }
                }
            }
            result.macro_nodes = static_cast<u32>(nodes);

            vector<FastNoise::SmartNode<>> graphs;
            for (size_t channel = 0; channel < kFieldChannelCount; ++channel)
            {
                graphs.push_back(FastNoise::NewFromEncodedNodeTree(terrain.macro.channels[channel].graph.c_str(),
                                                                   kFeatureSet));
            }
            Clock::time_point const graph_start = Clock::now();
            for (u32 rep = 0; rep < reps; ++rep)
            {
                for (FastNoise::SmartNode<> const& graph : graphs)
                {
                    graph->GenPositionArray3D(values.data(), static_cast<int>(nodes), nx.data(), ny.data(), nz.data(),
                                              0.0f, 0.0f, 0.0f, static_cast<int>(rep));
                    sink += static_cast<f64>(values[nodes / 2]);
                }
            }
            result.graph_floor = std::chrono::duration<f64>(Clock::now() - graph_start).count() / reps;
            result.checksum += sink;
        }

        // `chunks` chunks at one LOD, each with a new random seed and at a
        // random place on the surface, with the coarse output as the mesher
        // asks for it. Samplers are built off the clock.
        SweepResult sweep_lod(u32 lod, u32 chunks)
        {
            BodyTerrain const planet = example_planet(0);

            SweepResult result{};
            result.lod = lod;
            result.totals.reserve(chunks);

            i64 const stride = i64{1} << lod;
            i64 const extent = 32 * stride;

            std::mt19937_64                    rng{0x5EEDull * (lod + 1)};
            std::normal_distribution<f64>      normal{0.0, 1.0};
            std::uniform_real_distribution<f64> altitude{-3000.0, 3000.0};

            vector<f32>     samples;
            vector<f32>     coarse_values;
            vector<f32vec3> coarse_gradients;

            for (u32 chunk = 0; chunk < chunks; ++chunk)
            {
                BodyTerrain terrain = planet;
                terrain.seed        = bit_cast<i32>(static_cast<u32>(rng()));

                Clock::time_point const setup_start = Clock::now();
                TerrainSampler          sampler{terrain};
                result.setup += std::chrono::duration<f64>(Clock::now() - setup_start).count();

                f64vec3 direction{normal(rng), normal(rng), normal(rng)};
                direction = glm::normalize(direction);
                f64vec3 const cell = direction * (terrain.radius + altitude(rng)) / terrain.base_voxel_size;
                i64vec3 const origin{
                    static_cast<i64>(std::floor(cell.x / static_cast<f64>(extent))) * extent,
                    static_cast<i64>(std::floor(cell.y / static_cast<f64>(extent))) * extent,
                    static_cast<i64>(std::floor(cell.z / static_cast<f64>(extent))) * extent,
                };

                ChunkRequest const request = sampler.chunk(origin, lod);
                samples.resize(request.sample_count());
                coarse_values.resize(request.coarse_count());
                coarse_gradients.resize(request.coarse_count());
                CoarseSamples const coarse{coarse_values, coarse_gradients};

                Clock::time_point const start = Clock::now();
                sampler.sample_chunk(request, samples, &coarse);
                f64 const total = std::chrono::duration<f64>(Clock::now() - start).count();

                result.detail_octaves = request.detail_octaves;
                result.macro  += sampler.last_timing().macro;
                result.detail += sampler.last_timing().detail;
                result.coarse += sampler.last_timing().coarse;
                result.totals.push_back(total);
                result.checksum += static_cast<f64>(samples[samples.size() / 2]) +
                                   static_cast<f64>(coarse_values[coarse_values.size() / 2]);
                hash_values(result.hash, span<f32 const>{samples});
                hash_values(result.hash, span<f32 const>{coarse_values});
                hash_values(result.hash, span<f32vec3 const>{coarse_gradients});
            }

            measure_floors(planet, lod, result.detail_octaves, 200, result);
            return result;
        }

        f64 percentile(vector<f64> sorted, f64 fraction)
        {
            std::sort(sorted.begin(), sorted.end());
            size_t const index = std::min(sorted.size() - 1,
                                          static_cast<size_t>(fraction * static_cast<f64>(sorted.size())));
            return sorted[index];
        }
    }

    int run_sweep(u32 chunks, u32 threads)
    {
        try
        {
            if (threads == 0)
            {
                threads = std::max(1u, query_cpu().physical_cores - 1);
            }
            threads = std::min(threads, kSweepLods);
            std::printf("terrain sweep: LOD 0 to %u, %u chunks each, a new seed per chunk, %u thread%s\n",
                        kSweepLods - 1, chunks, threads, threads == 1 ? "" : "s");
            std::fflush(stdout);

            // Each LOD runs whole on one thread, taken in order, so the
            // costliest, with the most octaves, start first.
            array<SweepResult, kSweepLods> results{};
            std::atomic<u32>               next{0};
            {
                vector<std::jthread> workers;
                for (u32 t = 0; t < threads; ++t)
                {
                    workers.emplace_back([&] {
                        for (u32 i = next.fetch_add(1); i < kSweepLods; i = next.fetch_add(1))
                        {
                            results[i] = sweep_lod(i, chunks);
                            std::fprintf(stderr, "LOD %u done\n", i);
                        }
                    });
                }
            }

            std::printf("\nper chunk, mean over %u; ms unless stated\n", chunks);
            std::printf("LOD  oct   total    p50     p99   macro  detail  coarse | FN perlin  FN graphs (nodes) | ours   ours%% | setup | output hash\n");
            f64 checksum = 0.0;
            u64 hash     = 14695981039346656037ull;
            for (SweepResult const& r : results)
            {
                f64 const n      = static_cast<f64>(chunks);
                f64 const mean   = (r.macro + r.detail + r.coarse) / n * 1000.0;
                f64 const floors = (r.perlin_floor + r.graph_floor) * 1000.0;
                f64 const own    = (r.macro + r.detail) / n * 1000.0;
                std::printf("%3u  %3u  %6.3f  %6.3f  %6.3f  %6.3f  %6.3f  %6.3f | %9.3f  %9.3f %6u | %6.3f %5.0f%% | %5.3f | %016llx\n",
                            r.lod, r.detail_octaves, mean,
                            percentile(r.totals, 0.5) * 1000.0, percentile(r.totals, 0.99) * 1000.0,
                            r.macro / n * 1000.0, r.detail / n * 1000.0, r.coarse / n * 1000.0,
                            r.perlin_floor * 1000.0, r.graph_floor * 1000.0, r.macro_nodes,
                            own - floors, 100.0 * (own - floors) / own, r.setup / n * 1000.0,
                            static_cast<unsigned long long>(r.hash));
                checksum += r.checksum;
                hash_values(hash, span<u64 const>{&r.hash, 1});
            }
            std::printf("output hash %016llx\n", static_cast<unsigned long long>(hash));
            std::printf("checksum %.6g\n", checksum);
            return EXIT_SUCCESS;
        }
        catch (std::exception const& error)
        {
            std::fprintf(stderr, "terrain sweep failed: %s\n", error.what());
            return EXIT_FAILURE;
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
