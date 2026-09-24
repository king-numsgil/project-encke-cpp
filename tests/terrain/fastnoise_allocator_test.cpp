#include "core/pch.hpp"

#include "terrain/fastnoise.hpp"
#include "terrain/macro_field.hpp"
#include "terrain/terrain_field.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <mutex>
#include <thread>

#if ENCKE_USE_MIMALLOC
#   include <mimalloc.h>
#endif

// FastNoise2 is a static library built by vcpkg, linked into a binary whose
// operator new is mimalloc's. Its nodes come from its own pools, which it
// takes with std::malloc and returns with std::free; everything else it
// allocates -- decoded node data, metadata vectors, encoded strings -- goes
// through new and delete, so through mimalloc, from inside the library and
// from inlined header code in encke alike. These tests build, use and free
// all of it across threads, and free on other threads than the one that
// allocated. A heap mismatch shows as a crash, as wrong noise, or under
// mimalloc's MI_DEBUG_FULL as an "invalid pointer" report.
namespace encke::terrain
{
    namespace
    {
        constexpr int kThreads    = 8;
        constexpr int kRounds     = 40;
        constexpr int kGrid       = 16;
        constexpr int kGridValues = kGrid * kGrid * kGrid;
        constexpr int kSeed       = 1337;

        vector<f32> generate(FastNoise::Generator const& generator)
        {
            vector<f32> values(kGridValues);
            generator.GenUniformGrid3D(values.data(), -3.0f, 5.0f, 1000.0f, kGrid, kGrid, kGrid,
                                       7.0f, 7.0f, 7.0f, kSeed);
            return values;
        }
    }

    TEST_CASE("operator new in this binary is the configured allocator", "[terrain][allocator]")
    {
#if ENCKE_USE_MIMALLOC
        auto const probe = std::make_unique<int>(3);
        CHECK(mi_is_in_heap_region(probe.get()));
#else
        SUCCEED("mimalloc is off in this build (clang-sanitize); ASan watches the heap instead");
#endif
    }

    TEST_CASE("FastNoise2 node trees survive building and freeing across threads", "[terrain][allocator]")
    {
        string const graph = encode_fbm_graph(1'000.0f, 5, 0.5f, 2.0f);

        // Single-threaded, for comparison.
        vector<f32> expected;
        {
            FastNoise::SmartNode<> const node = FastNoise::NewFromEncodedNodeTree(graph.c_str(), kFeatureSet);
            REQUIRE(node);
            expected = generate(*node);
        }

        std::mutex                          handoff_lock;
        vector<FastNoise::SmartNode<>>      handoff;
        vector<std::unique_ptr<TerrainSampler>> samplers;
        std::atomic<int>                    mismatches{0};
        std::atomic<int>                    failures{0};

        {
            vector<std::jthread> threads;
            for (int t = 0; t < kThreads; ++t)
            {
                threads.emplace_back([&] {
                    for (int round = 0; round < kRounds; ++round)
                    {
                        // Encoding allocates NodeData and strings through new.
                        string const encoded = encode_fbm_graph(1'000.0f, 5, 0.5f, 2.0f);
                        if (encoded != graph)
                        {
                            ++failures;
                        }

                        FastNoise::SmartNode<> node = FastNoise::NewFromEncodedNodeTree(encoded.c_str(), kFeatureSet);
                        if (!node)
                        {
                            ++failures;
                            continue;
                        }
                        if (generate(*node) != expected)
                        {
                            ++mismatches;
                        }

                        // Every other tree is released by the main thread
                        // instead of this one.
                        if (round % 2 == 0)
                        {
                            std::scoped_lock lock{handoff_lock};
                            handoff.push_back(std::move(node));
                        }
                    }

                    // A whole sampler too, destroyed after this thread ends.
                    auto sampler = std::make_unique<TerrainSampler>(example_planet(kSeed));
                    std::scoped_lock lock{handoff_lock};
                    samplers.push_back(std::move(sampler));
                });
            }
        }

        CHECK(failures == 0);
        CHECK(mismatches == 0);
        CHECK(handoff.size() == static_cast<size_t>(kThreads * kRounds / 2));
        CHECK(samplers.size() == static_cast<size_t>(kThreads));

        // The handed-off trees still work after their threads have gone.
        for (FastNoise::SmartNode<> const& node : handoff)
        {
            CHECK(generate(*node) == expected);
        }

        handoff.clear();
        samplers.clear();

#if ENCKE_USE_MIMALLOC
        // Frees from other threads are deferred in mimalloc; collecting
        // makes it walk and reclaim them now, inside the test.
        mi_collect(true);
#endif
    }
}
