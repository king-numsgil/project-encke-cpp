#pragma once

#include "assets/model.hpp"
#include "render/camera.hpp"
#include "render/components.hpp"
#include "world/bodies.hpp"
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

    namespace terrain
    {
        class GroundProbe;
    }

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
        // while it draws. The Earth is a terrain::PlanetTerrain body for
        // terrain::TerrainOctree to mesh. The objects are stood on its ground
        // as `terrain_lod`, the finest the octree meshes, has it.
        void build_test_planet(AssetManager& assets, u32 terrain_lod);

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

        // World position the scene is laid out around: the terrain below the
        // test planet's pole. Objects are also stood on the ground under
        // their own positions, so y = 0 here is the ground only at the origin.
        f64vec3 origin() const { return origin_; }

        // Every object, light, camera, star and body is an entity: a
        // Transform, plus a Renderable or a Light (render/components.hpp), a
        // Camera (render/camera.hpp), or a Star or a Body
        // (world/bodies.hpp).
        entt::registry registry;

        // The camera the frame is drawn from: an entity with a Transform and
        // a Camera. Anything that moves it does so through its Transform,
        // before update() composes world transforms, or it draws a frame
        // late.
        entt::entity camera = entt::null;

        // Exposure as EV100: the fixed value when auto-exposure is off, and
        // where it starts when on. The scene is lit by a real-magnitude sun,
        // so it is set for daylight.
        f32 ev100 = 14.0f;

    private:
        // A mesh at `position` from the pole, flat material.
        entt::entity add(MeshHandle mesh, f64vec3 position, f64vec3 scale, f32vec3 albedo_srgb,
                         f32 roughness, f32 metallic, f32vec3 emissive = f32vec3{0.0f});

        // Every factor 1, so the maps are the material. Metalness comes from
        // the map too: a set without one is a dielectric.
        entt::entity add(MeshHandle mesh, f64vec3 position, f64vec3 scale, MaterialHandle material);

        // The nodes of `model` under `root`, as `spawn` asks.
        void instantiate(entt::entity root, Model const& model, ModelSpawn const& spawn);

        // `position`, from the pole, raised or lowered so what was on the
        // level ground at y = 0 stands on the terrain mesh under its x and z,
        // or by assembly_lift_ while one is being built. Unchanged where
        // there is no mesh to stand on.
        f64vec3 grounded(f64vec3 const& position) const;

        // How far the ground under (x, z) is above the origin's; 0 with no
        // mesh there.
        f64 ground_lift(f64 x, f64 z) const;

        // The lowest ground_lift under an assembly's supports. Every part of
        // it takes this one offset, so it stays in one piece; on sloping
        // ground its high side sinks a little rather than anything floating.
        f64 lowest_lift(std::initializer_list<f64vec2> supports) const;

        // A light at `position` relative to `parent`'s frame, facing
        // `direction` in that frame.
        entt::entity add_light(entt::entity parent, f64vec3 position, f64vec3 direction,
                               Light const& light);

        f64vec3 origin_{0.0};

        // The Earth's ground at the finest LOD, and the point on it below the
        // pole, in the Earth's frame: what grounded() stands things on.
        std::shared_ptr<terrain::GroundProbe const> ground_;
        f64vec3                                     ground_pole_{0.0};
        optional<f64>                               assembly_lift_;
    };
}
