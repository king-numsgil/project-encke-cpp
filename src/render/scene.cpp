#include "core/pch.hpp"

#include "render/scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace encke
{
    namespace
    {
        // A kilometre-scale offset in every axis, so the corridor sits where f32
        // world coordinates would already be jittering. Set to zero to compare:
        // the rendered image must not change.
        f64vec3 const kWorldOrigin{1'000'000.0, 250'000.0, -700'000.0};

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

        // Corridor dimensions, metres. Roughly a cramped ship passageway.
        constexpr f64 kHalfWidth = 2.0;
        constexpr f64 kHeight    = 2.8;
        constexpr f64 kLength    = 30.0;
    }

    void Scene::add_box(f64vec3 position, f64vec3 scale, f32vec3 albedo_srgb, f32 roughness,
                        f32 metallic, f32vec3 emissive)
    {
        SceneObject object;
        object.position  = origin_ + position;
        object.scale     = scale;
        object.albedo    = srgb_to_linear(albedo_srgb);
        object.roughness = roughness;
        object.metallic  = metallic;
        object.emissive  = emissive;
        object.model     = compose(object.position, 0.0, object.spin_axis, scale);
        object.previous_model = object.model;
        objects.push_back(object);
    }

    void Scene::build_test_corridor()
    {
        objects.clear();
        lights.clear();
        origin_       = kWorldOrigin;
        first_update_ = true;

        f64 const mid_z = -kLength * 0.5;

        f32vec3 const hull_grey{0.42f, 0.44f, 0.47f};
        f32vec3 const deck_grey{0.26f, 0.27f, 0.29f};
        f32vec3 const rib_dark{0.18f, 0.19f, 0.21f};

        // Shell. Slabs overlap at the edges so no seam leaks the background.
        add_box({0.0, -0.05, mid_z}, {kHalfWidth * 2.0 + 0.2, 0.1, kLength}, deck_grey, 0.55f,
                0.7f);
        add_box({0.0, kHeight + 0.05, mid_z}, {kHalfWidth * 2.0 + 0.2, 0.1, kLength}, hull_grey,
                0.7f, 0.0f);
        add_box({-kHalfWidth - 0.05, kHeight * 0.5, mid_z}, {0.1, kHeight, kLength}, hull_grey,
                0.5f, 0.0f);
        add_box({kHalfWidth + 0.05, kHeight * 0.5, mid_z}, {0.1, kHeight, kLength}, hull_grey,
                0.5f, 0.0f);
        add_box({0.0, kHeight * 0.5, 0.05}, {kHalfWidth * 2.0 + 0.2, kHeight + 0.2, 0.1},
                hull_grey, 0.5f, 0.0f);
        add_box({0.0, kHeight * 0.5, -kLength - 0.05}, {kHalfWidth * 2.0 + 0.2, kHeight + 0.2, 0.1},
                hull_grey, 0.5f, 0.0f);

        // Bulkhead ribs every 5 m.
        for (f64 z = -5.0; z > -kLength; z -= 5.0)
        {
            add_box({-kHalfWidth + 0.15, kHeight * 0.5, z}, {0.3, kHeight, 0.25}, rib_dark, 0.4f,
                    0.9f);
            add_box({kHalfWidth - 0.15, kHeight * 0.5, z}, {0.3, kHeight, 0.25}, rib_dark, 0.4f,
                    0.9f);
            add_box({0.0, kHeight - 0.15, z}, {kHalfWidth * 2.0, 0.3, 0.25}, rib_dark, 0.4f, 0.9f);
        }

        // Cargo and consoles.
        add_box({1.35, 0.4, -7.0}, {0.8, 0.8, 0.8}, {0.62f, 0.38f, 0.14f}, 0.85f, 0.0f);
        add_box({-1.45, 0.3, -12.5}, {0.6, 0.6, 1.1}, {0.36f, 0.40f, 0.22f}, 0.8f, 0.0f);
        add_box({1.3, 0.55, -17.5}, {0.9, 1.1, 0.7}, {0.30f, 0.33f, 0.38f}, 0.35f, 0.8f);
        add_box({-1.4, 0.45, -22.0}, {0.7, 0.9, 0.7}, {0.62f, 0.38f, 0.14f}, 0.85f, 0.0f);

        // Spinning showpiece: polished metal, to show off the specular lobe.
        {
            SceneObject object;
            object.position  = origin_ + f64vec3{0.0, 1.25, -9.5};
            object.scale     = f64vec3{0.6};
            object.albedo    = srgb_to_linear(f32vec3{0.92f, 0.78f, 0.52f});
            object.roughness = 0.25f;
            object.metallic  = 1.0f;
            object.spin_rate = 0.5;
            object.spin_axis = f64vec3{0.35, 1.0, 0.15};
            object.model     = compose(object.position, 0.0, object.spin_axis, object.scale);
            object.previous_model = object.model;
            objects.push_back(object);
        }

        // Ceiling panels: an emissive strip with a point light beneath each.
        f32vec3 const panel_white = srgb_to_linear(f32vec3{1.0f, 0.95f, 0.86f});
        for (f64 z = -1.0; z > -kLength; z -= 2.0)
        {
            add_box({0.0, kHeight - 0.02, z}, {1.2, 0.04, 0.4}, {0.9f, 0.9f, 0.9f}, 0.3f, 0.0f,
                    panel_white * 60.0f);

            SceneLight light;
            light.position  = origin_ + f64vec3{0.0, kHeight - 0.2, z};
            light.colour    = panel_white;
            light.intensity = 450.0f;
            light.radius    = 7.0f;
            lights.push_back(light);
        }

        // Red emergency lights low on the walls, and cyan console glow, so the
        // clusters see a mix of wide and tight lights.
        f32vec3 const emergency_red = srgb_to_linear(f32vec3{1.0f, 0.12f, 0.08f});
        f32vec3 const console_cyan  = srgb_to_linear(f32vec3{0.15f, 0.85f, 1.0f});

        for (f64 z = -3.0; z > -kLength; z -= 6.0)
        {
            for (f64 side : {-1.0, 1.0})
            {
                SceneLight light;
                light.position  = origin_ + f64vec3{side * (kHalfWidth - 0.1), 0.25, z};
                light.colour    = emergency_red;
                light.intensity = 40.0f;
                light.radius    = 2.5f;
                lights.push_back(light);
            }
        }

        for (f64 z : {-7.0, -12.5, -17.5, -22.0})
        {
            SceneLight light;
            light.position  = origin_ + f64vec3{z < -15.0 ? 0.8 : -0.9, 1.1, z + 0.6};
            light.colour    = console_cyan;
            light.intensity = 35.0f;
            light.radius    = 2.0f;
            lights.push_back(light);
        }

        camera.position    = origin_ + f64vec3{0.0, 1.7, -1.2};
        camera.orientation = f64quat{1.0, 0.0, 0.0, 0.0};
        previous_camera    = camera;
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

        // A slow dolly down the corridor with a gentle yaw, so motion vectors
        // carry both components and the clusters sweep across the geometry.
        f64 const travel = 3.0 * (1.0 - std::cos(seconds * 0.25));
        f64 const yaw    = 0.12 * std::sin(seconds * 0.4);

        camera.position    = origin_ + f64vec3{0.4 * std::sin(seconds * 0.3), 1.7, -1.2 - travel};
        camera.orientation = glm::angleAxis(yaw, f64vec3{0.0, 1.0, 0.0});

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
