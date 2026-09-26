#include "core/pch.hpp"

#include "terrain/ground.hpp"

#include <algorithm>
#include <cmath>

namespace encke::terrain
{
    namespace
    {
        // Every tile divides the period terrain UVs are offset by. The maps'
        // own roughness averages 0.26 for Grass004 and 0.55 to 0.7 for the
        // rest, which under a low sun read as wet stone and plastic grass;
        // snow keeps the lowest floor, for its sheen.
        array<GroundSet, kGroundSetCount> const kGroundSets{{
            {"Ground110", 2.0f, {0.20f, 0.17f, 0.14f}, 0.75f},    // gravel
            {"Rock051", 4.0f, {0.25f, 0.25f, 0.23f}, 0.65f},      // rock
            {"Grass004", 2.0f, {0.07f, 0.13f, 0.03f}, 0.80f},     // grass
            {"Snow010A", 4.0f, {0.80f, 0.85f, 0.90f}, 0.55f},     // snow
            {"Ground093C", 2.0f, {0.45f, 0.35f, 0.22f}, 0.75f},   // sand
        }};

        // The climate by latitude, before the macro channels perturb it:
        // degrees Celsius at the equator at the body's radius, lost toward a
        // pole as the square of the sine of latitude, and lost per kilometre
        // climbed. A pole at the radius sits about 6 degrees over freezing:
        // cold grass and gravel, snow a few hundred metres up, and an ice cap
        // only on high ground, which leaves the test scene at the north pole
        // on open ground.
        constexpr f32 kEquatorTemperature = 26.0f;
        constexpr f32 kPolarCooling       = 20.0f;
        constexpr f32 kLapseRate          = 6.5f;

        // Moisture taken from the subtropics, peaking at this latitude in
        // radians (28 degrees), over this half-width: the desert belts. And
        // added at the equator, over its own half-width: the wet tropics.
        constexpr f32 kDesertLatitude = 0.49f;
        constexpr f32 kDesertWidth    = 0.2f;
        constexpr f32 kDesertDrying   = 0.35f;
        constexpr f32 kTropicsWidth   = 0.25f;
        constexpr f32 kTropicsWetting = 0.25f;

        f32 smoothstep(f32 edge0, f32 edge1, f32 x)
        {
            f32 const t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }
    }

    span<GroundSet const, kGroundSetCount> ground_sets()
    {
        return kGroundSets;
    }

    u32 ground_materials(f64vec3 const& point, f32vec3 const& normal, f64 radius, f32 temperature, f32 moisture)
    {
        f64 const     distance = glm::length(point);
        f32vec3 const up{point / distance};
        f32 const     altitude = static_cast<f32>((distance - radius) * 1e-3);
        f32 const     latitude = std::asin(std::clamp(up.y, -1.0f, 1.0f));

        f32 const warmth = kEquatorTemperature - kPolarCooling * up.y * up.y - kLapseRate * altitude + temperature;
        f32 const belt    = (std::abs(latitude) - kDesertLatitude) / kDesertWidth;
        f32 const tropics = latitude / kTropicsWidth;
        f32 const wet     = moisture - kDesertDrying * std::exp(-belt * belt) +
                        kTropicsWetting * std::exp(-tropics * tropics);
        f32 const flat   = glm::dot(normal, up);

        // Snow lies at a warmer temperature on wet ground, and slides off
        // slopes long before they are rock.
        f32 const rock      = smoothstep(0.80f, 0.62f, flat);
        f32 const snow_line = 1.0f + 6.0f * (wet - 0.55f);
        f32 const snow      = smoothstep(snow_line, snow_line - 4.0f, warmth) * smoothstep(0.82f, 0.95f, flat);
        f32 const sand  = smoothstep(16.0f, 22.0f, warmth) * smoothstep(0.3f, 0.15f, wet);
        f32 const grass = smoothstep(0.3f, 0.5f, wet) * smoothstep(3.0f, 8.0f, warmth) *
                          smoothstep(34.0f, 28.0f, warmth);

        // The flat materials share what rock leaves; past a full share
        // between them, they are scaled down, and gravel gets nothing.
        f32 const share = (1.0f - rock) / std::max(snow + sand + grass, 1.0f);
        auto const byte = [](f32 weight) {
            return static_cast<u32>(std::lround(std::clamp(weight, 0.0f, 1.0f) * 255.0f));
        };
        return byte(rock) | (byte(grass * share) << 8) | (byte(snow * share) << 16) | (byte(sand * share) << 24);
    }

    array<f32, kGroundSetCount> unpack_ground(u32 packed)
    {
        array<f32, kGroundSetCount> weights{};
        f32 others = 0.0f;
        for (size_t index = 1; index < kGroundSetCount; ++index)
        {
            weights[index] = static_cast<f32>((packed >> (8 * (index - 1))) & 0xFFu) / 255.0f;
            others += weights[index];
        }
        weights[0] = std::clamp(1.0f - others, 0.0f, 1.0f);
        return weights;
    }
}
