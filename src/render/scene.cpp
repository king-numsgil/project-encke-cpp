#include "core/pch.hpp"

#include "render/scene.hpp"

#include "assets/asset_manager.hpp"
#include "core/log.hpp"
#include "render/mesh.hpp"
#include "render/pixels.hpp"
#include "terrain/planet.hpp"

#include <algorithm>
#include <cmath>

namespace encke
{
    namespace
    {
        // Where the north pole sits in the world: a kilometre-scale offset in
        // every axis, so the scene is where f32 world coordinates would
        // already be jittering. Set to zero to compare: the rendered image
        // must not change.
        f64vec3 const kWorldOrigin{1'000'000.0, 250'000.0, -700'000.0};

        // The Sun: 3.75e28 lm spread over 4 pi sr, which gives ~1.3e5 lux at
        // 1 AU. Low over the horizon so shadows are long enough to cross every
        // cascade.
        constexpr f64 kSunIntensity = 2.98e27;
        constexpr f64 kSunElevation = 0.21;   // radians, ~12 degrees
        constexpr f64 kSunAzimuth   = 0.6;

        constexpr f64 kPi = 3.14159265358979323846;

        f32 srgb_to_linear(f32 c)
        {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        f32vec3 srgb_to_linear(f32vec3 c)
        {
            return f32vec3{srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b)};
        }

        f32 cos_degrees(f64 degrees)
        {
            return static_cast<f32>(std::cos(degrees * kPi / 180.0));
        }

        // Tessellation of the procedural meshes.
        constexpr u32 kSphereSlices = 48;
        constexpr u32 kSphereStacks = 24;

        // The Earth's terrain noise.
        constexpr i32 kTerrainSeed = 1337;

        // The test scene's ambientCG sets and how many metres one repeat
        // covers, across and down; see assets/textures/CREDITS.md.
        struct Materials
        {
            MaterialHandle concrete;
            MaterialHandle planks;
            MaterialHandle painted_metal;
            MaterialHandle rusted_metal;
            MaterialHandle metal_plates;
        };

        Materials load_materials(AssetManager& assets)
        {
            return Materials{
                .concrete      = assets.load_ambientcg("Concrete034", f32vec2{1.1f, 0.55f}),
                .planks        = assets.load_ambientcg("Planks037A", f32vec2{2.0f}),
                .painted_metal = assets.load_ambientcg("PaintedMetal006", f32vec2{1.5f}),
                .rusted_metal  = assets.load_ambientcg("Metal041B", f32vec2{1.0f}),
                .metal_plates  = assets.load_ambientcg("MetalPlates013", f32vec2{1.6f}),
            };
        }
    }

    entt::entity Scene::add(MeshHandle mesh, f64vec3 position, f64vec3 scale, f32vec3 albedo_srgb,
                            f32 roughness, f32 metallic, f32vec3 emissive)
    {
        entt::entity const entity = registry.create();
        // On its own, by the lowest ground under its footprint's corners, so
        // no edge of it floats on a slope.
        f64vec2 const half{scale.x * 0.5, scale.z * 0.5};
        f64 const     lift = assembly_lift_.value_or(lowest_lift({
            f64vec2{position.x - half.x, position.z - half.y}, f64vec2{position.x + half.x, position.z - half.y},
            f64vec2{position.x - half.x, position.z + half.y}, f64vec2{position.x + half.x, position.z + half.y},
        }));
        registry.emplace<Transform>(entity, Transform{
                                                .position = origin_ + position + f64vec3{0.0, lift, 0.0},
                                                .scale    = f32vec3{scale},
                                            });
        registry.emplace<Renderable>(entity, Renderable{
                                                 .mesh      = mesh,
                                                 .material  = MaterialHandle{},
                                                 .albedo    = srgb_to_linear(albedo_srgb),
                                                 .roughness = roughness,
                                                 .emissive  = emissive,
                                                 .metallic  = metallic,
                                             });
        return entity;
    }

