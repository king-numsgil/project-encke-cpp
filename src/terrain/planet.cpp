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

        // The terrain's ambientCG set and the metres one repeat covers.
        constexpr char kGroundSet[] = "Ground110";
        constexpr f32  kGroundTile  = 2.1f;

        // Surface nets output as render vertices, UVs in metres for a tiling
        // material. Each vertex is projected onto the face of the body's cube
        // its position points through, from the body's centre: the two other
        // body-relative coordinates are u and v. They are small near each
        // face's centre, the pole among them, where f32 UVs keep their
        // precision; far from it they are large, and seen from far enough
        // away that only the smallest mips are read. Triangles whose corners
        // pick different faces, along the cube's edges, are smeared.
        void to_vertices(SurfaceMesh const& mesh, f64vec3 const& corner, MeshData& data)
        {
            // Per face axis: the directions u and v run along.
            array<f64vec3, 3> const u_axes{{{0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}};
            array<f64vec3, 3> const v_axes{{{0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 1.0, 0.0}}};

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
                    .uv       = f32vec2{static_cast<f32>(glm::dot(point, u_axis)),
                                        static_cast<f32>(glm::dot(point, v_axis))},
                };
            }
            data.indices = mesh.indices;
        }
    }

    f64 height_bound(BodyTerrain const& terrain)
    {
        MacroChannelSpec const& height    = channel(terrain, MacroChannel::Height);
        MacroChannelSpec const& amplitude = channel(terrain, MacroChannel::DetailAmplitude);
        f64 const               macro     = std::abs(static_cast<f64>(height.bias)) + std::abs(static_cast<f64>(height.scale));
        f64 const               detail    = (std::abs(static_cast<f64>(amplitude.bias)) + std::abs(static_cast<f64>(amplitude.scale))) *
                                            static_cast<f64>(terrain.detail.octave_count);
        return macro + detail;
    }

    GroundProbe::GroundProbe(BodyTerrain const& terrain, f64vec3 const& around, u32 lod)
    {
        TerrainSampler sampler{terrain};
        i64 const      extent   = i64{32} << lod;
        f64 const      extent_m = static_cast<f64>(extent) * terrain.base_voxel_size;
        f64vec3 const  down     = -glm::normalize(around);

        SurfaceMesh mesh;
        vector<f32> samples;
        for (u32 step = 0; step < 3; ++step)
        {
            f64vec3 const probe = around + down * (static_cast<f64>(step) * extent_m);
            i64vec3 const cell{glm::floor(probe / terrain.base_voxel_size)};
            i64vec3 const origin{floor_div(cell.x, extent) * extent, floor_div(cell.y, extent) * extent,
                                 floor_div(cell.z, extent) * extent};

            ChunkRequest const request = sampler.chunk(origin, lod);
            samples.resize(request.sample_count());
            sampler.sample_chunk(request, samples);
            surface_nets(samples, request.cells, terrain.voxel_size(lod), mesh);

            f64vec3 const corner = f64vec3{origin} * terrain.base_voxel_size;
            for (u32 const index : mesh.indices)
            {
                triangles_.push_back(corner + f64vec3{mesh.positions[index]});
            }
        }
    }

    optional<f64vec3> GroundProbe::hit(f64vec3 const& from, f64vec3 const& direction) const
    {
        // Moller-Trumbore in f64, nearest hit in front of `from`.
        optional<f64> nearest;
        for (size_t t = 0; t < triangles_.size(); t += 3)
        {
            f64vec3 const& a = triangles_[t];
            f64vec3 const  ab = triangles_[t + 1] - a;
            f64vec3 const  ac = triangles_[t + 2] - a;
            f64vec3 const  p  = glm::cross(direction, ac);
            f64 const      det = glm::dot(ab, p);
            if (std::abs(det) < 1e-12)
            {
                continue;
            }
            f64vec3 const s        = from - a;
            f64 const     u        = glm::dot(s, p) / det;
            f64vec3 const q        = glm::cross(s, ab);
            f64 const     v        = glm::dot(direction, q) / det;
            f64 const     distance = glm::dot(ac, q) / det;
            if (u < 0.0 || v < 0.0 || u + v > 1.0 || distance < 0.0)
            {
                continue;
            }
            if (!nearest.has_value() || distance < *nearest)
            {
                nearest = distance;
            }
        }
        if (!nearest.has_value())
        {
            return nullopt;
        }
        return from + direction * *nearest;
    }

    optional<f64vec3> GroundProbe::below(f64vec3 const& from) const
    {
        return hit(from, -glm::normalize(from));
    }

    ChunkPlan plan_chunks(TerrainSampler& sampler, u32 lod, f64 cull_factor)
    {
        BodyTerrain const& terrain = sampler.terrain();

        i64 const extent     = i64{32} << lod;
        f64 const bound      = terrain.radius + height_bound(terrain);
        i64 const half_cells = static_cast<i64>(std::ceil(bound / terrain.base_voxel_size));
        i64 const low        = floor_div(-half_cells, extent);
        i64 const high       = floor_div(half_cells, extent);

        f64 const voxel     = terrain.voxel_size(lod);
        u32 const octaves   = octaves_for_voxel(sampler.octaves(), voxel);
        f64 const half_diag = 0.5 * std::sqrt(3.0) * static_cast<f64>(extent) * terrain.base_voxel_size;

        ChunkPlan plan{.lod = lod, .kept = {}, .culled = {}, .culled_frontier = {}};
        std::set<array<i64, 3>> kept;
        for (i64 z = low; z <= high; ++z)
        {
            for (i64 y = low; y <= high; ++y)
            {
                for (i64 x = low; x <= high; ++x)
                {
                    i64vec3 const origin = i64vec3{x, y, z} * extent;
                    f64vec3 const centre = f64vec3{origin + i64vec3{extent / 2}} * terrain.base_voxel_size;
                    f64 const     value  = static_cast<f64>(sampler.sample_point(centre, voxel, octaves).value);

                    if (std::abs(value) <= cull_factor * half_diag)
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

    struct TerrainBuilder::Body
    {
        entt::entity                       entity = entt::null;
        std::shared_ptr<BodyTerrain const> terrain;
        u32                                lod          = 0;

        // One per worker, made on first use by that worker alone.
        vector<std::unique_ptr<TerrainSampler>> samplers;

        Clock::time_point start;
        // Nanoseconds from start to the last job's end on its worker.
        std::atomic<i64> last_finish{0};

        // Main thread.
        u32 planned   = 0;
        u32 culled    = 0;
        u32 frontier  = 0;
        u32 queued    = 0;
        u32 finished  = 0;
        u32 surfaced  = 0;
        u32 violations = 0;
        u64 vertices  = 0;
        u64 triangles = 0;

        TerrainSampler& sampler(u32 worker)
        {
            if (!samplers[worker])
            {
                samplers[worker] = std::make_unique<TerrainSampler>(*terrain);
            }
            return *samplers[worker];
        }

        void job_done()
        {
            i64 const now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
            i64       seen = last_finish.load();
            while (now > seen && !last_finish.compare_exchange_weak(seen, now))
            {
            }
        }

        void completed()
        {
            ++finished;
            if (finished != queued)
            {
                return;
            }
            log::info("terrain: LOD %u, %u chunks in the cube, %u culled, %u meshed, %u with surface; "
                      "%llu vertices, %llu triangles; %.1f ms on the pool",
                      lod, planned, culled, planned - culled, surfaced,
                      static_cast<unsigned long long>(vertices), static_cast<unsigned long long>(triangles),
                      static_cast<f64>(last_finish.load()) / 1e6);
            if (frontier > 0)
            {
                if (violations == 0)
                {
                    log::info("terrain: culling verified: none of %u culled frontier chunks has surface", frontier);
                }
                else
                {
                    log::error("terrain: %u of %u culled frontier chunks have surface; raise the cull factor",
                               violations, frontier);
                }
            }
        }
    };

    TerrainBuilder::TerrainBuilder()  = default;
    TerrainBuilder::~TerrainBuilder() = default;

    void TerrainBuilder::build(Scene& scene, AssetManager& assets, WorkerPool& pool, TerrainBuildSettings const& settings)
    {
        // One tiling set for every body until bodies name their own.
        MaterialHandle const surface_material = assets.load_ambientcg(kGroundSet, f32vec2{kGroundTile});

        for (auto const [entity, planet] : scene.registry.view<PlanetTerrain const>().each())
        {
            auto body          = std::make_shared<Body>();
            body->entity       = entity;
            body->terrain      = planet.terrain;
            body->lod          = settings.lod;
            body->samplers.resize(pool.size());

            TerrainSampler  planner{*planet.terrain};
            ChunkPlan const plan = plan_chunks(planner, settings.lod, settings.cull_factor);
            body->planned = static_cast<u32>(plan.kept.size() + plan.culled.size());
            body->culled  = static_cast<u32>(plan.culled.size());
            body->start   = Clock::now();

            Scene* const        scene_ptr  = &scene;
            AssetManager* const assets_ptr = &assets;
            f64 const           base_voxel = planet.terrain->base_voxel_size;

            for (i64vec3 const& origin : plan.kept)
            {
                ++body->queued;
                pool.submit([body, origin, scene_ptr, assets_ptr, surface_material, base_voxel](u32 worker) -> WorkerPool::Completion {
                    TerrainSampler&    sampler = body->sampler(worker);
                    ChunkRequest const request = sampler.chunk(origin, body->lod);

                    vector<f32> samples(request.sample_count());
                    sampler.sample_chunk(request, samples);

                    SurfaceMesh mesh;
                    surface_nets(samples, request.cells, sampler.terrain().voxel_size(body->lod), mesh);

                    f64vec3 const corner = f64vec3{origin} * base_voxel;
                    auto          data   = std::make_shared<MeshData>();
                    to_vertices(mesh, corner, *data);
                    body->job_done();

                    return [body, origin, corner, data, scene_ptr, assets_ptr, surface_material] {
                        if (!data->indices.empty())
                        {
                            ++body->surfaced;
                            body->vertices  += data->vertices.size();
                            body->triangles += data->indices.size() / 3;

                            string const name = "terrain chunk " + std::to_string(origin.x) + " " +
                                                std::to_string(origin.y) + " " + std::to_string(origin.z);
                            MeshHandle const handle = assets_ptr->add_mesh(name, std::move(*data));

                            entt::registry&    registry = scene_ptr->registry;
                            entt::entity const chunk    = registry.create();
                            registry.emplace<Transform>(chunk, Transform{.position = corner, .parent = body->entity});
                            registry.emplace<Renderable>(chunk, Renderable{
                                                                    .mesh      = handle,
                                                                    .material  = surface_material,
                                                                    .albedo    = f32vec3{1.0f},
                                                                    .roughness = 1.0f,
                                                                    .metallic  = 1.0f,
                                                                });
                        }
                        body->completed();
                    };
                });
            }

            if (settings.verify_culling)
            {
                body->frontier = static_cast<u32>(plan.culled_frontier.size());
                for (i64vec3 const& origin : plan.culled_frontier)
                {
                    ++body->queued;
                    pool.submit([body, origin](u32 worker) -> WorkerPool::Completion {
                        TerrainSampler&    sampler = body->sampler(worker);
                        ChunkRequest const request = sampler.chunk(origin, body->lod);
                        vector<f32>        samples(request.sample_count());
                        sampler.sample_chunk(request, samples);
                        bool const surface = chunk_has_surface(samples, request.cells);
                        body->job_done();

                        return [body, origin, surface] {
                            if (surface)
                            {
                                ++body->violations;
                                log::error("terrain: culled chunk %lld %lld %lld has surface",
                                           static_cast<long long>(origin.x), static_cast<long long>(origin.y),
                                           static_cast<long long>(origin.z));
                            }
                            body->completed();
                        };
                    });
                }
            }

            log::info("terrain: planned %zu chunks at LOD %u (%.0f m voxels), %zu culled at factor %.2f%s",
                      plan.kept.size() + plan.culled.size(), settings.lod, planet.terrain->voxel_size(settings.lod),
                      plan.culled.size(), settings.cull_factor,
                      settings.verify_culling ? ", verifying the frontier" : "");

            bodies_.push_back(std::move(body));
        }
    }

    bool TerrainBuilder::idle() const
    {
        return std::ranges::all_of(bodies_, [](std::shared_ptr<Body> const& body) { return body->finished == body->queued; });
    }
}
