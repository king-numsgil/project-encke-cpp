#include "core/pch.hpp"

#include "render/camera.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace encke
{
    f64mat4 Camera::view() const
    {
        // Inverse of translate(position) * rotate(orientation). The rotation is
        // orthonormal, so its inverse is its transpose.
        f64mat4 const rotation = glm::transpose(glm::mat4_cast(orientation));
        return rotation * glm::translate(f64mat4{1.0}, -position);
    }

    f64mat4 Camera::projection(f64 aspect) const
    {
        // Right-handed view space looking down -Z. Clip z is the constant near
        // distance and clip w is the view distance, so NDC depth is near / d:
        // 1.0 at the near plane, approaching 0.0 at infinity.
        f64 const focal = 1.0 / std::tan(vertical_fov * 0.5);

        f64mat4 m{0.0};
        m[0][0] = focal / aspect;
        m[1][1] = focal;
        m[2][3] = -1.0;
        m[3][2] = near_plane;
        return m;
    }
}
