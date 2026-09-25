#include "core/pch.hpp"

#include "terrain/planet.hpp"

#include "assets/asset_manager.hpp"
#include "core/log.hpp"
#include "platform/worker_pool.hpp"
#include "render/components.hpp"
#include "render/scene.hpp"
#include "terrain/surface_nets.hpp"
#include "world/transform.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace encke::terrain
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // Rounds toward negative infinity; `divisor` is positive.
        i64 floor_div(i64 value, i64 divisor)
        {
            i64 const quotient = value / divisor;
            return value % divisor < 0 ? quotient - 1 : quotient;
        }

        MacroChannelSpec const& channel(BodyTerrain const& terrain, MacroChannel which)
        {
            return terrain.macro.channels[static_cast<size_t>(which)];
        }

        f64 channel_bound(BodyTerrain const& terrain, MacroChannel which)
        {
            MacroChannelSpec const& spec = channel(terrain, which);
            return std::abs(static_cast<f64>(spec.bias)) + std::abs(static_cast<f64>(spec.scale));
        }

        // A chunk's edge in grid points and in metres.
        i64 extent_of(u32 lod)
        {
            return i64{32} << lod;
        }

        f64 extent_metres(BodyTerrain const& terrain, u32 lod)
        {
            return static_cast<f64>(extent_of(lod)) * terrain.base_voxel_size;
        }

        // How far `point` is from the chunk's box; 0 inside it.
        f64 distance_to(BodyTerrain const& terrain, NodeKey const& key, f64vec3 const& point)
        {
            f64vec3 const low  = f64vec3{key.origin} * terrain.base_voxel_size;
            f64vec3 const high = low + f64vec3{extent_metres(terrain, key.lod)};
            f64vec3 const out  = glm::max(glm::max(low - point, point - high), f64vec3{0.0});
            return glm::length(out);
        }

        NodeKey parent_of(NodeKey const& key)
        {
            i64 const parent_extent = extent_of(key.lod + 1);
            return NodeKey{
                .lod    = key.lod + 1,
                .origin = i64vec3{floor_div(key.origin.x, parent_extent), floor_div(key.origin.y, parent_extent),
                                  floor_div(key.origin.z, parent_extent)} *
                          parent_extent,
            };
        }

        // The terrain's ambientCG set and the metres one repeat covers.
        constexpr char kGroundSet[] = "Ground110";
        constexpr f32  kGroundTile  = 2.1f;

        // Surface nets output as render vertices, UVs in metres for a tiling
        // material that repeats every `tile` metres. Each vertex is projected
        // onto the face of the body's cube its position points through, from
        // the body's centre: the two other body-relative coordinates are u
        // and v. Triangles whose corners pick different faces, along the
        // cube's edges, are smeared.
        //
        // Those coordinates reach thousands of kilometres, where f32 steps
        // by tens of centimetres, a hundred texels, and the GPU's per-pixel
        // interpolation rounds differently as the view turns: the texture and
        // its normal map swim. So each chunk subtracts, per face, a whole
        // number of tiles taken from its corner, in f64. The texture repeats,
        // so it is unchanged, and neighbouring chunks' offsets differ by whole
        // tiles, so it stays continuous across them; and a chunk's UVs are no
        // bigger than the chunk. Near the camera that is metres.
        void to_vertices(SurfaceMesh const& mesh, f64vec3 const& corner, f64 tile, MeshData& data)
        {
            // Per face axis: the directions u and v run along.
            array<f64vec3, 3> const u_axes{{{0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}};
            array<f64vec3, 3> const v_axes{{{0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 1.0, 0.0}}};

            array<f64vec2, 3> offsets{};
            for (size_t face = 0; face < 3; ++face)
            {
                offsets[face] = f64vec2{std::floor(glm::dot(corner, u_axes[face]) / tile),
                                        std::floor(glm::dot(corner, v_axes[face]) / tile)} *
                                tile;
            }

            data.vertices.resize(mesh.positions.size());
            for (size_t i = 0; i < mesh.positions.size(); ++i)
            {
                f64vec3 const point = corner + f64vec3{mesh.positions[i]};
                f64vec3 const size  = glm::abs(point);
                size_t const  face  = size.x >= size.y && size.x >= size.z ? 0 : (size.y >= size.z ? 1 : 2);

                f64vec3 const& u_axis = u_axes[face];
                f64vec3 const& v_axis = v_axes[face];
                f64vec3 const  normal{mesh.normals[i]};

                // +u laid into the surface, and the sign that makes
                // cross(normal, tangent) * w point to -v, the top of the image.
                f64vec3 tangent = u_axis - normal * glm::dot(normal, u_axis);
                if (glm::length(tangent) < 1e-6)
                {
                    tangent = v_axis - normal * glm::dot(normal, v_axis);
                }
                tangent     = glm::normalize(tangent);
                f64 const w = glm::dot(glm::cross(normal, tangent), -v_axis) >= 0.0 ? 1.0 : -1.0;

                data.vertices[i] = Vertex{
                    .position = mesh.positions[i],
                    .normal   = mesh.normals[i],
                    .tangent  = f32vec4{f32vec3{tangent}, static_cast<f32>(w)},
                    .uv       = f32vec2{static_cast<f32>(glm::dot(point, u_axis) - offsets[face].x),
                                        static_cast<f32>(glm::dot(point, v_axis) - offsets[face].y)},
                };
            }
            data.indices = mesh.indices;
        }
    }

    f64 height_bound(BodyTerrain const& terrain)
    {
        return channel_bound(terrain, MacroChannel::Height) + detail_bound_from(terrain, 0);
    }

    f64 detail_bound_from(BodyTerrain const& terrain, u32 first)
    {
        f64 const amplitude   = channel_bound(terrain, MacroChannel::DetailAmplitude);
        f64 const persistence = std::min(channel_bound(terrain, MacroChannel::Persistence), 1.0);

        f64 weight = 1.0;
        f64 sum    = 0.0;
        for (u32 octave = 0; octave < terrain.detail.octave_count; ++octave)
        {
            if (octave >= first)
            {
                sum += weight;
            }
            weight *= persistence;
        }
        return amplitude * sum;
    }

    bool chunk_may_have_surface(TerrainSampler& sampler, i64vec3 const& origin, u32 lod, f64 cull_factor)
    {
        BodyTerrain const& terrain = sampler.terrain();

        i64 const     extent  = extent_of(lod);
        f64vec3 const centre  = f64vec3{origin + i64vec3{extent / 2}} * terrain.base_voxel_size;
        f64 const     voxel   = terrain.voxel_size(lod);
        u32 const     octaves = octaves_for_voxel(sampler.octaves(), voxel);
        f64 const     value   = static_cast<f64>(sampler.sample_value(centre, voxel, octaves));

        f64 const half_diag = 0.5 * std::sqrt(3.0) * extent_metres(terrain, lod);
        return std::abs(value) <= cull_factor * half_diag + detail_bound_from(terrain, octaves);
    }

    GroundProbe::GroundProbe(BodyTerrain const& terrain, u32 lod)
        : sampler_(terrain)
        , voxel_size_(terrain.voxel_size(lod))
        , octaves_(octaves_for_voxel(sampler_.octaves(), terrain.voxel_size(lod)))
    {
    }

    optional<f64vec3> GroundProbe::hit(f64vec3 const& from, f64vec3 const& direction, f64 reach) const
    {
        auto const value = [&](f64 t) {
            return static_cast<f64>(sampler_.sample_value(from + direction * t, voxel_size_, octaves_));
        };

        f64 t = 0.0;
        f64 v = value(t);
        if (v <= 0.0)
        {
            return from;
        }

        // Steps of half the value are safe while the field's gradient stays
        // under 2, the cull factor's assumption too; a floor keeps the march
        // from crawling as it closes in.
        f64 const least = voxel_size_ * 0.05;
        while (t < reach)
        {
            f64 const next = t + std::max(0.5 * v, least);
            f64 const at   = value(next);
            if (at <= 0.0)
            {
                // Bracketed: bisect to well under a millimetre.
                f64 low  = t;
                f64 high = next;
                for (int step = 0; step < 40; ++step)
                {
                    f64 const middle = 0.5 * (low + high);
                    (value(middle) > 0.0 ? low : high) = middle;
                }
                return from + direction * high;
            }
            t = next;
            v = at;
        }
        return nullopt;
    }

    optional<f64vec3> GroundProbe::below(f64vec3 const& from) const
    {
        return hit(from, -glm::normalize(from));
    }

    ChunkPlan plan_chunks(TerrainSampler& sampler, u32 lod, f64 cull_factor)
    {
        BodyTerrain const& terrain = sampler.terrain();

        i64 const extent     = extent_of(lod);
        f64 const bound      = terrain.radius + height_bound(terrain);
        i64 const half_cells = static_cast<i64>(std::ceil(bound / terrain.base_voxel_size));
        i64 const low        = floor_div(-half_cells, extent);
        i64 const high       = floor_div(half_cells, extent);

        ChunkPlan plan{.lod = lod, .kept = {}, .culled = {}, .culled_frontier = {}};
        std::set<array<i64, 3>> kept;
        for (i64 z = low; z <= high; ++z)
        {
            for (i64 y = low; y <= high; ++y)
            {
                for (i64 x = low; x <= high; ++x)
                {
                    i64vec3 const origin = i64vec3{x, y, z} * extent;
                    if (chunk_may_have_surface(sampler, origin, lod, cull_factor))
                    {
                        plan.kept.push_back(origin);
                        kept.insert(array<i64, 3>{{x, y, z}});
                    }
                    else
                    {
                        plan.culled.push_back(origin);
                    }
                }
            }
        }

        for (i64vec3 const& origin : plan.culled)
        {
            i64vec3 const index = origin / extent;
            bool          near  = false;
            for (i64 dz = -1; dz <= 1 && !near; ++dz)
            {
                for (i64 dy = -1; dy <= 1 && !near; ++dy)
                {
                    for (i64 dx = -1; dx <= 1 && !near; ++dx)
                    {
                        near = kept.contains(array<i64, 3>{{index.x + dx, index.y + dy, index.z + dz}});
                    }
                }
            }
            if (near)
            {
                plan.culled_frontier.push_back(origin);
            }
        }
        return plan;
    }

    bool chunk_has_surface(span<f32 const> samples, u32 cells)
    {
        i32 const n    = static_cast<i32>(cells);
        i32 const axis = n + 4;
        auto const inside = [&](i32 x, i32 y, i32 z) {
            return samples[static_cast<size_t>(((z + 2) * axis + (y + 2)) * axis + (x + 2))] < 0.0f;
        };

        // The edges surface_nets owns: from corners 0 to cells - 1, along
        // +x, +y and +z.
        for (i32 z = 0; z < n; ++z)
        {
            for (i32 y = 0; y < n; ++y)
            {
                for (i32 x = 0; x < n; ++x)
                {
                    bool const here = inside(x, y, z);
                    if (here != inside(x + 1, y, z) || here != inside(x, y + 1, z) || here != inside(x, y, z + 1))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    size_t NodeKeyHash::operator()(NodeKey const& key) const
    {
        // FNV-1a over the four fields.
        u64 hash = 0xcbf29ce484222325ull;
        for (u64 const part : {static_cast<u64>(key.lod), static_cast<u64>(key.origin.x),
                               static_cast<u64>(key.origin.y), static_cast<u64>(key.origin.z)})
        {
            hash = (hash ^ part) * 0x100000001b3ull;
        }
        return static_cast<size_t>(hash);
    }

    u32 root_lod(BodyTerrain const& terrain)
    {
        f64 const bound = terrain.radius + height_bound(terrain);
        u32       lod   = 0;
        while (extent_metres(terrain, lod) < bound)
        {
            ++lod;
        }
        return lod;
    }

    vector<NodeKey> select_leaves(BodyTerrain const& terrain, f64vec3 const& camera, OctreeSettings const& settings,
                                  function<bool(NodeKey const&)> const& may_have_surface)
    {
        vector<NodeKey> leaves;

        function<void(NodeKey const&)> descend = [&](NodeKey const& key) {
            if (!may_have_surface(key))
            {
                return;
            }
            f64 const extent = extent_metres(terrain, key.lod);
            if (key.lod > settings.finest_lod && distance_to(terrain, key, camera) < settings.split_factor * extent)
            {
                i64 const half = extent_of(key.lod - 1);
                for (i64 z = 0; z < 2; ++z)
                {
                    for (i64 y = 0; y < 2; ++y)
                    {
                        for (i64 x = 0; x < 2; ++x)
                        {
                            descend(NodeKey{.lod = key.lod - 1, .origin = key.origin + i64vec3{x, y, z} * half});
                        }
                    }
                }
                return;
            }
            leaves.push_back(key);
        };

        // Eight roots about the centre.
        u32 const root   = root_lod(terrain);
        i64 const extent = extent_of(root);
        for (i64 z = -1; z < 1; ++z)
        {
            for (i64 y = -1; y < 1; ++y)
            {
                for (i64 x = -1; x < 1; ++x)
                {
                    descend(NodeKey{.lod = root, .origin = i64vec3{x, y, z} * extent});
                }
            }
        }
        return leaves;
    }

    struct TerrainOctree::Node
    {
        // Meshed; with no mesh when the chunk turned out to have no surface.
        bool ready     = false;
        // In the scene: its entity, if it has a mesh, exists.
        bool displayed = false;

        MeshHandle   mesh;
        entt::entity entity   = entt::null;
        u64          vertices = 0;

        // Read by the node's job before it starts, so one the camera has
        // moved off is skipped.
        std::shared_ptr<std::atomic<bool>> wanted;

        // The pool's order among queued jobs: metres from the camera,
        // rewritten every frame, so the nearest chunk now is meshed next. An
        // unwanted node's is below every distance, so its skip is quick and
        // frees its slot in the queue.
        std::shared_ptr<std::atomic<f64>> priority;
    };

    struct TerrainOctree::Body
    {
        entt::entity                       entity = entt::null;
        std::shared_ptr<BodyTerrain const> terrain;

        // The main thread's, for cull tests.
        std::unique_ptr<TerrainSampler> planner;

        // One per worker, made on first use by that worker alone.
        vector<std::unique_ptr<TerrainSampler>> samplers;

        // Main thread from here down.
        std::unordered_map<NodeKey, bool, NodeKeyHash> may_have_surface;
        std::unordered_map<NodeKey, Node, NodeKeyHash> nodes;
        u32                                            in_flight = 0;

        bool              settled = false;
        Clock::time_point busy_since;

        TerrainSampler& sampler(u32 worker)
        {
            if (!samplers[worker])
            {
                samplers[worker] = std::make_unique<TerrainSampler>(*terrain);
            }
            return *samplers[worker];
        }
    };

    TerrainOctree::TerrainOctree(OctreeSettings const& settings)
        : settings_(settings)
    {
    }

    TerrainOctree::~TerrainOctree() = default;

    void TerrainOctree::submit(std::shared_ptr<Body> const& body, NodeKey const& key)
    {
        ++body->in_flight;
        Node const&                              node   = body->nodes.at(key);
        std::shared_ptr<std::atomic<bool>> const wanted = node.wanted;

        pool_->submit([this, body, key, wanted](u32 worker) -> WorkerPool::Completion {
            if (!wanted->load())
            {
                return [this, body, key] { complete(body, key, nullptr, true); };
            }

            TerrainSampler&    sampler = body->sampler(worker);
            ChunkRequest const request = sampler.chunk(key.origin, key.lod);

            vector<f32> samples(request.sample_count());
            sampler.sample_chunk(request, samples);

            SurfaceMesh mesh;
            surface_nets(samples, request.cells, body->terrain->voxel_size(key.lod), mesh);

            f64vec3 const corner = f64vec3{key.origin} * body->terrain->base_voxel_size;
            auto          data   = std::make_shared<MeshData>();
            // The tile exactly as the material has it, an f32, so the offsets
            // are whole tiles to the shader too.
            to_vertices(mesh, corner, static_cast<f64>(kGroundTile), *data);

            return [this, body, key, data] { complete(body, key, data, false); };
        }, node.priority);
    }

    void TerrainOctree::complete(std::shared_ptr<Body> const& body, NodeKey const& key,
                                 std::shared_ptr<MeshData> const& data, bool skipped)
    {
        --body->in_flight;

        auto const found = body->nodes.find(key);
        if (found == body->nodes.end())
        {
            return;
        }
        Node&      node   = found->second;
        bool const wanted = node.wanted->load();

        if (skipped)
        {
            // Wanted again since it was skipped: back in the queue.
            if (wanted)
            {
                submit(body, key);
            }
            else
            {
                body->nodes.erase(found);
            }
            return;
        }
        if (!wanted)
        {
            body->nodes.erase(found);
            return;
        }

        if (!data->indices.empty())
        {
            node.vertices = data->vertices.size();
            node.mesh     = assets_->add_mesh("terrain L" + std::to_string(key.lod) + " " + std::to_string(key.origin.x) +
                                                  " " + std::to_string(key.origin.y) + " " + std::to_string(key.origin.z),
                                              std::move(*data));
        }
        node.ready = true;
    }

    void TerrainOctree::update(Scene& scene, AssetManager& assets, WorkerPool& pool)
    {
        assets_ = &assets;
        pool_   = &pool;
        entt::registry& registry = scene.registry;

        // One tiling set for every body until bodies name their own.
        if (!material_)
        {
            material_ = assets.load_ambientcg(kGroundSet, f32vec2{kGroundTile});
        }

        for (auto const [entity, planet] : registry.view<PlanetTerrain const>().each())
        {
            if (std::ranges::none_of(bodies_, [entity](std::shared_ptr<Body> const& body) { return body->entity == entity; }))
            {
                auto body     = std::make_shared<Body>();
                body->entity  = entity;
                body->terrain = planet.terrain;
                body->planner = std::make_unique<TerrainSampler>(*planet.terrain);
                body->samplers.resize(pool.size());
                body->busy_since = Clock::now();
                bodies_.push_back(std::move(body));
            }
        }

        WorldTransform const* const camera = registry.try_get<WorldTransform>(scene.camera);
        if (camera == nullptr)
        {
            return;
        }

        for (std::shared_ptr<Body> const& body : bodies_)
        {
            WorldTransform const* const frame = registry.try_get<WorldTransform>(body->entity);
            if (frame == nullptr)
            {
                continue;
            }
            BodyTerrain const& terrain = *body->terrain;
            f64vec3 const      eye     = glm::inverse(frame->rotation) * (camera->position - frame->position);

            vector<NodeKey> const leaves = select_leaves(terrain, eye, settings_, [&](NodeKey const& key) {
                auto const [known, added] = body->may_have_surface.try_emplace(key, false);
                if (added)
                {
                    known->second = chunk_may_have_surface(*body->planner, key.origin, key.lod, settings_.cull_factor);
                }
                return known->second;
            });
            std::unordered_set<NodeKey, NodeKeyHash> const target(leaves.begin(), leaves.end());

            for (auto& [key, node] : body->nodes)
            {
                bool const wanted = target.contains(key);
                node.wanted->store(wanted);
                node.priority->store(wanted ? distance_to(terrain, key, eye) : -1.0, std::memory_order_relaxed);
            }

            // New leaves. The pool orders them, and everything already
            // queued, by distance from the camera as it stands when a worker
            // frees up.
            for (NodeKey const& leaf : leaves)
            {
                if (body->nodes.contains(leaf))
                {
                    continue;
                }
                body->nodes.emplace(leaf, Node{
                                              .ready     = false,
                                              .displayed = false,
                                              .mesh      = MeshHandle{},
                                              .entity    = entt::null,
                                              .vertices  = 0,
                                              .wanted    = std::make_shared<std::atomic<bool>>(true),
                                              .priority  = std::make_shared<std::atomic<f64>>(distance_to(terrain, leaf, eye)),
                                          });
                submit(body, leaf);
                if (body->settled)
                {
                    body->settled    = false;
                    body->busy_since = Clock::now();
                }
            }

            auto const displayed_ancestor = [&](NodeKey key) -> optional<NodeKey> {
                u32 const root = root_lod(terrain);
                while (key.lod < root)
                {
                    key = parent_of(key);
                    auto const found = body->nodes.find(key);
                    if (found != body->nodes.end() && found->second.displayed)
                    {
                        return key;
                    }
                }
                return nullopt;
            };

            // For each node on screen, how many of the leaves inside it are
            // still being meshed. Nodes on screen never overlap, so a leaf has
            // at most one displayed ancestor.
            std::unordered_map<NodeKey, u32, NodeKeyHash> pending_under;
            for (NodeKey const& leaf : leaves)
            {
                if (!body->nodes.at(leaf).ready)
                {
                    if (optional<NodeKey> const above = displayed_ancestor(leaf))
                    {
                        ++pending_under[*above];
                    }
                }
            }

            // Off a node the camera no longer wants once what replaces it is
            // ready: its target ancestor when merging, every leaf inside it
            // when splitting.
            auto const hide = [&](Node& node) {
                if (node.entity != entt::null)
                {
                    registry.destroy(node.entity);
                    node.entity = entt::null;
                }
                if (node.mesh)
                {
                    assets.release_mesh(node.mesh);
                    node.mesh = MeshHandle{};
                }
                node.displayed = false;
            };

            vector<NodeKey> retire;
            for (auto& [key, node] : body->nodes)
            {
                if (!node.displayed || target.contains(key))
                {
                    continue;
                }
                optional<NodeKey> target_above;
                for (NodeKey above = key; above.lod < root_lod(terrain);)
                {
                    above = parent_of(above);
                    if (target.contains(above))
                    {
                        target_above = above;
                        break;
                    }
                }
                bool const covered = target_above.has_value()
                                         ? body->nodes.at(*target_above).ready
                                         : !pending_under.contains(key);
                if (covered)
                {
                    retire.push_back(key);
                }
            }
            for (NodeKey const& key : retire)
            {
                hide(body->nodes.at(key));
                body->nodes.erase(key);
            }

            // On with every ready leaf nothing coarser still covers.
            for (NodeKey const& leaf : leaves)
            {
                Node& node = body->nodes.at(leaf);
                if (!node.ready || node.displayed || displayed_ancestor(leaf).has_value())
                {
                    continue;
                }
                if (node.mesh)
                {
                    node.entity = registry.create();
                    registry.emplace<Transform>(node.entity, Transform{
                                                                 .position = f64vec3{leaf.origin} * terrain.base_voxel_size,
                                                                 .parent   = body->entity,
                                                             });
                    registry.emplace<Renderable>(node.entity, Renderable{
                                                                  .mesh      = node.mesh,
                                                                  .material  = material_,
                                                                  .albedo    = f32vec3{1.0f},
                                                                  .roughness = 1.0f,
                                                                  .metallic  = 1.0f,
                                                              });
                }
                node.displayed = true;
            }

            // Meshed but never shown, and no longer wanted.
            std::erase_if(body->nodes, [&](auto& entry) {
                auto& [key, node] = entry;
                if (target.contains(key) || !node.ready || node.displayed)
                {
                    return false;
                }
                hide(node);
                return true;
            });

            // Settled: exactly the leaves on screen, nothing in flight.
            bool const settled =
                body->in_flight == 0 &&
                std::ranges::all_of(leaves, [&](NodeKey const& leaf) { return body->nodes.at(leaf).displayed; }) &&
                std::ranges::all_of(body->nodes, [&](auto const& entry) { return target.contains(entry.first); });

            if (settled && !body->settled)
            {
                array<u32, 32> per_lod{};
                u64            vertices = 0;
                u32            drawn    = 0;
                for (auto const& [key, node] : body->nodes)
                {
                    if (node.mesh)
                    {
                        ++per_lod[std::min<size_t>(key.lod, per_lod.size() - 1)];
                        vertices += node.vertices;
                        ++drawn;
                    }
                }
                string lods;
                for (size_t lod = 0; lod < per_lod.size(); ++lod)
                {
                    if (per_lod[lod] > 0)
                    {
                        lods += " L" + std::to_string(lod) + ":" + std::to_string(per_lod[lod]);
                    }
                }
                log::info("terrain: settled, %zu leaves, %u with surface drawn (%s ), %llu vertices, %.1f ms since the "
                          "last change",
                          leaves.size(), drawn, lods.c_str(), static_cast<unsigned long long>(vertices),
                          std::chrono::duration<f64, std::milli>(Clock::now() - body->busy_since).count());
            }
            body->settled = settled;
        }
    }

    vector<NodeKey> TerrainOctree::displayed() const
    {
        vector<NodeKey> keys;
        for (std::shared_ptr<Body> const& body : bodies_)
        {
            for (auto const& [key, node] : body->nodes)
            {
                if (node.displayed)
                {
                    keys.push_back(key);
                }
            }
        }
        return keys;
    }

    vector<MeshHandle> TerrainOctree::meshes() const
    {
        vector<MeshHandle> handles;
        for (std::shared_ptr<Body> const& body : bodies_)
        {
            for (auto const& [key, node] : body->nodes)
            {
                if (node.mesh)
                {
                    handles.push_back(node.mesh);
                }
            }
        }
        return handles;
    }

    bool TerrainOctree::idle() const
    {
        return !bodies_.empty() &&
               std::ranges::all_of(bodies_, [](std::shared_ptr<Body> const& body) { return body->settled; });
    }
}
