#pragma once

#include "render/camera.hpp"
#include "render/material.hpp"
#include "render/mesh.hpp"

namespace encke
{
    // All world-space quantities are f64 and never leave the CPU as such; see
    // render/camera.hpp for why.

    inline constexpr f64 kEarthRadius       = 6'371'000.0;
    inline constexpr f64 kMoonRadius        = 1'737'400.0;
    inline constexpr f64 kEarthMoonDistance = 384'400'000.0;
    inline constexpr f64 kAstronomicalUnit  = 149'597'870'700.0;

    struct SceneObject
    {
        f64mat4 model{1.0};
        f64mat4 previous_model{1.0};   // last frame, for motion vectors

        MeshKind     mesh     = MeshKind::Cube;
        MaterialKind material = MaterialKind::None;

        // With a material these are glTF-style factors on its maps; without,
        // they are the whole material.
        f32vec3 albedo{1.0f};          // linear
        f32     roughness = 0.5f;
        f32vec3 emissive{0.0f};        // linear, HDR
        f32     metallic  = 0.0f;

        // Rotation rate in rad/s about `spin_axis`; zero for static geometry.
        f64     spin_rate = 0.0;
        f64vec3 spin_axis{0.0, 1.0, 0.0};
        f64vec3 position{0.0};
        f64vec3 scale{1.0};

        // World-space bounding sphere, for shadow caster culling. Not the
        // model origin in general: the planet's origin is its north pole.
        f64vec3 bounds_centre{0.0};
        f64     bounds_radius = 0.0;
    };

    // A point light, or a spot when cos_outer is above -1. Only spots cast
    // shadows; casts_shadow on a point light is ignored.
    struct SceneLight
    {
        f64vec3 position{0.0};         // world, metres
        f64vec3 direction{0.0, -1.0, 0.0};  // world, unit; spots only
        f32vec3 colour{1.0f};          // linear
        f32     intensity = 1.0f;      // candela
        f32     radius    = 5.0f;      // metres; influence cut to zero here

        // Cone half-angles as cosines. The defaults make a point light.
        f32 cos_inner = -1.0f;
        f32 cos_outer = -2.0f;

        // Shadowed only while the camera is within shadow_range of the light,
        // and only if it wins one of the config::kMaxShadowedSpots slots.
        bool casts_shadow = false;
        f64  shadow_range = 40.0;
    };

    // The system's star. Its direction and illuminance are worked out per
    // frame from where the camera is, so moving either -- flying across the
    // system, or the star's own motion -- needs nothing else changed.
    struct Star
    {
        f64vec3 position{0.0};             // world, metres
        f64     luminous_intensity = 0.0;  // candela
        f32vec3 colour{1.0f};              // linear tint, multiplied by the illuminance

        f64vec3 direction_from(f64vec3 const& point) const
        {
            return glm::normalize(position - point);
        }

        // Lux at `point`: inverse square, no atmosphere.
        f64 illuminance_at(f64vec3 const& point) const
        {
            f64vec3 const delta = position - point;
            return luminous_intensity / glm::dot(delta, delta);
        }
    };

    class Scene
    {
    public:
        // An Earth-sized planet whose north pole is the floor, strewn with
        // boxes, spheres and pillars, lit by a Sun low on the horizon and a
        // handful of spot and point lights, with the Moon overhead at its real
        // distance. Places the camera at the edge of the field. Built far from the world origin on purpose, so any
        // regression in camera-relative rendering shows up as visible jitter.
        void build_test_planet();

        // Advances animation and rolls this frame's transforms, camera
        // included, into last frame's, which is what motion vectors are
        // measured against. Move the camera after this, not before.
        void update(f64 seconds);

        vector<SceneObject> objects;
        vector<SceneLight>  lights;
        Star                star;

        // Exposure as EV100: the fixed value when auto-exposure is off, and
        // where it starts when on. The scene is lit by a real-magnitude sun,
        // so it is set for daylight.
        f32 ev100 = 14.0f;

        // Stand-in for bounce light until there is any GI, in the same
        // radiometric scale as the lights.
        f32vec3 ambient{0.0f};

        Camera camera;
        Camera previous_camera;

    private:
        SceneObject& add(MeshKind mesh, f64vec3 position, f64vec3 scale, f32vec3 albedo_srgb,
                         f32 roughness, f32 metallic, f32vec3 emissive = f32vec3{0.0f});

        // Every factor 1, so the maps are the material. Metalness comes from
        // the map too: a set without one is a dielectric.
        SceneObject& add(MeshKind mesh, f64vec3 position, f64vec3 scale, MaterialKind material);

        f64vec3 origin_{0.0};
        bool    first_update_ = true;
    };
}
