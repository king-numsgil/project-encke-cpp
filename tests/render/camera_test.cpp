#include "core/pch.hpp"

#include "render/camera.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace encke
{
    namespace
    {
        // NDC depth of a point `distance` metres straight ahead.
        f64 depth_at(CameraView const& camera, f64 distance)
        {
            f64vec4 const clip = camera.projection(16.0 / 9.0) * f64vec4{0.0, 0.0, -distance, 1.0};
            return clip.z / clip.w;
        }
    }

    TEST_CASE("projection is infinite reversed-Z", "[camera]")
    {
        CameraView camera;
        f64 const  near_plane = camera.lens.near_plane;

        CHECK_THAT(depth_at(camera, near_plane), WithinRel(1.0, 1e-12));
        CHECK_THAT(depth_at(camera, 2.0 * near_plane), WithinRel(0.5, 1e-12));

        // No far plane: an astronomical distance still lands inside 0..1.
        f64 const sun = depth_at(camera, 1.5e11);
        CHECK(sun > 0.0);
        CHECK(sun < 1e-12);
    }

    TEST_CASE("projection keeps +Y up; the flip belongs to the viewport", "[camera]")
    {
        CameraView    camera;
        f64vec4 const clip = camera.projection(1.0) * f64vec4{0.0, 1.0, -5.0, 1.0};
        CHECK(clip.y / clip.w > 0.0);
    }

    TEST_CASE("view cancels the camera's position in f64", "[camera]")
    {
        CameraView camera;
        camera.position = {1.0e6, 2.5e5, -7.0e5};

        // A millimetre, a thousand kilometres out: lost if anything narrowed.
        // Much further and the f64 sum below rounds it before view() runs.
        f64vec4 const point = camera.view() * f64vec4{camera.position + f64vec3{0.001, 0.0, -3.0}, 1.0};

        CHECK_THAT(point.x, WithinAbs(0.001, 1e-9));
        CHECK_THAT(point.y, WithinAbs(0.0, 1e-9));
        CHECK_THAT(point.z, WithinAbs(-3.0, 1e-9));
    }
}
