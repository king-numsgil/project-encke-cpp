#pragma once

#include "render/camera.hpp"

namespace encke
{
    // All world-space quantities are f64 and never leave the CPU as such; see
    // render/camera.hpp for why.

    struct SceneObject
    {
        f64mat4 model{1.0};
        f64mat4 previous_model{1.0};   // last frame, for motion vectors

        f32vec3 albedo{1.0f};          // linear
        f32     roughness = 0.5f;
        f32vec3 emissive{0.0f};        // linear, HDR
        f32     metallic  = 0.0f;

        // Rotation rate in rad/s about `spin_axis`; zero for static geometry.
        f64     spin_rate = 0.0;
        f64vec3 spin_axis{0.0, 1.0, 0.0};
        f64vec3 position{0.0};
        f64vec3 scale{1.0};
    };

    struct SceneLight
    {
        f64vec3 position{0.0};   // world, metres
        f32vec3 colour{1.0f};    // linear
        f32     intensity = 1.0f;  // candela
        f32     radius    = 5.0f;  // metres; influence cut to zero here
    };

    class Scene
    {
    public:
        // A ship corridor: floor, ceiling, walls, bulkhead ribs, crates, a
        // spinning showpiece, emissive ceiling panels and a few dozen lights.
        // Built far from the world origin on purpose, so any regression in
        // camera-relative rendering shows up as visible jitter.
        void build_test_corridor();

        // Advances animation and rolls this frame's transforms into last
        // frame's, which is what motion vectors are measured against.
        void update(f64 seconds);

        vector<SceneObject> objects;
        vector<SceneLight>  lights;

        Camera camera;
        Camera previous_camera;

    private:
        void add_box(f64vec3 position, f64vec3 scale, f32vec3 albedo_srgb, f32 roughness,
                     f32 metallic, f32vec3 emissive = f32vec3{0.0f});

        f64vec3 origin_{0.0};
        bool    first_update_ = true;
    };
}
