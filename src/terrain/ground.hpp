#pragma once

namespace encke::terrain
{
    // The ground's ambientCG sets, in the palette's order -- gravel, rock,
    // grass, snow, sand -- with the metres one repeat covers, the flat linear
    // colour each draws in until its maps land, and its roughness floor.
    struct GroundSet
    {
        char const* name;
        f32         tile;
        f32vec3     albedo;
        f32         roughness;
    };

    inline constexpr size_t kGroundSetCount = 5;

    span<GroundSet const, kGroundSetCount> ground_sets();

    // The ground's mix at a point with surface normal `normal`, as
    // TerrainVertex packs it, a byte each from the lowest: rock, grass, snow,
    // sand; gravel is what they leave. Rock where it is steep, then among
    // what rock leaves, snow where it is cold, sand where it is hot and dry,
    // grass where it is mild and wet, and gravel for the rest. The climate
    // is set by latitude, against the body's Y axis, and altitude, and
    // perturbed by the macro channels' `temperature` and `moisture`.
    // Body-relative, like everything in BodyTerrain.
    u32 ground_materials(f64vec3 const& point, f32vec3 const& normal, f64 radius, f32 temperature, f32 moisture);

    // The five weights in ground_sets() order, gravel first, from what
    // ground_materials packs.
    array<f32, kGroundSetCount> unpack_ground(u32 packed);
}
