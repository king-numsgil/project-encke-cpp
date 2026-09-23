#include "core/pch.hpp"

#include "render/shadows.hpp"

#include "render/scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace encke
{
    namespace
    {
        constexpr f64 kPi = 3.14159265358979323846;

        // Extra field of view around a spot's outer cone, so PCF taps at the
        // cone's edge still land inside the map.
        constexpr f64 kSpotFovMargin = 2.0 * kPi / 180.0;
        constexpr f64 kSpotMaxFov    = 170.0 * kPi / 180.0;

        // Two unit vectors perpendicular to `forward` and each other. The hint
        // only has to be fixed and not parallel; a constant one keeps the
        // basis, and so the cascade texel grid, still from frame to frame.
        void basis(f64vec3 const& forward, f64vec3& right, f64vec3& up)
        {
            f64vec3 const hint = std::abs(forward.y) < 0.99 ? f64vec3{0.0, 1.0, 0.0}
                                                            : f64vec3{1.0, 0.0, 0.0};
            right = glm::normalize(glm::cross(forward, hint));
            up    = glm::cross(right, forward);
        }

        // Whether a view-space sphere reaches into the camera's frustum. The
        // projection is symmetric, so each side plane passes through the eye
        // with a slope of 1 / focal.
        bool sphere_in_view(f64vec3 const& centre, f64 radius, f64 focal_x, f64 focal_y)
        {
            if (centre.z - radius > 0.0)
            {
                return false;
            }

            f64 const tx = 1.0 / focal_x;
            f64 const ty = 1.0 / focal_y;
            f64 const nx = std::sqrt(1.0 + tx * tx);
            f64 const ny = std::sqrt(1.0 + ty * ty);

            return (centre.x + centre.z * tx) / nx <= radius &&
                   (-centre.x + centre.z * tx) / nx <= radius &&
                   (centre.y + centre.z * ty) / ny <= radius &&
                   (-centre.y + centre.z * ty) / ny <= radius;
        }

        void plan_cascades(CameraView const& camera, Star const& star, f64 aspect, ShadowPlan& plan)
        {
            f64vec3 const to_sun = star.direction_from(camera.position);
            f64vec3       right;
            f64vec3       up;
            basis(-to_sun, right, up);

            // Squared lateral spread of the frustum per metre of depth, to its
            // corners.
            f64 const ty = std::tan(camera.lens.vertical_fov * 0.5);
            f64 const tx = ty * aspect;
            f64 const k2 = tx * tx + ty * ty;

            f64 const near     = camera.lens.near_plane;
            f64 const far      = static_cast<f64>(config::kShadowDistance);
            f64 const lambda   = static_cast<f64>(config::kCascadeSplitLambda);
            f64 const count    = static_cast<f64>(config::kCascadeCount);
            f64 const resolution = static_cast<f64>(config::kCascadeResolution);

            auto split = [&](u32 index) {
                f64 const t = static_cast<f64>(index) / count;
                return lambda * near * std::pow(far / near, t) + (1.0 - lambda) * (near + (far - near) * t);
            };

            for (u32 cascade = 0; cascade < config::kCascadeCount; ++cascade)
            {
                f64 const n = split(cascade);
                f64 const f = split(cascade + 1);

                // The smallest sphere through the slice's near and far corners
                // has its centre on the view axis, where the two are
                // equidistant; past the far plane the far corners alone decide.
                // It depends only on n, f and the field of view, so its radius
                // is constant while the camera turns -- which a tight box is
                // not, and a changing size would make the texels swim.
                f64 const c      = std::min(f, 0.5 * (n + f) * (1.0 + k2));
                f64 const radius = std::max(std::sqrt((f - c) * (f - c) + f * f * k2),
                                            std::sqrt((c - n) * (c - n) + n * n * k2));

                f64vec3 const centre =
                    camera.position + camera.orientation * f64vec3{0.0, 0.0, -c};

                // Snap the centre to whole texels in the light's plane, against
                // a fixed world anchor. Camera-relative coordinates would move
                // the grid with the camera; this is f64 world space, so the
                // grid stays put.
                f64 const texel = 2.0 * radius / resolution;
                f64 const cx    = glm::dot(centre, right);
                f64 const cy    = glm::dot(centre, up);
                f64vec3 const snapped = centre + right * (std::floor(cx / texel) * texel - cx) +
                                        up * (std::floor(cy / texel) * texel - cy);

                f64 const back  = radius + config::kCascadeCasterMargin;
                f64 const depth = back + radius;

                ShadowMapView& view = plan.views[cascade];
                view.light_view     = glm::lookAt(snapped + to_sun * back, snapped, up);
                // Near and far swapped: reversed-Z, 1.0 at the eye.
                view.projection   = glm::orthoRH_ZO(-radius, radius, -radius, radius, depth, 0.0);
                view.extent       = radius;
                view.far_distance = f;
                view.texel        = texel;
                view.strength     = 1.0f;
                view.perspective  = false;
            }

            plan.cascade_count = config::kCascadeCount;
        }

        void plan_spots(CameraView const& camera, span<RenderLight const> lights, f64 aspect,
                        ShadowPlan& plan)
        {
            f64mat4 const view   = camera.view();
            f64mat4 const proj   = camera.projection(aspect);

            struct Candidate
            {
                f64 distance;
                u32 light;
            };

            vector<Candidate> candidates;
            for (u32 index = 0; index < lights.size(); ++index)
            {
                RenderLight const& spot  = lights[index];
                Light const&       light = spot.light;
                if (!light.casts_shadow || light.cos_outer <= -1.0f)
                {
                    continue;
                }

                f64 const distance = glm::length(spot.position - camera.position);
                if (distance > light.shadow_range)
                {
                    continue;
                }

                // A spot whose whole reach is out of view casts nothing seen.
                f64vec3 const centre = f64vec3{view * f64vec4{spot.position, 1.0}};
                if (!sphere_in_view(centre, static_cast<f64>(light.radius), proj[0][0], proj[1][1]))
                {
                    continue;
                }

                candidates.push_back({distance, index});
            }

            std::sort(candidates.begin(), candidates.end(),
                      [](Candidate const& a, Candidate const& b) { return a.distance < b.distance; });

            u32 const chosen =
                static_cast<u32>(std::min<size_t>(candidates.size(), config::kMaxShadowedSpots));

            f64 const resolution = static_cast<f64>(config::kSpotShadowResolution);
            f64 const fade       = static_cast<f64>(config::kShadowFadeFraction);

            for (u32 slot = 0; slot < chosen; ++slot)
            {
                RenderLight const& spot  = lights[candidates[slot].light];
                Light const&       light = spot.light;

                f64vec3 right;
                f64vec3 up;
                basis(spot.direction, right, up);

                f64 const half_angle = std::acos(static_cast<f64>(light.cos_outer));
                f64 const fov   = std::min(2.0 * half_angle + kSpotFovMargin, kSpotMaxFov);
                f64 const range = static_cast<f64>(light.radius);

                f64 const fade_length = std::max(fade * light.shadow_range, 1e-3);
                f64 const strength =
                    std::clamp((light.shadow_range - candidates[slot].distance) / fade_length, 0.0, 1.0);

                ShadowMapView& map = plan.views[config::kCascadeCount + slot];
                map.light_view = glm::lookAt(spot.position, spot.position + spot.direction, up);
                // Near and far swapped: reversed-Z, 1.0 at the near plane.
                map.projection   = glm::perspectiveRH_ZO(fov, 1.0, range, config::kSpotShadowNear);
                map.extent       = range;
                map.far_distance = 0.0;
                map.texel        = 2.0 * std::tan(fov * 0.5) / resolution;
                map.strength     = static_cast<f32>(strength);
                map.perspective  = true;

                plan.spot_lights[slot] = candidates[slot].light;
            }

            plan.spot_count = chosen;
        }
    }

    bool ShadowMapView::may_cast(f64vec3 const& centre, f64 radius) const
    {
        f64vec3 const p = f64vec3{light_view * f64vec4{centre, 1.0}};

        if (perspective)
        {
            // Behind the light, or beyond its reach.
            return p.z - radius <= 0.0 && glm::length(p) - radius <= extent;
        }

        // Outside the box sideways. Nearer the sun than the box is fine: depth
        // clamp flattens it onto the near plane, still casting.
        return std::abs(p.x) - radius <= extent && std::abs(p.y) - radius <= extent;
    }

    void plan_shadows(CameraView const& camera, Star const& star, span<RenderLight const> lights,
                      f64 aspect, ShadowPlan& plan)
    {
        plan_cascades(camera, star, aspect, plan);
        plan_spots(camera, lights, aspect, plan);
    }
}
