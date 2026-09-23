#pragma once

#include "assets/model.hpp"
#include "render/camera.hpp"
#include "render/components.hpp"
#include "world/transform.hpp"

namespace encke
{
    // All world-space quantities are f64 and never leave the CPU as such; see
    // render/camera.hpp for why.

    inline constexpr f64 kEarthRadius       = 6'371'000.0;
    inline constexpr f64 kMoonRadius        = 1'737'400.0;
    inline constexpr f64 kEarthMoonDistance = 384'400'000.0;
    inline constexpr f64 kAstronomicalUnit  = 149'597'870'700.0;

    class AssetManager;

    // Turns an entity about `axis`, in its parent's frame, at `rate` rad/s.
    // It replaces the Transform's rotation outright: the angle is rate times
    // the scene clock, from no rotation at zero.
    struct Spin
    {
        f64     rate = 0.0;
        f64vec3 axis{0.0, 1.0, 0.0};
    };

    // On a model's root entity until the model is Ready and its nodes have
    // been spawned under it; Scene::update does that and removes this. Scale
    // is not inherited, so the model's is folded into every node's offset and
    // size then.
    struct ModelSpawn
    {
        ModelHandle model;

        // Uniform. With `fit`, worked out instead so the model's widest
        // extent measures `fit` metres.
        f64           scale = 1.0;
        optional<f64> fit;

        // Lifts the model so its lowest point, along the root's +Y, sits at
        // the root: the root is then where it stands.
        bool rest_on_root = false;

        // cd/m^2 for an emissive factor of 1.
        f32 luminance = 1.0f;
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
        // distance, and the glTF sample helmet on a table. Places the camera
        // at the edge of the field. Built far from the world origin on
        // purpose, so any regression in camera-relative rendering shows up as
        // visible jitter.
        //
        // Asks `assets` for the meshes and materials it uses; they stream in
        // while it draws.
        void build_test_planet(AssetManager& assets);

        // A root entity for `spawn.model`, at `position` (world) with
        // `orientation`, returned at once so it can be placed and parented
        // while the model loads. Its nodes appear under it in the update
        // after the model is Ready: an entity per node, parented as in the
        // file, and a child per part of a node's mesh carrying a Renderable.
        // If the model fails, the root stays empty.
        entt::entity spawn(f64vec3 const& position, f64quat const& orientation,
                           ModelSpawn const& spawn);

        // Spawns the nodes of models that became Ready, advances animation
        // to `seconds` on the scene clock, then composes every
        // WorldTransform. Read world transforms after this.
        void update(f64 seconds, AssetManager const& assets);

        // World position of the test planet's pole, which the scene is laid
        // out around.
        f64vec3 origin() const { return origin_; }

        // Away from the centre of the one body this scene has, unit: where
        // the environment's horizon lies. Lighting only; the camera has its
        // own frame. With several bodies this becomes the nearest one's, or
        // a blend, and is the scene's decision. World +Y means nothing here.
        f64vec3 up_at(f64vec3 const& position) const;

        // Every object and light is an entity: a Transform, plus a
        // Renderable or a Light (render/components.hpp).
        entt::registry registry;

        Star star;

        // Exposure as EV100: the fixed value when auto-exposure is off, and
        // where it starts when on. The scene is lit by a real-magnitude sun,
        // so it is set for daylight.
        f32 ev100 = 14.0f;

        // The environment the lighting pass is filled by and reflects, a
        // stand-in for bounce light until there is any GI: the ground below
        // the horizon, lit by the star, and the sky above it. The renderer
        // works out the ground's radiance per frame from the star, so only
        // its albedo is set here.
        //
        // An airless sky is black, which leaves anything facing up in shadow
        // black too. `sky_fill` gives the sky that fraction of the ground's
        // radiance anyway, for the light nearby objects would bounce, which
        // a two-colour environment cannot know about.
        f32vec3 ground_albedo{0.0f};   // linear
        f32     sky_fill = 0.0f;

        Camera camera;

    private:
        // A mesh at `position` from the pole, flat material.
        entt::entity add(MeshHandle mesh, f64vec3 position, f64vec3 scale, f32vec3 albedo_srgb,
                         f32 roughness, f32 metallic, f32vec3 emissive = f32vec3{0.0f});

        // Every factor 1, so the maps are the material. Metalness comes from
        // the map too: a set without one is a dielectric.
        entt::entity add(MeshHandle mesh, f64vec3 position, f64vec3 scale, MaterialHandle material);

        // The nodes of `model` under `root`, as `spawn` asks.
        void instantiate(entt::entity root, Model const& model, ModelSpawn const& spawn);

        // A light at `position` relative to `parent`'s frame, facing
        // `direction` in that frame.
        entt::entity add_light(entt::entity parent, f64vec3 position, f64vec3 direction,
                               Light const& light);

        f64vec3 origin_{0.0};
        f64vec3 planet_centre_{0.0};
    };
}
