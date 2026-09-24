#pragma once

#include "assets/handle.hpp"
#include "terrain/terrain_field.hpp"

#include <memory>

namespace encke
{
    class AssetManager;
    class Scene;
    class WorkerPool;
}

namespace encke::terrain
{
    // On a body's entity: its terrain, meshed about the entity's origin,
    // which is therefore the body's centre, in the entity's frame. Radius,
    // seed and graph set are the BodyTerrain's.
    struct PlanetTerrain
    {
        std::shared_ptr<BodyTerrain const> terrain;
    };

    // A conservative bound on |height| anywhere: the height channel's bias
    // plus its scale, and the detail amplitude's over every octave at a
    // weight of at most one. Graphs are taken to stay within [-1, 1].
    f64 height_bound(BodyTerrain const& terrain);

    // Moves the height channel's bias so the macro height at `point` is zero,
    // as a chunk at `lod` samples it: puts a scene laid out on flat ground at
    // `point` on the surface.
    void zero_height_at(BodyTerrain& terrain, f64vec3 const& point, u32 lod);

    // The chunks of the body's bounding cube at one LOD, split by a cull test
    // at the chunk's centre: kept where |SDF| <= cull_factor * half-diagonal.
    // The field is not a true distance -- its gradient is 1 plus the height's
    // slope -- so a factor of 1 could cull a chunk the surface crosses;
    // cull_factor is the Lipschitz bound the culling trusts.
    struct ChunkPlan
    {
        u32             lod = 0;
        vector<i64vec3> kept;
        vector<i64vec3> culled;
        // Culled chunks sharing a face, edge or corner with a kept one. If the
        // surface crossed any culled chunk it would cross one of these: it is
        // closed and connected and passes through kept chunks.
        vector<i64vec3> culled_frontier;
    };
    ChunkPlan plan_chunks(TerrainSampler& sampler, u32 lod, f64 cull_factor);

    // Whether any edge a chunk owns changes sign: whether surface_nets would
    // emit anything for it. Samples as sample_chunk lays them out.
    bool chunk_has_surface(span<f32 const> samples, u32 cells);

    struct TerrainBuildSettings
    {
        u32  lod         = 17;
        f64  cull_factor = 1.5;
        // Also samples every culled frontier chunk on the pool and logs any
        // that has surface: the check that cull_factor is safe.
        bool verify_culling = false;
    };

    // Meshes every PlanetTerrain body in a scene at one uniform LOD on the
    // worker pool. Each chunk with surface becomes a mesh asset and a child
    // entity of its body, placed at the chunk's corner; vertices are metres
    // from there in f32. Main thread only; completions run in the pool's
    // drain().
    class TerrainBuilder
    {
    public:
        TerrainBuilder();
        ~TerrainBuilder();

        TerrainBuilder(TerrainBuilder const&)            = delete;
        TerrainBuilder& operator=(TerrainBuilder const&) = delete;

        void build(Scene& scene, AssetManager& assets, WorkerPool& pool, TerrainBuildSettings const& settings);

        // Every queued chunk meshed and in the scene.
        bool idle() const;

    private:
        struct Body;

        MaterialHandle material(AssetManager& assets);

        vector<std::shared_ptr<Body>> bodies_;
        MaterialHandle                material_;
        bool                          have_material_ = false;
    };
}
