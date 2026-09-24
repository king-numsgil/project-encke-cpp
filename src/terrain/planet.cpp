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

        // The material's lookup tables: slope across, height down.
        constexpr u32 kLutSize = 64;

        f32 srgb_to_linear(f32 c)
        {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        f32 linear_to_srgb(f32 c)
        {
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }

        // Lowland green through dry grass and bare rock to snow with height,
        // and toward rock with slope. Blended in linear space, stored sRGB.
        void build_luts(Pixels& albedo, Pixels& orm)
        {
            struct Band
            {
                f32     height;      // 0 lowest, 1 highest
                f32vec3 colour;      // sRGB
                f32     roughness;
            };
            array<Band, 5> const bands{{
                {0.00f, {0.20f, 0.27f, 0.14f}, 0.90f},
                {0.45f, {0.33f, 0.36f, 0.19f}, 0.90f},
                {0.60f, {0.45f, 0.40f, 0.30f}, 0.85f},
                {0.75f, {0.47f, 0.45f, 0.43f}, 0.80f},
                {0.90f, {0.90f, 0.91f, 0.93f}, 0.60f},
            }};
            f32vec3 const rock{0.40f, 0.37f, 0.34f};

            auto const linear = [](f32vec3 const& srgb) {
                return f32vec3{srgb_to_linear(srgb.r), srgb_to_linear(srgb.g), srgb_to_linear(srgb.b)};
            };

            albedo = Pixels{.width = kLutSize, .height = kLutSize, .rgba = vector<u8>(kLutSize * kLutSize * 4)};
            orm    = Pixels{.width = kLutSize, .height = kLutSize, .rgba = vector<u8>(kLutSize * kLutSize * 4)};

            for (u32 row = 0; row < kLutSize; ++row)
            {
                f32 const height = (static_cast<f32>(row) + 0.5f) / kLutSize;

                size_t band = 0;
                while (band + 2 < bands.size() && height > bands[band + 1].height)
                {
                    ++band;
                }
                Band const& low  = bands[band];
                Band const& high = bands[band + 1];
                f32 const   t    = std::clamp((height - low.height) / (high.height - low.height), 0.0f, 1.0f);

                f32vec3 const ground    = glm::mix(linear(low.colour), linear(high.colour), t);
                f32 const     roughness = glm::mix(low.roughness, high.roughness, t);

                for (u32 column = 0; column < kLutSize; ++column)
                {
                    f32 const slope = (static_cast<f32>(column) + 0.5f) / kLutSize;
                    f32 const bare  = glm::smoothstep(0.35f, 0.75f, slope);

                    f32vec3 const colour = glm::mix(ground, linear(rock), bare);
                    f32 const     rough  = glm::mix(roughness, 0.85f, bare);

                    size_t const texel = (static_cast<size_t>(row) * kLutSize + column) * 4;
                    for (glm::length_t c = 0; c < 3; ++c)
                    {
                        albedo.rgba[texel + static_cast<size_t>(c)] =
                            static_cast<u8>(std::lround(linear_to_srgb(colour[c]) * 255.0f));
                    }
                    albedo.rgba[texel + 3] = 255;

                    orm.rgba[texel + 0] = 255;
                    orm.rgba[texel + 1] = static_cast<u8>(std::lround(rough * 255.0f));
                    orm.rgba[texel + 2] = 0;
                    orm.rgba[texel + 3] = 255;
                }
            }
        }

        // Surface nets output as render vertices. UV addresses the lookup
        // tables: u the slope, 1 - cos of the angle from the body's up, v the
        // height against the height channel's range. Both stay half a texel
        // inside, since the material sampler repeats.
        void to_vertices(SurfaceMesh const& mesh, f64vec3 const& corner, BodyTerrain const& terrain,
                         f64 height_range, MeshData& data)
        {
            f32 const margin = 0.5f / kLutSize;

            data.vertices.resize(mesh.positions.size());
            for (size_t i = 0; i < mesh.positions.size(); ++i)
            {
                f32vec3 const normal = mesh.normals[i];
                f64vec3 const point  = corner + f64vec3{mesh.positions[i]};
                f64 const     radius = glm::length(point);
                f32vec3 const up{point / radius};

                f32 const slope  = std::clamp(1.0f - glm::dot(normal, up), 0.0f, 1.0f);
                f32 const height = static_cast<f32>(0.5 + 0.5 * (radius - terrain.radius) / height_range);

                // No normal map samples it, but keep the frame orthonormal.
                f32vec3 const across  = std::abs(normal.y) < 0.99f ? f32vec3{0.0f, 1.0f, 0.0f} : f32vec3{1.0f, 0.0f, 0.0f};
                f32vec3 const tangent = glm::normalize(glm::cross(across, normal));

                data.vertices[i] = Vertex{
                    .position = mesh.positions[i],
                    .normal   = normal,
                    .tangent  = f32vec4{tangent, 1.0f},
                    .uv       = f32vec2{std::clamp(slope * 4.0f, margin, 1.0f - margin),
                                        std::clamp(height, margin, 1.0f - margin)},
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

    void zero_height_at(BodyTerrain& terrain, f64vec3 const& point, u32 lod)
    {
        // With no detail octaves, the field is |p| - R - macro height.
        TerrainSampler    sampler{terrain};
        PointSample const sample = sampler.sample_point(point, terrain.voxel_size(lod), 0);
        f64 const         height = glm::length(point) - terrain.radius - static_cast<f64>(sample.value);

        MacroChannelSpec& spec = terrain.macro.channels[static_cast<size_t>(MacroChannel::Height)];
        spec.bias = static_cast<f32>(static_cast<f64>(spec.bias) - height);
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
        f64                                height_range = 1.0;

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

    MaterialHandle TerrainBuilder::material(AssetManager& assets)
    {
        if (!have_material_)
        {
            Pixels albedo;
            Pixels orm;
            build_luts(albedo, orm);
            material_ = assets.add_material(MaterialAsset{
                .name     = "terrain",
                .albedo   = assets.add_texture("terrain albedo", TextureEncoding::Srgb, std::move(albedo)),
                .normal   = TextureHandle{},
                .orm      = assets.add_texture("terrain orm", TextureEncoding::Linear, std::move(orm)),
                .emission = TextureHandle{},
                .tiling   = false,
            });
            have_material_ = true;
        }
        return material_;
    }

    void TerrainBuilder::build(Scene& scene, AssetManager& assets, WorkerPool& pool, TerrainBuildSettings const& settings)
    {
        MaterialHandle const surface_material = material(assets);

        for (auto const [entity, planet] : scene.registry.view<PlanetTerrain const>().each())
        {
            auto body          = std::make_shared<Body>();
            body->entity       = entity;
            body->terrain      = planet.terrain;
            body->lod          = settings.lod;
            body->height_range = std::max(std::abs(static_cast<f64>(channel(*planet.terrain, MacroChannel::Height).bias)) +
                                              std::abs(static_cast<f64>(channel(*planet.terrain, MacroChannel::Height).scale)),
                                          1.0);
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
                    to_vertices(mesh, corner, *body->terrain, body->height_range, *data);
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
