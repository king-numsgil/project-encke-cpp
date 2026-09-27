#include "core/pch.hpp"

#include "terrain/modifiers.hpp"

#include <algorithm>
#include <cmath>

namespace encke::terrain
{
    namespace
    {
        // 1 up to `inner`, 0 from `inner + width`, smooth between; exactly 0
        // past it, so a point outside changes by nothing at all.
        f64 ease_out(f64 x, f64 inner, f64 width)
        {
            f64 const t = std::clamp((x - inner) / width, 0.0, 1.0);
            return 1.0 - t * t * (3.0 - 2.0 * t);
        }
    }

    f64 Flatten::bound() const
    {
        f64 const across = radius + blend;
        f64 const along  = reach + blend;
        return std::sqrt(across * across + along * along);
    }

    f64 Flatten::apply(f64vec3 const& point, f64 value) const
    {
        f64vec3 const d      = point - centre;
        f64 const     height = glm::dot(d, up);
        f64 const     along  = std::abs(height);
        if (along >= reach + blend)
        {
            return value;
        }

        f64 const outer   = radius + blend;
        f64 const across2 = glm::dot(d, d) - height * height;
        if (across2 >= outer * outer)
        {
            return value;
        }

        f64 const across = std::sqrt(std::max(across2, 0.0));
        f64 const weight = ease_out(across, radius, blend) * ease_out(along, reach, blend);
        return value + weight * (height - value);
    }
}
