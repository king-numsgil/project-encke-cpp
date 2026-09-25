#include "core/pch.hpp"

#include "assets/asset_manager.hpp"
#include "platform/worker_pool.hpp"
#include "render/scene.hpp"
#include "terrain/planet.hpp"
#include "world/transform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <unordered_set>

namespace encke::terrain
{
    namespace
    {
        // A 40 km body with a few kilometres of relief, meshed no finer than
        // LOD 8 (64 m voxels, 2 km chunks), so a flight to its surface is a
        // few hundred chunks.
        BodyTerrain asteroid()
        {
            BodyTerrain terrain{.radius = 40'000.0, .seed = 5, .base_voxel_size = 0.25};
            terrain.macro.channels[static_cast<size_t>(MacroChannel::Height)] = MacroChannelSpec{
                .graph = encode_fbm_graph(30'000.0f, 3, 0.5f, 2.0f), .scale = 2'000.0f, .bias = 0.0f};
            terrain.macro.channels[static_cast<size_t>(MacroChannel::DetailAmplitude)].bias = 20.0f;
            terrain.macro.channels[static_cast<size_t>(MacroChannel::Persistence)].bias     = 0.5f;
            return terrain;
        }

        OctreeSettings const kSettings{.finest_lod = 8, .split_factor = 2.0, .cull_factor = 1.5};

        bool contains(NodeKey const& outer, NodeKey const& inner)
        {
            if (outer.lod <= inner.lod)
            {
                return false;
            }
            i64 const     extent = i64{32} << outer.lod;
            i64vec3 const offset = inner.origin - outer.origin;
            return glm::all(glm::greaterThanEqual(offset, i64vec3{0})) && glm::all(glm::lessThan(offset, i64vec3{extent}));
        }

        // No node on screen inside another.
        u32 overlaps(vector<NodeKey> const& nodes)
        {
            u32 count = 0;
            for (NodeKey const& a : nodes)
            {
                for (NodeKey const& b : nodes)
                {
                    count += contains(a, b) ? 1u : 0u;
                }
            }
            return count;
        }
    }

    TEST_CASE("octree leaves are disjoint and finest under the camera", "[terrain][octree]")
    {
        BodyTerrain const terrain = asteroid();
        TerrainSampler    sampler{terrain};
        f64vec3 const     camera{0.0, terrain.radius + 3'000.0, 0.0};

        vector<NodeKey> const leaves = select_leaves(terrain, camera, kSettings, [&](NodeKey const& key) {
            return chunk_may_have_surface(sampler, key.origin, key.lod, kSettings.cull_factor);
        });

        REQUIRE(!leaves.empty());
        CHECK(overlaps(leaves) == 0);

        u32 finest = ~0u;
        u32 coarsest = 0;
        for (NodeKey const& leaf : leaves)
        {
            finest   = std::min(finest, leaf.lod);
            coarsest = std::max(coarsest, leaf.lod);
        }
        CHECK(finest == kSettings.finest_lod);
        CHECK(coarsest > finest);
        CHECK(coarsest <= root_lod(terrain));
    }

    TEST_CASE("octree leaves that touch differ by at most one LOD", "[terrain][octree]")
    {
        // Geomorph's seams rely on it: a chunk collapses onto the LOD above
        // it and no further. Split factors over sqrt(3) guarantee it.
        BodyTerrain const terrain = asteroid();
        TerrainSampler    sampler{terrain};

        for (f64 const altitude : {500.0, 3'000.0, 12'000.0})
        {
            f64vec3 const         camera = glm::normalize(f64vec3{0.3, 1.0, -0.2}) * (terrain.radius + altitude);
            vector<NodeKey> const leaves = select_leaves(terrain, camera, kSettings, [&](NodeKey const& key) {
                return chunk_may_have_surface(sampler, key.origin, key.lod, kSettings.cull_factor);
            });

            u32 touching   = 0;
            u32 unbalanced = 0;
            for (NodeKey const& a : leaves)
            {
                i64vec3 const a_high = a.origin + i64vec3{i64{32} << a.lod};
                for (NodeKey const& b : leaves)
                {
                    i64vec3 const b_high = b.origin + i64vec3{i64{32} << b.lod};
                    bool const    touch  = glm::all(glm::lessThanEqual(a.origin, b_high)) &&
                                          glm::all(glm::lessThanEqual(b.origin, a_high)) && !(a == b);
                    if (touch && a.lod != b.lod)
                    {
                        ++touching;
                        unbalanced += (a.lod > b.lod ? a.lod - b.lod : b.lod - a.lod) > 1 ? 1u : 0u;
                    }
                }
            }
            CAPTURE(altitude, leaves.size(), touching);
            CHECK(touching > 0);
            CHECK(unbalanced == 0);
        }
    }

    TEST_CASE("geomorph ends before the parent replaces a chunk", "[terrain][octree]")
    {
        BodyTerrain const terrain = asteroid();
        for (u32 lod = kSettings.finest_lod; lod < root_lod(terrain); ++lod)
        {
            Geomorph const morph      = geomorph_for(terrain, lod, kSettings);
            f64 const      extent     = static_cast<f64>(i64{32} << lod) * terrain.base_voxel_size;
            f64 const      merge_from = kSettings.split_factor * 2.0 * extent;

            CAPTURE(lod);
            CHECK(morph.start < morph.end);
            // A voxel for apron vertices outside the chunk and one for the
            // frame the octree trails the camera.
            CHECK(static_cast<f64>(morph.end) <= merge_from - 2.0 * terrain.voxel_size(lod) + 1e-3);
        }
        Geomorph const root = geomorph_for(terrain, root_lod(terrain), kSettings);
        CHECK(root.start >= 1e30f);
    }

    TEST_CASE("the octree swaps chunks as the camera flies without overlap, and releases what it drops", "[terrain][octree]")
    {
        Scene        scene;
        AssetManager assets;
        WorkerPool   pool;
        pool.start(2, "octree test");

        entt::entity const body = scene.registry.create();
        scene.registry.emplace<Transform>(body);
        scene.registry.emplace<PlanetTerrain>(body, PlanetTerrain{.terrain = std::make_shared<BodyTerrain const>(asteroid())});

        scene.camera = scene.registry.create();
        scene.registry.emplace<Transform>(scene.camera);

        TerrainOctree octree{kSettings};

        vector<MeshHandle> released;
        auto const frame = [&](f64vec3 const& camera) {
            scene.registry.get<Transform>(scene.camera).position = camera;
            propagate_transforms(scene.registry);
            pool.drain();
            octree.update(scene, assets, pool);
            for (MeshHandle const handle : assets.take_released_meshes())
            {
                released.push_back(handle);
            }
            // Stand in for the renderer, so queued mesh data does not pile up.
            assets.take_ready_meshes();
        };

        // Every frame until nothing is left to swap, checking what is on
        // screen never overlaps on the way: the swaps are caught half done.
        u32 worst_overlap = 0;
        auto const settle = [&](f64vec3 const& camera) {
            frame(camera);
            for (int step = 0; step < 1'000'000 && !octree.idle(); ++step)
            {
                frame(camera);
                worst_overlap = std::max(worst_overlap, overlaps(octree.displayed()));
            }
            REQUIRE(octree.idle());
        };

        // From far out, down to the surface and along it, then back out.
        f64 const  radius = 40'000.0;
        vector<f64vec3> const path{
            {0.0, 400'000.0, 0.0}, {0.0, 120'000.0, 0.0}, {0.0, 60'000.0, 0.0}, {0.0, radius + 4'000.0, 0.0},
            {6'000.0, radius + 2'500.0, 2'000.0}, {15'000.0, radius, 8'000.0}, {0.0, 300'000.0, 0.0},
        };

        for (f64vec3 const& stop : path)
        {
            settle(stop);
        }
        CHECK(worst_overlap == 0);

        // Settled far out: only coarse nodes, exactly the leaves.
        {
            TerrainSampler sampler{asteroid()};
            vector<NodeKey> const leaves = select_leaves(asteroid(), path.back(), kSettings, [&](NodeKey const& key) {
                return chunk_may_have_surface(sampler, key.origin, key.lod, kSettings.cull_factor);
            });
            std::unordered_set<NodeKey, NodeKeyHash> const want(leaves.begin(), leaves.end());
            vector<NodeKey> const shown = octree.displayed();
            CHECK(shown.size() == leaves.size());
            for (NodeKey const& key : shown)
            {
                CHECK(want.contains(key));
            }
        }

        // Released meshes no longer resolve; held ones all do, and none is
        // both.
        REQUIRE(!released.empty());
        for (MeshHandle const handle : released)
        {
            CHECK(assets.mesh(handle) == nullptr);
        }
        for (MeshHandle const handle : octree.meshes())
        {
            CHECK(assets.mesh(handle) != nullptr);
        }

        pool.stop();
    }
}
