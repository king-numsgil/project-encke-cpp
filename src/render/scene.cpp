#include "core/pch.hpp"

#include "render/scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

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

        f64mat4 compose(f64vec3 const& position, f64 angle, f64vec3 const& axis,
                        f64vec3 const& scale)
        {
            f64mat4 m = glm::translate(f64mat4{1.0}, position);
            if (angle != 0.0)
            {
                m = glm::rotate(m, angle, glm::normalize(axis));
            }
            return glm::scale(m, scale);
        }

        f32 cos_degrees(f64 degrees)
        {
            return static_cast<f32>(std::cos(degrees * kPi / 180.0));
        }
    }

    SceneObject& Scene::add(MeshKind mesh, f64vec3 position, f64vec3 scale, f32vec3 albedo_srgb,
                            f32 roughness, f32 metallic, f32vec3 emissive)
    {
        SceneObject object;
        object.mesh      = static_cast<u32>(mesh);
        object.position  = origin_ + position;
        object.scale     = scale;
        object.albedo    = srgb_to_linear(albedo_srgb);
        object.roughness = roughness;
        object.metallic  = metallic;
        object.emissive  = emissive;
        object.model     = compose(object.position, 0.0, object.spin_axis, scale);
        object.previous_model = object.model;

        // Both meshes are unit-sized: the cube's half-diagonal bounds it at
        // any rotation, the sphere's largest half-axis does.
        object.bounds_centre = object.position;
        object.bounds_radius = mesh == MeshKind::Sphere
                                   ? 0.5 * std::max({scale.x, scale.y, scale.z})
                                   : 0.5 * glm::length(scale);

        objects.push_back(object);
        return objects.back();
    }

    SceneObject& Scene::add(MeshKind mesh, f64vec3 position, f64vec3 scale, MaterialKind material)
    {
        SceneObject& object = add(mesh, position, scale, f32vec3{1.0f}, 1.0f, 1.0f);
        object.material     = static_cast<u32>(material);
        return object;
    }

    void Scene::add_model(Model const& model, f64vec3 const& position, f64quat const& orientation,
                          f64 scale, f32 luminance)
    {
        f64mat4 const transform = glm::translate(f64mat4{1.0}, position) *
                                  glm::mat4_cast(orientation) *
                                  glm::scale(f64mat4{1.0}, f64vec3{scale});

        for (ModelPart const& part : model.parts)
        {
            SceneObject object;
            object.mesh           = part.mesh;
            object.material       = part.material;
            object.albedo         = part.albedo;
            object.roughness      = part.roughness;
            object.metallic       = part.metallic;
            object.emissive       = part.emissive * luminance;
            object.position       = position;
            object.scale          = f64vec3{scale};
            object.model          = transform;
            object.previous_model = transform;
            object.bounds_centre  = f64vec3{transform * f64vec4{part.bounds_centre, 1.0}};
            object.bounds_radius  = part.bounds_radius * scale;
            objects.push_back(object);
        }
    }

    void Scene::build_test_planet(Model const* helmet)
    {
        objects.clear();
        lights.clear();
        origin_       = kWorldOrigin;
        first_update_ = true;

        ev100   = 14.0f;
        // The pole's local up, which the whole field is laid out along.
        // Ground albedo is a guess at the Ground110 set's average, not
        // measured from it.
        up            = f64vec3{0.0, 1.0, 0.0};
        ground_albedo = f32vec3{0.25f};
        sky_fill      = 0.1f;

        // Local frame at the pole: +Y is up, the ground is y = 0. The planet
        // curves away by d^2 / 2R, a tenth of a millimetre at 30 m, so the
        // object field can treat it as flat.
        {
            SceneObject planet;
            planet.mesh          = static_cast<u32>(MeshKind::Planet);
            planet.material      = static_cast<u32>(MaterialKind::Ground);
            planet.position      = origin_;
            planet.albedo        = f32vec3{1.0f};
            planet.roughness     = 1.0f;
            planet.model         = glm::translate(f64mat4{1.0}, origin_);
            planet.previous_model = planet.model;
            planet.bounds_centre = origin_ - f64vec3{0.0, kEarthRadius, 0.0};
            planet.bounds_radius = kEarthRadius;
            objects.push_back(planet);
        }

        // The Moon, straight up at its real distance from the Earth's centre.
        // Half a degree across: a few pixels.
        add(MeshKind::Sphere, f64vec3{0.0, kEarthMoonDistance - kEarthRadius, 0.0},
            f64vec3{2.0 * kMoonRadius}, {0.36f, 0.35f, 0.33f}, 0.95f, 0.0f);

        {
            f64vec3 const direction{std::cos(kSunElevation) * std::cos(kSunAzimuth),
                                    std::sin(kSunElevation),
                                    std::cos(kSunElevation) * std::sin(kSunAzimuth)};
            star.position           = origin_ + direction * kAstronomicalUnit;
            star.luminous_intensity = kSunIntensity;
            star.colour             = f32vec3{1.0f, 0.97f, 0.93f};
        }

        // Textured and flat objects side by side: the colonnade, the spheres
        // but one, the showpiece and the lamps keep flat materials.
        f32vec3 const slate{0.30f, 0.33f, 0.38f};
        f32vec3 const white{0.85f, 0.85f, 0.85f};

        // A tower: its shadow runs ~50 m across the field at this sun angle,
        // through every cascade.
        add(MeshKind::Cube, {-9.0, 6.0, -7.0}, {1.6, 12.0, 1.6}, MaterialKind::Concrete);

        // A colonnade: striped shadows, and fine detail for the near cascade.
        for (i32 index = 0; index < 8; ++index)
        {
            f64 const x = -6.0 + 1.6 * static_cast<f64>(index);
            add(MeshKind::Cube, {x, 1.5, 6.0}, {0.35, 3.0, 0.35}, white, 0.6f, 0.0f);
        }
        add(MeshKind::Cube, {-0.4, 3.15, 6.0}, {12.0, 0.3, 0.8}, white, 0.6f, 0.0f);

        // A gateway.
        add(MeshKind::Cube, {7.0, 2.0, -3.0}, {0.8, 4.0, 0.8}, MaterialKind::MetalPlates);
        add(MeshKind::Cube, {7.0, 2.0, 1.0}, {0.8, 4.0, 0.8}, MaterialKind::MetalPlates);
        add(MeshKind::Cube, {7.0, 4.3, -1.0}, {1.0, 0.6, 5.0}, MaterialKind::MetalPlates);

        // A table: a slab on legs, whose underside only a spot can light.
        f64vec3 const table{-3.0, 1.0, -2.0};
        f64 const     table_thickness = 0.12;
        add(MeshKind::Cube, table, {3.0, table_thickness, 1.8}, MaterialKind::Planks);
        for (f64 const x : {-4.3, -1.7})
        {
            for (f64 const z : {-2.75, -1.25})
            {
                add(MeshKind::Cube, {x, 0.47, z}, {0.12, 0.94, 0.12}, MaterialKind::Planks);
            }
        }

        // The glTF sample helmet, life-size, resting on the table and
        // turned three-quarters toward the camera's starting point. glTF
        // models face +Z.
        if (helmet != nullptr)
        {
            constexpr f64 kHelmetWidth = 0.32;   // metres, across its widest axis
            constexpr f64 kHelmetYaw   = 0.9;    // radians about +Y

            f64vec3 const extent = helmet->max - helmet->min;
            f64 const     scale  = kHelmetWidth / std::max({extent.x, extent.y, extent.z});

            // Its lowest point on the table top: yaw leaves height alone.
            f64vec3 const at{table.x, table.y + table_thickness * 0.5 - helmet->min.y * scale,
                             table.z};

            // The visor's glow, in cd/m^2 for an emissive factor of 1:
            // bright enough to read in daylight, well under the lamp heads.
            constexpr f32 kVisorLuminance = 2.0e4f;

            add_model(*helmet, origin_ + at, glm::angleAxis(kHelmetYaw, f64vec3{0.0, 1.0, 0.0}),
                      scale, kVisorLuminance);
        }

        // Crates.
        add(MeshKind::Cube, {2.0, 0.5, -6.0}, {1.0, 1.0, 1.0}, MaterialKind::RustedMetal);
        add(MeshKind::Cube, {3.1, 0.4, -6.4}, {0.8, 0.8, 0.8}, MaterialKind::PaintedMetal);
        add(MeshKind::Cube, {2.5, 1.3, -6.1}, {0.6, 0.6, 0.6}, MaterialKind::PaintedMetal);
        add(MeshKind::Cube, {-6.0, 0.75, 1.5}, {1.5, 1.5, 1.5}, MaterialKind::Concrete);
        add(MeshKind::Cube, {11.0, 1.0, 5.0}, {2.0, 2.0, 3.0}, MaterialKind::MetalPlates);

        // Spheres, rough to mirror, and one textured to show the sphere's
        // mapping.
        add(MeshKind::Sphere, {0.0, 1.0, 0.0}, f64vec3{2.0}, {0.92f, 0.78f, 0.52f}, 0.2f, 1.0f);
        add(MeshKind::Sphere, {-2.0, 0.4, 2.5}, f64vec3{0.8}, {0.8f, 0.2f, 0.15f}, 0.5f, 0.0f);
        add(MeshKind::Sphere, {4.0, 0.6, 2.0}, f64vec3{1.2}, white, 0.1f, 0.0f);
        add(MeshKind::Sphere, {-11.0, 2.5, 3.0}, f64vec3{5.0}, MaterialKind::Concrete);
        add(MeshKind::Sphere, {5.5, 0.3, -8.0}, f64vec3{0.6}, {0.2f, 0.5f, 0.9f}, 0.3f, 0.0f);

        // A spinning showpiece on a plinth, so some shadow moves.
        add(MeshKind::Cube, {-4.0, 0.5, -9.0}, {1.2, 1.0, 1.2}, MaterialKind::Concrete);
        {
            SceneObject& spinner = add(MeshKind::Cube, {-4.0, 2.2, -9.0}, f64vec3{1.2},
                                       {0.92f, 0.78f, 0.52f}, 0.25f, 1.0f);
            spinner.spin_rate = 0.5;
            spinner.spin_axis = f64vec3{0.35, 1.0, 0.15};
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
            add(MeshKind::Cube, mast.base + f64vec3{0.0, kMastHeight * 0.5, 0.0},
                {0.2, kMastHeight, 0.2}, MaterialKind::RustedMetal);

            f64vec3 const head = mast.base + f64vec3{0.0, kMastHeight + 0.3, 0.0};
            f32vec3 const tint = srgb_to_linear(mast.colour_srgb);
            add(MeshKind::Cube, head, f64vec3{0.5, 0.4, 0.5}, {0.1f, 0.1f, 0.1f}, 0.5f, 0.0f,
                tint * 2.0e5f);

            SceneLight light;
            light.position     = origin_ + head - f64vec3{0.0, 0.3, 0.0};
            light.direction    = glm::normalize(mast.target - (head - f64vec3{0.0, 0.3, 0.0}));
            light.colour       = tint;
            light.intensity    = 2.0e6f;
            light.radius       = 30.0f;
            light.cos_inner    = cos_degrees(20.0);
            light.cos_outer    = cos_degrees(30.0);
            light.casts_shadow = true;
            light.shadow_range = mast.shadow_range;
            lights.push_back(light);
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

            add(MeshKind::Cube, at + f64vec3{0.0, 0.4, 0.0}, {0.1, 0.8, 0.1}, slate, 0.5f, 0.6f);
            add(MeshKind::Sphere, at + f64vec3{0.0, 0.9, 0.0}, f64vec3{0.2}, {0.1f, 0.1f, 0.1f},
                0.5f, 0.0f, colour * 1.0e5f);

            SceneLight light;
            light.position  = origin_ + at + f64vec3{0.0, 0.9, 0.0};
            light.colour    = colour;
            light.intensity = 8000.0f;
            light.radius    = 7.0f;
            lights.push_back(light);
        }

        // Standing 16 m from the pole at eye height, facing across the field.
        f64 const facing   = 0.25;
        camera.position    = origin_ + f64vec3{16.0 * std::sin(facing), 1.7, 16.0 * std::cos(facing)};
        camera.orientation = glm::angleAxis(facing, f64vec3{0.0, 1.0, 0.0}) *
                             glm::angleAxis(-0.08, f64vec3{1.0, 0.0, 0.0});        previous_camera    = camera;
    }

    void Scene::update(f64 seconds)
    {
        // Last frame's state becomes the motion-vector reference. On the very
        // first frame there is no last frame, so both are the same and motion
        // is zero rather than whatever the build-time transforms implied.
        if (!first_update_)
        {
            previous_camera = camera;
            for (SceneObject& object : objects)
            {
                object.previous_model = object.model;
            }
        }

        for (SceneObject& object : objects)
        {
            if (object.spin_rate != 0.0)
            {
                object.model = compose(object.position, seconds * object.spin_rate,
                                       object.spin_axis, object.scale);
            }
        }

        // The camera is not animated: the app flies it (render/fly_camera),
        // after this has rolled it into previous_camera.

        if (first_update_)
        {
            previous_camera = camera;
            for (SceneObject& object : objects)
            {
                object.previous_model = object.model;
            }
            first_update_ = false;
        }
    }
}
