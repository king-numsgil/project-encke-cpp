#pragma once

#include "assets/handle.hpp"
#include "render/components.hpp"
#include "terrain/terrain_field.hpp"

#include <memory>

namespace encke
{
    class AssetManager;
    class Scene;
    class WorkerPool;
    struct MeshData;
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

    // The most the detail octaves from `first` on can move the field: the
    // largest amplitude times the largest weights those octaves can have.
    f64 detail_bound_from(BodyTerrain const& terrain, u32 first);

    // Whether the surface may cross the chunk at `origin`, `lod`, or any chunk
    // inside it at a finer LOD: |SDF| at its centre within cull_factor
    // half-diagonals, plus what the octaves this LOD leaves out could add.
    // The field is not a true distance -- its gradient is 1 plus the
    // terrain's slope -- so cull_factor is the Lipschitz bound the test
    // trusts, and 1 is not safe.
    bool chunk_may_have_surface(TerrainSampler& sampler, i64vec3 const& origin, u32 lod, f64 cull_factor);

    // The field's own surface below a point, found by marching and bisecting
    // the point query at one LOD: what a chunk at that LOD meshes there, to
    // the voxel's few centimetres of Surface Nets error. For standing things
    // on the ground. Body-relative, like everything in BodyTerrain. One per
    // thread.
    class GroundProbe
    {
    public:
        GroundProbe(BodyTerrain const& terrain, u32 lod);

        // The first crossing along the ray from `from` along `direction`
        // (unit) within `reach` metres; nullopt if there is none. From
        // inside, `from` itself.
        optional<f64vec3> hit(f64vec3 const& from, f64vec3 const& direction, f64 reach = 100'000.0) const;

        // hit() toward the body's centre.
        optional<f64vec3> below(f64vec3 const& from) const;

    private:
        mutable TerrainSampler sampler_;
        f64                    voxel_size_ = 0.0;
        u32                    octaves_    = 0;
    };

    // The chunks of the body's bounding cube at one LOD, split by
    // chunk_may_have_surface.
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

    // An octree node: a chunk at one LOD, named by its corner on the grid.
    // Implicit: a node's children are the eight chunks of the next LOD down
    // inside it, found by arithmetic, and nothing stores the tree.
    struct NodeKey
    {
        u32     lod = 0;
        i64vec3 origin{0};

        bool operator==(NodeKey const&) const = default;
    };

    struct NodeKeyHash
    {
        size_t operator()(NodeKey const& key) const;
    };

    struct OctreeSettings
    {
        u32 finest_lod   = 0;
        // A node splits while the camera is within this many of its edges of
        // it, so a leaf's voxels cover about the same angle wherever it is.
        f64 split_factor = 2.0;
        f64 cull_factor  = 1.5;
    };

    // The coarsest LOD: the least whose chunk edge reaches from the centre
    // past the bounding radius, so eight chunks cover the body.
    u32 root_lod(BodyTerrain const& terrain);

    // The distances over which a chunk at `lod` morphs toward its parent,
    // with its extent and voxel; no neighbours marked.
    //
    // A node splits within split_factor k of its edges, E, so a leaf's
    // parent replaces it once the camera is 2kE from the parent: every
    // vertex of the leaf is then at least that far, less the voxel its
    // apron vertices reach outside the chunk. The morph ends a voxel short
    // of that, for the frame the camera moves before the octree follows, and
    // the swap shows nothing. It starts at (k + 1)E, past which a finer
    // neighbour is rare; within kE the chunk would itself have split, so at
    // the finest LOD, which never does, it starts there. The root never
    // merges and never morphs.
    Geomorph geomorph_for(BodyTerrain const& terrain, u32 lod, OctreeSettings const& settings);

    // The leaves for a camera at `camera`, body-relative: the nodes the
    // descent stops at, splitting from the eight roots while the camera is
    // near and the finest LOD is not reached, and skipping every node
    // `may_have_surface` rejects, with everything inside it. Disjoint, and
    // between them they hold every chunk of surface.
    //
    // `clearance` is a distance no surface is nearer the camera than; a node
    // counts as at least that far. A chunk's box can reach far closer than
    // the surface inside it, most of all from above, and splitting it for
    // that meshes children that are drawn fully morphed back to it.
    vector<NodeKey> select_leaves(BodyTerrain const& terrain, f64vec3 const& camera, OctreeSettings const& settings,
                                  function<bool(NodeKey const&)> const& may_have_surface, f64 clearance = 0.0);

    // A distance from `camera` within which there is no surface: its field
    // value over the Lipschitz bound the cull trusts. Zero inside.
    f64 surface_clearance(TerrainSampler& sampler, f64vec3 const& camera, OctreeSettings const& settings);

    // Every PlanetTerrain body in a scene as an implicit octree of chunks,
    // chosen each frame from the camera: nodes appear as the camera nears
    // and merge as it leaves. Chunks are meshed on the worker pool nearest
    // the camera first, by a distance kept current while they wait; each
    // with surface becomes a mesh asset and a child entity of
    // its body at the chunk's corner, its vertices f32 metres from there.
    //
    // Swaps leave no holes: a node the camera has moved off stays drawn
    // until everything that replaces it is ready, then goes in the same
    // frame the replacements appear, and its mesh is released.
    //
    // Every chunk geomorphs toward its parent (Geomorph, geomorph_for), so
    // by the distance a swap happens the two draw the same surface, and each
    // chunk carries masks of which neighbours on screen are coarser or
    // finer, which close the seams between LODs. Both hold once the octree
    // has caught up with the camera; while meshing lags, a node kept on
    // screen past its time can still pop when it goes.
    //
    // Main thread only; completions run in the pool's drain(), and update()
    // goes after it and before Scene::update.
    class TerrainOctree
    {
    public:
        explicit TerrainOctree(OctreeSettings const& settings);
        ~TerrainOctree();

        TerrainOctree(TerrainOctree const&)            = delete;
        TerrainOctree& operator=(TerrainOctree const&) = delete;

        void update(Scene& scene, AssetManager& assets, WorkerPool& pool);

        // Every body shows exactly the leaves the camera wants, all meshed.
        bool idle() const;

        // Frozen, update() selects, submits and swaps nothing: what is on
        // screen stays, with its neighbour masks, however the camera moves.
        // For looking at seams from where the octree did not choose them.
        // Jobs already queued still finish and wait.
        void set_frozen(bool frozen) { frozen_ = frozen; }
        bool frozen() const { return frozen_; }

        // What its chunks are drawn with, once the first update has loaded
        // it: for the renderer. A chunk's own Renderable names only the
        // first, gravel.
        optional<TerrainPalette> const& palette() const { return palette_; }

        // What is on screen, over every body, and every mesh the octree holds,
        // on screen or waiting: for tests.
        vector<NodeKey>    displayed() const;
        vector<MeshHandle> meshes() const;

    private:
        struct Body;
        struct Node;

        void submit(std::shared_ptr<Body> const& body, NodeKey const& key);
        void complete(std::shared_ptr<Body> const& body, NodeKey const& key,
                      std::shared_ptr<MeshData> const& data, bool skipped);

        // Rewrites the Geomorph masks of every node on screen next to one of
        // `changed`, which came on or went off this frame.
        void update_neighbours(Body& body, entt::registry& registry, span<NodeKey const> changed) const;

        OctreeSettings                settings_;
        vector<std::shared_ptr<Body>> bodies_;
        optional<TerrainPalette>      palette_;
        bool                          frozen_ = false;

        // Set by update(), for completions and resubmissions; main thread.
        AssetManager* assets_ = nullptr;
        WorkerPool*   pool_   = nullptr;
    };
}
