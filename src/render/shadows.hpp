#pragma once

#include "render/config.hpp"
#include "render/extract.hpp"

namespace encke
{
    // One shadow map's view for this frame. World space, f64, CPU only: the
    // renderer composes these with each object's model, and with the camera's
    // inverse view for the lighting pass, before anything is narrowed.
    struct ShadowMapView
    {
        f64mat4 light_view{1.0};    // world -> light view, looking down -Z
        f64mat4 projection{1.0};    // light view -> map clip, reversed-Z

        // Cascade: half-width of the orthographic box. Spot: light range.
        f64 extent = 0.0;

        // Cascade: far view distance of the frustum slice it covers.
        f64 far_distance = 0.0;

        // Metres per texel; for a spot, per metre of distance from the light.
        f64 texel = 0.0;

        f32  strength    = 1.0f;    // fades a spot's shadow out at its range limit
        bool perspective = false;

        // Whether an object with this world bounding sphere can put anything
        // into the map. Conservative.
        bool may_cast(f64vec3 const& centre, f64 radius) const;
    };

    struct ShadowPlan
    {
        // Cascades first, nearest to farthest, then the chosen spots.
        array<ShadowMapView, config::kShadowViewCount> views{};
        u32 cascade_count = 0;
        u32 spot_count    = 0;

        // Index into the frame's RenderList::lights behind spot view
        // kCascadeCount + i.
        array<u32, config::kMaxShadowedSpots> spot_lights{};
    };

    // Fits the sun's cascades to the camera's frustum, texel-snapped so they
    // do not shimmer as the camera moves, and picks which of `lights`, the
    // frame's extracted ones, get a map this frame.
    // `to_sun` is the direction toward the star, world space, unit.
    void plan_shadows(CameraView const& camera, f64vec3 const& to_sun,
                      span<RenderLight const> lights, f64 aspect, ShadowPlan& plan);
}
