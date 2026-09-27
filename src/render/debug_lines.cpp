#include "core/pch.hpp"

#include "render/debug_lines.hpp"

#include <algorithm>
#include <cmath>

namespace encke
{
    namespace
    {
        // Mirrors kLodColours in shaders/debug_draw.slang.
        array<f32vec3, 8> const kLodColours{{
            f32vec3{1.00f, 0.35f, 0.30f}, f32vec3{1.00f, 0.65f, 0.20f}, f32vec3{0.95f, 0.95f, 0.30f},
            f32vec3{0.40f, 0.95f, 0.35f}, f32vec3{0.30f, 0.90f, 0.90f}, f32vec3{0.35f, 0.55f, 1.00f},
            f32vec3{0.70f, 0.45f, 1.00f}, f32vec3{1.00f, 0.45f, 0.85f},
        }};
    }

    void DebugLines::line(f64vec3 const& a, f64vec3 const& b, u32 colour)
    {
        lines_.push_back(Line{.a = a, .b = b, .colour = colour});
    }

    void DebugLines::triangle(f64vec3 const& a, f64vec3 const& b, f64vec3 const& c, u32 colour)
    {
        line(a, b, colour);
        line(b, c, colour);
        line(c, a, colour);
    }

    void DebugLines::box(f64vec3 const& centre, f64quat const& rotation, f64vec3 const& half, u32 colour)
    {
        // Corner i has its x, y and z at +half where bits 0, 1 and 2 are set.
        array<f64vec3, 8> corners{};
        for (u32 i = 0; i < 8; ++i)
        {
            f64vec3 const sign{(i & 1u) != 0u ? 1.0 : -1.0, (i & 2u) != 0u ? 1.0 : -1.0,
                               (i & 4u) != 0u ? 1.0 : -1.0};
            corners[i] = centre + rotation * (sign * half);
        }

        // Every pair differing in one bit is an edge.
        for (u32 i = 0; i < 8; ++i)
        {
            for (u32 const bit : {1u, 2u, 4u})
            {
                if ((i & bit) == 0u)
                {
                    line(corners[i], corners[i | bit], colour);
                }
            }
        }
    }

    u32 rgb(f32vec3 const& srgb)
    {
        auto const byte = [](f32 c) { return static_cast<u32>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); };
        return byte(srgb.r) | byte(srgb.g) << 8u | byte(srgb.b) << 16u | 0xFF000000u;
    }

    u32 lod_colour(f64 voxel_size)
    {
        auto const n = static_cast<i64>(std::lround(std::log2(voxel_size)));
        return rgb(kLodColours[static_cast<size_t>((n % 8 + 8) % 8)]);
    }
}