    entt::entity Scene::add(MeshHandle mesh, f64vec3 position, f64vec3 scale,
                            MaterialHandle material)
    {
        entt::entity const entity = add(mesh, position, scale, f32vec3{1.0f}, 1.0f, 1.0f);
        registry.get<Renderable>(entity).material = material;
        return entity;
    }

    f64 Scene::ground_lift(f64 x, f64 z) const
    {
        if (!ground_)
        {
            return 0.0;
        }

        // Straight down the scene's -Y from well above, onto the ground.
        f64vec3 const           from = ground_pole_ + f64vec3{x, 1'000.0, z};
        optional<f64vec3> const hit  = ground_->hit(from, f64vec3{0.0, -1.0, 0.0});
        return hit.has_value() ? hit->y - ground_pole_.y : 0.0;
    }

    f64 Scene::lowest_lift(std::initializer_list<f64vec2> supports) const
    {
        f64 lowest = std::numeric_limits<f64>::infinity();
        for (f64vec2 const& support : supports)
        {
            lowest = std::min(lowest, ground_lift(support.x, support.y));
        }
        return supports.size() > 0 ? lowest : 0.0;
    }

    f64vec3 Scene::grounded(f64vec3 const& position) const
    {
        f64 const lift = assembly_lift_.value_or(ground_lift(position.x, position.z));
        return position + f64vec3{0.0, lift, 0.0};
    }

    entt::entity Scene::add_light(entt::entity parent, f64vec3 position, f64vec3 direction,
                                  Light const& light)
    {
        entt::entity const entity = registry.create();
        registry.emplace<Transform>(entity, Transform{
                                                .position = position,
                                                .rotation = look_rotation(direction,
                                                                          f64vec3{0.0, 1.0, 0.0}),
                                                .parent   = parent,
                                            });
        registry.emplace<Light>(entity, light);
        return entity;
    }

    entt::entity Scene::spawn(f64vec3 const& position, f64quat const& orientation,
                              ModelSpawn const& spawn)
    {
        entt::entity const root = registry.create();
        registry.emplace<Transform>(root, Transform{.position = position, .rotation = orientation});
        registry.emplace<ModelSpawn>(root, spawn);
        return root;
    }

    void Scene::instantiate(entt::entity root, Model const& model, ModelSpawn const& spawn)
    {
        f64vec3 const extent = model.max - model.min;
        f64 const     widest = std::max({extent.x, extent.y, extent.z});
        f64 const     scale  = spawn.fit.has_value() && widest > 0.0 ? *spawn.fit / widest
                                                                     : spawn.scale;

        // In the root's frame, so it is along the root's +Y whatever the
        // root's orientation.
        f64vec3 const lift = spawn.rest_on_root ? f64vec3{0.0, -model.min.y * scale, 0.0}
                                                : f64vec3{0.0};

        // Parents precede children, so each node's parent entity exists by
        // the time it is reached.
        vector<entt::entity> nodes(model.nodes.size(), entt::entity{entt::null});
        for (size_t index = 0; index < model.nodes.size(); ++index)
        {
            ModelNode const& node = model.nodes[index];

            // Uniform, so it commutes with every rotation above: scaling each
            // offset and size is exactly scaling the whole model.
            f32vec3 const size{node.scale * scale};

            bool const top = !node.parent.has_value();

            entt::entity const entity = registry.create();
            registry.emplace<Transform>(entity, Transform{
                                                    .position = node.position * scale +
                                                                (top ? lift : f64vec3{0.0}),
                                                    .rotation = node.rotation,
                                                    .scale    = size,
                                                    .parent   = top ? root : nodes[*node.parent],
                                                });
            nodes[index] = entity;

            if (!node.mesh.has_value())
            {
                continue;
            }

            // One entity per part, as an entity draws one mesh. Scale is not
            // inherited, so each repeats the node's.
            for (ModelPart const& part : model.meshes[*node.mesh].parts)
            {
                entt::entity const drawn = registry.create();
                registry.emplace<Transform>(drawn, Transform{.scale = size, .parent = entity});
                registry.emplace<Renderable>(drawn, Renderable{
                                                        .mesh      = part.mesh,
                                                        .material  = part.material,
                                                        .albedo    = part.albedo,
                                                        .roughness = part.roughness,
                                                        .emissive  = part.emissive * spawn.luminance,
                                                        .metallic  = part.metallic,
                                                    });
            }
        }
    }

    void Scene::build_test_planet(AssetManager& assets, u32 terrain_lod)
    {
        registry.clear();
        origin_ = kWorldOrigin;
        ground_.reset();

        MeshHandle cube;
        MeshHandle sphere;
        {
            MeshData data;
            build_cube(data.vertices, data.indices);
            cube = assets.add_mesh("cube", std::move(data));

            data = MeshData{};
            build_sphere(data.vertices, data.indices, kSphereSlices, kSphereStacks);
            sphere = assets.add_mesh("sphere", std::move(data));
        }

        Materials const materials = load_materials(assets);

        ev100 = 14.0f;

        // Local frame at the pole: +Y is up, the ground is y = 0. The Earth
        // is terrain, meshed by terrain::TerrainOctree about its entity's
        // origin, so the entity sits at the centre, a radius below the pole
        // of the sphere. The terrain's surface there is wherever its height
        // puts it, so the object field moves to stand on the ground directly
        // below the pole, as the finest LOD meshes it. Ground albedo is a
        // guess at the terrain's average colour.
        {
            auto terrain = std::make_shared<terrain::BodyTerrain>(terrain::example_planet(kTerrainSeed));

            // Everything placed from here on is also stood on the ground under
            // its own x and z (grounded), since the ground is not level.
            f64vec3 const pole{0.0, kEarthRadius, 0.0};
            f64vec3 const above{0.0, kEarthRadius + terrain::height_bound(*terrain) + 100.0, 0.0};
            auto          probe = std::make_shared<terrain::GroundProbe const>(*terrain, terrain_lod);
            if (optional<f64vec3> const ground = probe->below(above))
            {
                origin_      = kWorldOrigin + (*ground - pole);
                ground_      = std::move(probe);
                ground_pole_ = *ground;
                log::info("scene: ground at the pole is %.1f m from the sphere; the object field moves to it",
                          ground->y - pole.y);
            }
            else
            {
                log::warn("scene: no terrain found below the pole; the object field stays on the sphere");
            }

            entt::entity const earth = registry.create();
            registry.emplace<Transform>(earth, Transform{.position = kWorldOrigin + f64vec3{0.0, -kEarthRadius, 0.0}});
            registry.emplace<Body>(earth, Body{
                                              .radius        = kEarthRadius,
                                              .centre        = f64vec3{0.0},
                                              .ground_albedo = f32vec3{0.25f},
                                              .sky_fill      = 0.1f,
                                          });
            registry.emplace<terrain::PlanetTerrain>(earth, terrain::PlanetTerrain{.terrain = std::move(terrain)});
            registry.emplace<Atmosphere>(earth);
        }

        // The Moon, straight up at its real distance from the Earth's centre.
        // Half a degree across: a few pixels. Its environment is its own grey
        // regolith, for anyone who flies there.
        {
            f32vec3 const regolith{0.36f, 0.35f, 0.33f};
            // Placed from the sphere's pole, not the moved object field.
            entt::entity const moon =
                add(sphere, kWorldOrigin - origin_ + f64vec3{0.0, kEarthMoonDistance - kEarthRadius, 0.0},
                    f64vec3{2.0 * kMoonRadius}, regolith, 0.95f, 0.0f);
            registry.emplace<Body>(moon, Body{
                                             .radius        = kMoonRadius,
                                             .centre        = f64vec3{0.0},
                                             .ground_albedo = srgb_to_linear(regolith),
                                             .sky_fill      = 0.1f,
                                         });
        }

        // The Sun, not drawn: only its light is.
        {
            f64vec3 const direction{std::cos(kSunElevation) * std::cos(kSunAzimuth),
                                    std::sin(kSunElevation),
                                    std::cos(kSunElevation) * std::sin(kSunAzimuth)};
            entt::entity const sun = registry.create();
            registry.emplace<Transform>(
                sun, Transform{.position = origin_ + direction * kAstronomicalUnit});
            registry.emplace<Star>(sun, Star{
                                            .luminous_intensity = kSunIntensity,
                                            .colour             = f32vec3{1.0f, 0.97f, 0.93f},
                                        });
        }

        // Textured and flat objects side by side: the colonnade, the spheres
        // but one, the showpiece and the lamps keep flat materials.
        f32vec3 const slate{0.30f, 0.33f, 0.38f};
        f32vec3 const white{0.85f, 0.85f, 0.85f};

        // A tower: its shadow runs ~50 m across the field at this sun angle,
        // through every cascade.
        add(cube, {-9.0, 6.0, -7.0}, {1.6, 12.0, 1.6}, materials.concrete);

        // A colonnade: striped shadows, and fine detail for the near cascade.
        assembly_lift_ = lowest_lift({f64vec2{-6.0, 6.0}, f64vec2{5.2, 6.0}});
        for (i32 index = 0; index < 8; ++index)
        {
            f64 const x = -6.0 + 1.6 * static_cast<f64>(index);
            add(cube, {x, 1.5, 6.0}, {0.35, 3.0, 0.35}, white, 0.6f, 0.0f);
        }
        add(cube, {-0.4, 3.15, 6.0}, {12.0, 0.3, 0.8}, white, 0.6f, 0.0f);

        // A gateway.
        assembly_lift_ = lowest_lift({f64vec2{7.0, -3.0}, f64vec2{7.0, 1.0}});
        add(cube, {7.0, 2.0, -3.0}, {0.8, 4.0, 0.8}, materials.metal_plates);
        add(cube, {7.0, 2.0, 1.0}, {0.8, 4.0, 0.8}, materials.metal_plates);
        add(cube, {7.0, 4.3, -1.0}, {1.0, 0.6, 5.0}, materials.metal_plates);
        assembly_lift_.reset();

        // A table: a slab on legs, whose underside only a spot can light.
        f64vec3 const table{-3.0, 1.0, -2.0};
        f64 const     table_thickness = 0.12;
        assembly_lift_ = lowest_lift({f64vec2{-4.3, -2.75}, f64vec2{-4.3, -1.25},
                                      f64vec2{-1.7, -2.75}, f64vec2{-1.7, -1.25}});
        add(cube, table, {3.0, table_thickness, 1.8}, materials.planks);
        for (f64 const x : {-4.3, -1.7})
        {
            for (f64 const z : {-2.75, -1.25})
            {
                add(cube, {x, 0.47, z}, {0.12, 0.94, 0.12}, materials.planks);
            }
        }

        // The glTF sample helmet, life-size, resting on the table and
        // turned three-quarters toward the camera's starting point. glTF
        // models face +Z. If it fails to load the table stays empty.
        {
            constexpr f64 kHelmetWidth = 0.32;   // metres, across its widest axis
            constexpr f64 kHelmetYaw   = 0.9;    // radians about +Y

            // The visor's glow, in cd/m^2 for an emissive factor of 1:
            // bright enough to read in daylight, well under the lamp heads.
            constexpr f32 kVisorLuminance = 2.0e4f;

            f64vec3 const top{table.x, table.y + table_thickness * 0.5, table.z};
            spawn(origin_ + grounded(top), glm::angleAxis(kHelmetYaw, f64vec3{0.0, 1.0, 0.0}),
                  ModelSpawn{
                      .model = assets.load_model(asset_path("models/DamagedHelmet/DamagedHelmet.glb")),
                      .fit          = kHelmetWidth,
                      .rest_on_root = true,
                      .luminance    = kVisorLuminance,
                  });
        }

        // Crates, the small one stacked across the other two.
        assembly_lift_ = lowest_lift({f64vec2{2.0, -6.0}, f64vec2{3.1, -6.4}});
        add(cube, {2.0, 0.5, -6.0}, {1.0, 1.0, 1.0}, materials.rusted_metal);
        add(cube, {3.1, 0.4, -6.4}, {0.8, 0.8, 0.8}, materials.painted_metal);
        add(cube, {2.5, 1.3, -6.1}, {0.6, 0.6, 0.6}, materials.painted_metal);
        assembly_lift_.reset();
        add(cube, {-6.0, 0.75, 1.5}, {1.5, 1.5, 1.5}, materials.concrete);
        add(cube, {11.0, 1.0, 5.0}, {2.0, 2.0, 3.0}, materials.metal_plates);

        // Spheres, rough to mirror, and one textured to show the sphere's
        // mapping.
        add(sphere, {0.0, 1.0, 0.0}, f64vec3{2.0}, {0.92f, 0.78f, 0.52f}, 0.2f, 1.0f);
        add(sphere, {-2.0, 0.4, 2.5}, f64vec3{0.8}, {0.8f, 0.2f, 0.15f}, 0.5f, 0.0f);
        add(sphere, {4.0, 0.6, 2.0}, f64vec3{1.2}, white, 0.1f, 0.0f);
        add(sphere, {-11.0, 2.5, 3.0}, f64vec3{5.0}, materials.concrete);
        add(sphere, {5.5, 0.3, -8.0}, f64vec3{0.6}, {0.2f, 0.5f, 0.9f}, 0.3f, 0.0f);

        // A spinning showpiece on a plinth, so some shadow moves.
        add(cube, {-4.0, 0.5, -9.0}, {1.2, 1.0, 1.2}, materials.concrete);
        {
            entt::entity const spinner = add(cube, {-4.0, 2.2, -9.0}, f64vec3{1.2},
                                             {0.92f, 0.78f, 0.52f}, 0.25f, 1.0f);
            registry.emplace<Spin>(spinner, Spin{.rate = 0.5, .axis = f64vec3{0.35, 1.0, 0.15}});
        }

        // Spot masts: four in the field, each aimed at something, and two far
        // out whose shadows only switch on as the camera comes within range.
        struct Mast
        {
            f64vec3 base;
            f64vec3 target;
            f32vec3 colour_srgb;
            f64     shadow_range;
        };

        Mast const masts[]{
            {{-1.0, 0.0, -12.0}, {-3.0, 0.0, -2.0}, {1.0f, 0.95f, 0.85f}, 40.0},
            {{10.0, 0.0, 9.0}, {3.0, 0.0, 5.0}, {0.55f, 0.75f, 1.0f}, 40.0},
            {{-12.0, 0.0, 10.0}, {-5.0, 0.0, 5.0}, {1.0f, 0.7f, 0.35f}, 40.0},
            {{12.0, 0.0, -9.0}, {6.0, 0.0, -2.0}, {1.0f, 0.95f, 0.85f}, 40.0},
            {{40.0, 0.0, -30.0}, {30.0, 0.0, -22.0}, {1.0f, 0.4f, 0.3f}, 35.0},
            {{-38.0, 0.0, -32.0}, {-28.0, 0.0, -24.0}, {0.4f, 1.0f, 0.5f}, 35.0},
        };

        constexpr f64 kMastHeight = 8.0;

        for (Mast const& mast : masts)
        {
            add(cube, mast.base + f64vec3{0.0, kMastHeight * 0.5, 0.0},
                {0.2, kMastHeight, 0.2}, materials.rusted_metal);

            f64vec3 const head = mast.base + f64vec3{0.0, kMastHeight + 0.3, 0.0};
            f32vec3 const tint = srgb_to_linear(mast.colour_srgb);
            entt::entity const lamp = add(cube, head, f64vec3{0.5, 0.4, 0.5},
                                          {0.1f, 0.1f, 0.1f}, 0.5f, 0.0f, tint * 2.0e5f);

            // Under the lamp head, which is unrotated, so directions in its
            // frame are the scene's: 5 cm below its bottom face and 5 cm
            // above the pole's top, shifted 0.2 m toward the target so the
            // pole is behind it. On the pole's axis the light sat in the
            // plane of its top face, which the spot map saw edge-on and
            // clamped onto the near plane, cutting the pool in half.
            f64vec3 toward = mast.target - head;
            toward.y       = 0.0;
            f64vec3 const hang = glm::normalize(toward) * 0.2 + f64vec3{0.0, -0.25, 0.0};
            add_light(lamp, hang, grounded(mast.target) - (grounded(head) + hang),
                      Light{
                          .colour       = tint,
                          .intensity    = 2.0e6f,
                          .radius       = 30.0f,
                          .cos_inner    = cos_degrees(20.0),
                          .cos_outer    = cos_degrees(30.0),
                          .casts_shadow = true,
                          .shadow_range = mast.shadow_range,
                      });
        }

        // Low coloured lamps in a ring, for the clusters to sort. Dim against
        // the sun; they show in the shade.
        f32vec3 const lamp_colours[]{
            srgb_to_linear(f32vec3{1.0f, 0.12f, 0.08f}),
            srgb_to_linear(f32vec3{0.15f, 0.85f, 1.0f}),
            srgb_to_linear(f32vec3{1.0f, 0.8f, 0.3f}),
        };

        constexpr u32 kLamps = 24;
        for (u32 index = 0; index < kLamps; ++index)
        {
            f64 const angle  = 2.0 * kPi * static_cast<f64>(index) / static_cast<f64>(kLamps);
            f64 const ring   = index % 2 == 0 ? 14.0 : 18.0;
            f64vec3 const at{ring * std::cos(angle), 0.0, ring * std::sin(angle)};
            f32vec3 const colour = lamp_colours[index % 3];

            add(cube, at + f64vec3{0.0, 0.4, 0.0}, {0.1, 0.8, 0.1}, slate, 0.5f, 0.6f);
            entt::entity const bulb = add(sphere, at + f64vec3{0.0, 0.9, 0.0},
                                          f64vec3{0.2}, {0.1f, 0.1f, 0.1f}, 0.5f, 0.0f,
                                          colour * 1.0e5f);

            // A point light has no facing; any direction will do.
            add_light(bulb, f64vec3{0.0}, f64vec3{0.0, -1.0, 0.0},
                      Light{.colour = colour, .intensity = 8000.0f, .radius = 7.0f});
        }

        // Standing 16 m from the pole at eye height, facing across the field.
        f64 const facing = 0.25;
        camera           = registry.create();
        registry.emplace<Transform>(
            camera, Transform{
                        .position = origin_ + grounded(f64vec3{16.0 * std::sin(facing), 1.7,
                                                               16.0 * std::cos(facing)}),
                        .rotation = glm::angleAxis(facing, f64vec3{0.0, 1.0, 0.0}) *
                                    glm::angleAxis(-0.08, f64vec3{1.0, 0.0, 0.0}),
                    });
        registry.emplace<Camera>(camera);
    }

    void Scene::update(f64 seconds, AssetManager const& assets)
    {
        // Collected first: instantiating creates entities and removes the
        // component being iterated.
        vector<entt::entity> settled;
        for (auto const [root, pending] : registry.view<ModelSpawn const>().each())
        {
            ModelAsset const* const model = assets.model(pending.model);
            if (model == nullptr || model->state != AssetState::Loading)
            {
                settled.push_back(root);
            }
        }

        for (entt::entity const root : settled)
        {
            ModelSpawn const        pending = registry.get<ModelSpawn>(root);
            ModelAsset const* const model   = assets.model(pending.model);
            registry.remove<ModelSpawn>(root);

            if (model != nullptr && model->model.has_value())
            {
                instantiate(root, *model->model, pending);
            }
            else
            {
                log::warn("scene: %s did not load; its root stays empty",
                          model != nullptr ? model->name.c_str() : "a model");
            }
        }

        registry.view<Transform, Spin const>().each([seconds](Transform& transform, Spin const& spin) {
            transform.rotation = glm::angleAxis(seconds * spin.rate, glm::normalize(spin.axis));
        });

        // Last frame's transforms, for motion vectors, are the renderer's to
        // keep, the camera's included.
        propagate_transforms(registry);
    }
}
