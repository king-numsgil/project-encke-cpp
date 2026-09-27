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

        constexpr f64 kPi = 3.14159265358979323846;

        // The site, in the Earth's frame: an F3 pose on rolling sand and
        // gravel 930 km from the north pole, 81.6 degrees north, facing
        // across the ground the field stands on. It was taken from the
        // pole's ground, 72 m above the sphere, which is added back here.
        // The start camera stands where it stood and faces the way it faced.
        f64vec3 const kSiteEye{754'145.8628, 6'371'072.0 - 67'217.7477, 532'417.6535};
        f64vec3 const kSiteHeading{-1.3984, 1.8591, -9.7257};

        // The start camera, in the scene's frame: this far from the middle of
        // the field, turned this far about +Y from facing -Z.
        constexpr f64 kStartDistance = 16.0;
        constexpr f64 kStartFacing   = 0.25;

        // The floor the field stands on, flattened into the ground: level
        // across its radius, easing back to the terrain over the blend, cut
        // and filled up to its reach above and below. The far masts stand
        // 50 m out.
        constexpr f64 kFloorRadius = 64.0;
        constexpr f64 kFloorBlend  = 48.0;
        constexpr f64 kFloorReach  = 100.0;

        // The Sun: 3.75e28 lm spread over 4 pi sr, which gives ~1.3e5 lux at
        // 1 AU. Its direction from the Earth, fixed: 12 degrees north of the
        // equator, turned kSunLongitude about the axis from +X toward +Z.
        // It stands about 20 degrees up over the site, low enough that
        // shadows cross every cascade.
        constexpr f64 kSunIntensity   = 2.98e27;
        constexpr f64 kSunDeclination = 0.21;   // radians
        constexpr f64 kSunLongitude   = 0.6;

        // The ecliptic's tilt to the equator, and the Moon's orbit's to the
        // ecliptic.
        constexpr f64 kObliquity       = 23.44 * kPi / 180.0;
        constexpr f64 kMoonInclination = 5.145 * kPi / 180.0;

        // The Moon's direction from the Earth's centre, with the Sun's.
        // Where the Sun stands on the ecliptic follows from its declination
        // (the spring one of the two that fit); the Moon is at first quarter,
        // 90 degrees east of it along the ecliptic, and at the north of its
        // orbit. Its lit half faces the Sun, so it shows as a half moon.
        f64vec3 moon_direction(f64vec3 const& sun)
        {
            f64vec3 const north{0.0, 1.0, 0.0};
            f64 const     sun_longitude = std::asin(sun.y / std::sin(kObliquity));
            f64 const     sun_ascension = std::atan2(std::cos(kObliquity) * std::sin(sun_longitude),
                                                     std::cos(sun_longitude));

            // The equator's axes: the equinox, the Sun's meridian turned back
            // west by its right ascension, and east of it. The Earth turns
            // east, a positive turn about north.
            f64vec3 const meridian = glm::normalize(sun - north * sun.y);
            f64vec3 const equinox  = glm::angleAxis(-sun_ascension, north) * meridian;
            f64vec3 const east     = glm::cross(north, equinox);

            f64 const longitude = sun_longitude + 0.5 * kPi;
            f64 const latitude  = kMoonInclination;
            f64 const x = std::cos(latitude) * std::cos(longitude);
            f64 const y = std::cos(latitude) * std::sin(longitude) * std::cos(kObliquity) -
                          std::sin(latitude) * std::sin(kObliquity);
            f64 const z = std::cos(latitude) * std::sin(longitude) * std::sin(kObliquity) +
                          std::sin(latitude) * std::cos(kObliquity);
            return equinox * x + east * y + north * z;
        }

        // Degrees above the horizon at a point whose up is `up`.
        f64 elevation_degrees(f64vec3 const& direction, f64vec3 const& up)
        {
            return std::asin(glm::dot(glm::normalize(direction), up)) * 180.0 / kPi;
        }

        // The ground over a floor-sized disk: its mean height along `up` from
        // `middle`, which is where the floor goes so that it cuts about as
        // much as it fills, and the most it strays from that out to the
        // blend's edge.
        struct Level
        {
            f64 mean   = 0.0;
            f64 spread = 0.0;
        };

        Level level_ground(terrain::GroundProbe const& probe, f64vec3 const& middle, f64quat const& frame)
        {
            f64vec3 const up = frame * f64vec3{0.0, 1.0, 0.0};

            // Rings a quarter of the radius apart, out to the blend's edge.
            vector<f64> inside;
            vector<f64> all;
            for (u32 ring = 0; static_cast<f64>(ring) * 0.25 * kFloorRadius <= kFloorRadius + kFloorBlend; ++ring)
            {
                f64 const radius = static_cast<f64>(ring) * 0.25 * kFloorRadius;
                u32 const around = ring == 0 ? 1 : 16;
                for (u32 index = 0; index < around; ++index)
                {
                    f64 const     angle  = 2.0 * kPi * static_cast<f64>(index) / static_cast<f64>(around);
                    f64vec3 const offset = frame * f64vec3{radius * std::cos(angle), 0.0, radius * std::sin(angle)};
                    optional<f64vec3> const hit = probe.hit(middle + offset + up * 1'000.0, -up);
                    if (!hit.has_value())
                    {
                        continue;
                    }
                    f64 const height = glm::dot(*hit - middle, up);
                    all.push_back(height);
                    if (radius <= kFloorRadius)
                    {
                        inside.push_back(height);
                    }
                }
            }

            Level level;
            for (f64 const height : inside)
            {
                level.mean += height / static_cast<f64>(inside.size());
            }
            for (f64 const height : all)
            {
                level.spread = std::max(level.spread, std::abs(height - level.mean));
            }
            return level;
        }

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
                                                .position = to_world(position + f64vec3{0.0, lift, 0.0}),
                                                .rotation = rotation_,
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
        f64vec3 const           up   = rotation_ * f64vec3{0.0, 1.0, 0.0};
        f64vec3 const           from = ground_origin_ + rotation_ * f64vec3{x, 1'000.0, z};
        optional<f64vec3> const hit  = ground_->hit(from, -up);
        return hit.has_value() ? glm::dot(*hit - ground_origin_, up) : 0.0;
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
        cube_mesh      = cube;
        sphere_mesh    = sphere;
        crate_material = materials.planks;

        ev100 = 14.0f;

        // The Earth is terrain, meshed by terrain::TerrainOctree about its
        // entity's origin, so the entity sits at the centre, a radius below
        // kWorldOrigin, the pole of the sphere. The scene's frame stands on
        // the site: +Y is the ground's up there, and the field is laid out on
        // a floor flattened into the terrain, level at the ground's mean
        // height, as the finest LOD meshes it. Ground albedo is a guess at
        // the terrain's average colour.
        f64vec3 const earth_centre = kWorldOrigin + f64vec3{0.0, -kEarthRadius, 0.0};
        {
            terrain::BodyTerrain terrain = terrain::example_planet(kTerrainSeed);

            // The frame: +Y up, and turned so that the start camera, kStartFacing
            // about +Y and kStartDistance out, faces along the site's heading.
            f64vec3 const up      = glm::normalize(kSiteEye);
            f64vec3 const heading = glm::normalize(kSiteHeading - up * glm::dot(kSiteHeading, up));
            rotation_             = look_rotation(heading, up) * glm::angleAxis(-kStartFacing, f64vec3{0.0, 1.0, 0.0});

            f64vec3 middle = kSiteEye + heading * kStartDistance;
            {
                terrain::GroundProbe const plain{terrain, terrain_lod};
                Level const                level = level_ground(plain, middle, rotation_);
                middle += up * level.mean;
                log::info("scene: the floor cuts and fills up to %.1f m", level.spread);
            }
            terrain.modifiers.flattens.push_back(terrain::Flatten{
                .centre = middle,
                .up     = up,
                .radius = kFloorRadius,
                .blend  = kFloorBlend,
                .reach  = kFloorReach,
            });

            // Everything placed from here on is also stood on the ground under
            // its own x and z (grounded), which is level on the floor.
            origin_        = earth_centre + middle;
            ground_origin_ = middle;
            ground_        = std::make_shared<terrain::GroundProbe const>(terrain, terrain_lod);

            // What the ground's materials start from there, for tuning, and
            // how steep the blend made the field, which the octree's cull
            // trusts to stay under kTerrainCullFactor.
            {
                terrain::TerrainSampler sampler{terrain};
                terrain::TerrainSampler plain{terrain::example_planet(kTerrainSeed)};
                u32 const               octaves  = terrain::octaves_for_voxel(sampler.octaves(),
                                                                              terrain.voxel_size(terrain_lod));
                f64                     steepest = 0.0;
                f64                     unedited = 0.0;
                for (f64 const across : {0.25, 0.5, 0.75})
                {
                    for (u32 index = 0; index < 32; ++index)
                    {
                        f64 const     angle  = 2.0 * kPi * static_cast<f64>(index) / 32.0;
                        f64 const     radius = kFloorRadius + across * kFloorBlend;
                        f64vec3 const above  = middle + rotation_ * f64vec3{radius * std::cos(angle), 1'000.0,
                                                                            radius * std::sin(angle)};
                        if (optional<f64vec3> const hit = ground_->hit(above, -up))
                        {
                            f64 const voxel = terrain.voxel_size(terrain_lod);
                            steepest = std::max(steepest, static_cast<f64>(glm::length(
                                                              sampler.sample_point(*hit, voxel, octaves).gradient)));
                            unedited = std::max(unedited, static_cast<f64>(glm::length(
                                                              plain.sample_point(*hit, voxel, octaves).gradient)));
                        }
                    }
                }
                log::info("scene: the field's gradient where the floor blends reaches %.2f, %.2f unflattened",
                          steepest, unedited);

                array<f32, 1> const     zero{{0.0f}};
                array<f32, 1>           temperature{};
                array<f32, 1>           moisture{};
                sampler.sample_climate(middle, terrain.voxel_size(terrain_lod), zero, zero, zero, temperature, moisture);
                log::info("scene: climate channels at the floor: temperature %+.1f, moisture %.2f",
                          static_cast<f64>(temperature[0]), static_cast<f64>(moisture[0]));
            }

            entt::entity const earth = registry.create();
            registry.emplace<Transform>(earth, Transform{.position = earth_centre});
            registry.emplace<Body>(earth, Body{
                                              .radius        = kEarthRadius,
                                              .centre        = f64vec3{0.0},
                                              .ground_albedo   = f32vec3{0.25f},
                                              .sky_fill        = 0.1f,
                                              .surface_gravity = 9.81,
                                          });
            registry.emplace<terrain::PlanetTerrain>(
                earth, terrain::PlanetTerrain{.terrain = std::make_shared<terrain::BodyTerrain>(std::move(terrain))});
            registry.emplace<Atmosphere>(earth);
        }

        f64vec3 const sun_direction{std::cos(kSunDeclination) * std::cos(kSunLongitude),
                                    std::sin(kSunDeclination),
                                    std::cos(kSunDeclination) * std::sin(kSunLongitude)};
        f64vec3 const up = rotation_ * f64vec3{0.0, 1.0, 0.0};

        // The Moon at its real distance from the Earth's centre, where
        // moon_direction puts it. Half a degree across: a few pixels. Its
        // environment is its own grey regolith, for anyone who flies there.
        {
            f32vec3 const regolith{0.36f, 0.35f, 0.33f};
            f64vec3 const at = earth_centre + moon_direction(sun_direction) * kEarthMoonDistance;

            // Not stood on anything.
            assembly_lift_ = 0.0;
            entt::entity const moon = add(sphere, f64vec3{0.0}, f64vec3{2.0 * kMoonRadius}, regolith, 0.95f, 0.0f);
            assembly_lift_.reset();
            registry.get<Transform>(moon).position = at;
            log::info("scene: over the floor the Sun stands %.1f degrees up and the Moon %.1f",
                      elevation_degrees(sun_direction, up), elevation_degrees(at - origin_, up));

            registry.emplace<Body>(moon, Body{
                                             .radius        = kMoonRadius,
                                             .centre        = f64vec3{0.0},
                                             .ground_albedo   = srgb_to_linear(regolith),
                                             .sky_fill        = 0.1f,
                                             .surface_gravity = 1.62,
                                         });
        }

        // The Sun, not drawn: only its light is.
        {
            entt::entity const sun = registry.create();
            registry.emplace<Transform>(
                sun, Transform{.position = origin_ + sun_direction * kAstronomicalUnit});
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
            spawn(to_world(grounded(top)), rotation_ * glm::angleAxis(kHelmetYaw, f64vec3{0.0, 1.0, 0.0}),
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
            registry.emplace<Spin>(spinner, Spin{.rate = 0.5, .axis = f64vec3{0.35, 1.0, 0.15}, .base = rotation_});
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

        // Standing at the site's pose at eye height, facing across the field.
        camera = registry.create();
        registry.emplace<Transform>(
            camera, Transform{
                        .position = to_world(grounded(f64vec3{kStartDistance * std::sin(kStartFacing), 1.7,
                                                              kStartDistance * std::cos(kStartFacing)})),
                        .rotation = rotation_ * glm::angleAxis(kStartFacing, f64vec3{0.0, 1.0, 0.0}) *
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
            transform.rotation = spin.base * glm::angleAxis(seconds * spin.rate, glm::normalize(spin.axis));
        });

        // Last frame's transforms, for motion vectors, are the renderer's to
        // keep, the camera's included.
        propagate_transforms(registry);
    }
}
