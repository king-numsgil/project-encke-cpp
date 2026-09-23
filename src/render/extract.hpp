#pragma once

#include "render/camera.hpp"
#include "render/components.hpp"
#include "world/bodies.hpp"

namespace encke
{
    class AssetManager;
    class Scene;

    // The boundary between the scene and the renderer. Once a frame the
    // renderer copies what it draws out of the registry into flat arrays,
    // and reads nothing of the registry after that. World space and f64,
    // like the scene: nothing is narrowed until the renderer has composed
    // it with the view.

    struct RenderObject
    {
        entt::entity entity = entt::null;   // keys the renderer's own per-object state
        f64mat4      model{1.0};            // world
        f32vec3      scale{1.0f};           // the entity's, for texture stretching

        // World bounding sphere, for shadow caster culling: around the
        // mesh's box, so exact for the cube at any scale and loose for the
        // sphere.
        f64vec3 bounds_centre{0.0};
        f64     bounds_radius = 0.0;

        Renderable renderable;
    };

    struct RenderLight
    {
        f64vec3 position{0.0};                 // world
        f64vec3 direction{0.0, 0.0, -1.0};     // world, unit; the entity's -Z
        Light   light;
    };

    // Everything the renderer reads of a scene in one frame. An object's
    // index is its GPU object slot, a light's its GPU light slot, for this
    // frame only.
    struct RenderList
    {
        CameraView           camera;
        vector<RenderObject> objects;
        vector<RenderLight>  lights;

        // At the camera: the brightest star, and the nearest body's
        // environment. Nullopt when the scene has none, and then nothing
        // lights the frame from outside it.
        optional<Starlight>    star;
        optional<Surroundings> surroundings;
    };

    // Replaces `list` with the scene's active camera, what lights it from
    // outside, and its Renderables and Lights, in registry order, capped at config::kMaxObjects and
    // kMaxLights: the excess is dropped, and logged once. A scene without a
    // usable camera keeps the last one, logged once. Reads WorldTransform,
    // so call it after Scene::update. Reuse one list, and its storage is
    // reused too.
    void extract(Scene const& scene, AssetManager const& assets, RenderList& list);
}
