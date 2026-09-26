#include "core/pch.hpp"

#include "terrain/surface_map.hpp"

#include "core/log.hpp"
#include "render/material.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace encke::terrain
{
    namespace
    {
        f64 srgb_to_linear(f64 encoded)
        {
            return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
        }

        // Through a table, since a bake encodes millions of texels: 4096
        // steps are finer than a byte of sRGB even where its curve is
        // steepest, near black.
        u8 linear_to_srgb_byte(f64 linear)
        {
            constexpr size_t kSteps = 4096;
            static array<u8, kSteps> const table = [] {
                array<u8, kSteps> bytes{};
                for (size_t step = 0; step < kSteps; ++step)
                {
                    f64 const value   = static_cast<f64>(step) / static_cast<f64>(kSteps - 1);
                    f64 const encoded = value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
                    bytes[step]       = static_cast<u8>(std::lround(encoded * 255.0));
                }
                return bytes;
            }();
            f64 const clamped = std::clamp(linear, 0.0, 1.0);
            return table[static_cast<size_t>(std::lround(clamped * static_cast<f64>(kSteps - 1)))];
        }

        u8 unorm_byte(f64 value)
        {
            return static_cast<u8>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
        }

        // The mean of a map's RGB, each channel over [0, 1].
        f64vec3 mean_rgb(Pixels const& pixels, bool srgb)
        {
            array<f64, 256> decode{};
            for (size_t value = 0; value < decode.size(); ++value)
            {
                f64 const unit = static_cast<f64>(value) / 255.0;
                decode[value]  = srgb ? srgb_to_linear(unit) : unit;
            }

            f64vec3      sum{0.0};
            size_t const texels = size_t{pixels.width} * pixels.height;
            for (size_t texel = 0; texel < texels; ++texel)
            {
                u8 const* const rgb = pixels.rgba.data() + texel * 4;
                sum += f64vec3{decode[rgb[0]], decode[rgb[1]], decode[rgb[2]]};
            }
            return sum / static_cast<f64>(std::max<size_t>(texels, 1));
        }

        // Axis `a`'s unit vector.
        f64vec3 axis(u32 a)
        {
            f64vec3 unit{0.0};
            unit[static_cast<glm::length_t>(a)] = 1.0;
            return unit;
        }
    }

    f64vec3 face_direction(u32 face, f64 s, f64 t)
    {
        u32 const a    = face / 2;
        f64 const sign = face % 2 == 0 ? 1.0 : -1.0;
        return glm::normalize(axis(a) * sign + axis((a + 1) % 3) * s + axis((a + 2) % 3) * t);
    }

    FacePoint face_point(f64vec3 const& direction)
    {
        f64vec3 const size = glm::abs(direction);
        u32 const     a    = size.x >= size.y && size.x >= size.z ? 0u : (size.y >= size.z ? 1u : 2u);
        auto const    at   = [&](u32 index) { return direction[static_cast<glm::length_t>(index)]; };
        f64 const     major = at(a);
        return FacePoint{
            .face = 2 * a + (major < 0.0 ? 1u : 0u),
            .st   = f64vec2{at((a + 1) % 3), at((a + 2) % 3)} / std::abs(major),
        };
    }

    f64vec3 face_tangent(u32 face, f64vec3 const& direction)
    {
        f64vec3 const along = axis((face / 2 + 1) % 3);
        return glm::normalize(along - direction * glm::dot(along, direction));
    }

    f64vec2 map_height_range(BodyTerrain const& terrain)
    {
        MacroChannelSpec const& height = terrain.macro.channels[static_cast<size_t>(MacroChannel::Height)];
        f64 const               reach  = std::abs(static_cast<f64>(height.scale));
        return f64vec2{static_cast<f64>(height.bias) - reach, std::max(2.0 * reach, 1.0)};
    }

    GroundLook measure_ground()
    {
        GroundLook                                   look;
        span<GroundSet const, kGroundSetCount> const sets = ground_sets();
        for (size_t index = 0; index < sets.size(); ++index)
        {
            GroundSet const& set = sets[index];
            look.albedo[index]    = set.albedo;
            look.roughness[index] = set.roughness;

            Pixels colour;
            Pixels orm;
            Pixels normal;
            if (!load_ambientcg(set.name, AmbientCgMap::Colour, colour) ||
                !load_ambientcg(set.name, AmbientCgMap::Orm, orm) ||
                !load_ambientcg(set.name, AmbientCgMap::Normal, normal))
            {
                log::error("surface map: %s's maps did not load; its flat colour stands in", set.name);
                continue;
            }

            // As the G-buffer shades it once every mip has averaged away:
            // the map's roughness into [floor, 1], then Toksvig for the
            // bump the average normal's shortfall stands for.
            f64 const map_roughness = mean_rgb(orm, false).g;
            f64 const roughness     = static_cast<f64>(set.roughness) + (1.0 - static_cast<f64>(set.roughness)) * map_roughness;
            f64 const bump          = std::max(glm::length(mean_rgb(normal, false) * 2.0 - 1.0), 1e-3);
            f64 const variance      = std::clamp((1.0 - bump) / bump, 0.0, 1.0);
            f64 const alpha         = roughness * roughness;

            look.albedo[index]    = f32vec3{mean_rgb(colour, true)};
            look.roughness[index] = static_cast<f32>(std::min(std::sqrt(std::sqrt(alpha * alpha + variance)), 1.0));
        }
        return look;
    }

    void bake_face(BodyTerrain const& terrain, GroundLook const& look, SurfaceMapLayout const& layout, u32 face,
                   SurfaceTile& tile)
    {
        // Each texel averages kSub x kSub sub-samples: the macro layer has
        // spurs a few kilometres wide, under a texel, and one sample a texel
        // turns them into speckle wherever slope decides the ground. The
        // sub-samples lie on a grid over the tile with a ring more, for the
        // normals' differences, index 0 being the ring.
        constexpr u32 kSub = 2;
        u32 const     n            = layout.face;
        u32 const     side         = layout.tile();
        u32 const     grid         = side * kSub + 2;
        f64 const     radius       = terrain.radius;
        f64vec2 const height_range = map_height_range(terrain);
        MacroField    macro{terrain.macro, terrain.seed};
        MacroValues   values;

        // Sub-sample q's (s or t): its centre in texels from the tile's
        // corner, less the border, over the face.
        vector<f64> coordinates(grid);
        for (u32 q = 0; q < grid; ++q)
        {
            f64 const texels = (static_cast<f64>(q) - 0.5) / kSub - static_cast<f64>(layout.border);
            coordinates[q]   = texels / static_cast<f64>(n) * 2.0 - 1.0;
        }

        // Rows of sub-samples, the ground's height and where that puts it,
        // kept four at a time: a texel row needs the two of its own and one
        // either side, and the next shares two of them.
        struct SubRow
        {
            i64             index = -1;
            vector<f64vec3> points;
        };
        array<SubRow, 4> rows;
        vector<f32>      x(grid), y(grid), z(grid);
        auto const       height  = static_cast<size_t>(MacroChannel::Height);
        vector<f64vec3>  directions(grid);
        auto const       sub_row = [&](u32 q) -> SubRow const& {
            SubRow& row = rows[q % rows.size()];
            if (row.index == static_cast<i64>(q))
            {
                return row;
            }
            row.index = static_cast<i64>(q);
            row.points.resize(grid);
            for (u32 c = 0; c < grid; ++c)
            {
                directions[c]   = face_direction(face, coordinates[c], coordinates[q]);
                f64vec3 const p = directions[c] * radius;
                x[c]            = static_cast<f32>(p.x);
                y[c]            = static_cast<f32>(p.y);
                z[c]            = static_cast<f32>(p.z);
            }
            macro.sample_points(x, y, z, values, height, height + 1);
            for (u32 c = 0; c < grid; ++c)
            {
                row.points[c] = directions[c] * (radius + static_cast<f64>(values.channels[height][c]));
            }
            return row;
        };

        tile.albedo.assign(size_t{side} * side * 4, 255);
        tile.surface.assign(size_t{side} * side * 4, 255);

        auto const temperature = static_cast<size_t>(MacroChannel::Temperature);
        auto const moisture    = static_cast<size_t>(MacroChannel::Moisture);

        // Per texel of the row being baked: its sub-samples' points and
        // normals, and its centre, where the climate is read.
        vector<array<f64vec3, kSub * kSub>> sub_points(side);
        vector<array<f64vec3, kSub * kSub>> sub_normals(side);
        vector<f64vec3>                     centres(side);
        vector<f32>                         cx(side), cy(side), cz(side);

        for (u32 texel_row = 0; texel_row < side; ++texel_row)
        {
            u32 const            first = texel_row * kSub;
            array<SubRow const*, kSub + 2> around{};
            for (u32 r = 0; r < around.size(); ++r)
            {
                around[r] = &sub_row(first + r);
            }

            for (u32 column = 0; column < side; ++column)
            {
                f64vec3 centre{0.0};
                for (u32 sy = 0; sy < kSub; ++sy)
                {
                    for (u32 sx = 0; sx < kSub; ++sx)
                    {
                        u32 const       c      = column * kSub + sx + 1;
                        SubRow const&   here   = *around[sy + 1];
                        f64vec3 const   across = here.points[c + 1] - here.points[c - 1];
                        f64vec3 const   down   = around[sy + 2]->points[c] - around[sy]->points[c];
                        f64vec3         normal = glm::normalize(glm::cross(across, down));
                        normal                 = glm::dot(normal, here.points[c]) < 0.0 ? -normal : normal;

                        sub_points[column][sy * kSub + sx]  = here.points[c];
                        sub_normals[column][sy * kSub + sx] = normal;
                        centre += here.points[c];
                    }
                }
                centres[column] = centre / static_cast<f64>(kSub * kSub);
                cx[column]      = static_cast<f32>(centres[column].x);
                cy[column]      = static_cast<f32>(centres[column].y);
                cz[column]      = static_cast<f32>(centres[column].z);
            }
            macro.sample_points(cx, cy, cz, values, temperature, moisture + 1);

            for (u32 column = 0; column < side; ++column)
            {
                // Heights averaged as heights: points averaged on a sphere
                // fall inside it.
                f64vec3 albedo{0.0};
                f64vec3 normal{0.0};
                f64     roughness = 0.0;
                f64     above     = 0.0;
                for (u32 sub = 0; sub < kSub * kSub; ++sub)
                {
                    above += glm::length(sub_points[column][sub]) - radius;
                    u32 const packed = ground_materials(sub_points[column][sub], f32vec3{sub_normals[column][sub]},
                                                        radius, values.channels[temperature][column],
                                                        values.channels[moisture][column]);
                    array<f32, kGroundSetCount> const weights = unpack_ground(packed);
                    for (size_t set = 0; set < kGroundSetCount; ++set)
                    {
                        albedo    += f64vec3{look.albedo[set]} * static_cast<f64>(weights[set]);
                        roughness += static_cast<f64>(look.roughness[set]) * static_cast<f64>(weights[set]);
                    }
                    normal += sub_normals[column][sub];
                }
                f64 const share = 1.0 / static_cast<f64>(kSub * kSub);
                albedo    *= share;
                roughness *= share;
                above     *= share;
                normal     = glm::normalize(normal);

                f64vec3 const direction = glm::normalize(centres[column]);
                f64vec3 const along_s   = face_tangent(face, direction);
                f64vec3 const along_t   = glm::cross(direction, along_s);

                size_t const texel = (size_t{texel_row} * side + column) * 4;
                tile.albedo[texel + 0]  = linear_to_srgb_byte(albedo.r);
                tile.albedo[texel + 1]  = linear_to_srgb_byte(albedo.g);
                tile.albedo[texel + 2]  = linear_to_srgb_byte(albedo.b);
                tile.surface[texel + 0] = unorm_byte(glm::dot(normal, along_s) + 0.5);
                tile.surface[texel + 1] = unorm_byte(glm::dot(normal, along_t) + 0.5);
                tile.surface[texel + 2] = unorm_byte(roughness);
                tile.surface[texel + 3] = unorm_byte((above - height_range.x) / height_range.y);
            }
        }
    }

    void assemble_atlas(span<SurfaceTile const> tiles, SurfaceMapLayout const& layout, Pixels& albedo,
                        Pixels& surface)
    {
        u32 const    side  = layout.tile();
        size_t const bytes = size_t{layout.width()} * layout.height() * 4;
        albedo  = Pixels{.width = layout.width(), .height = layout.height(), .rgba = vector<u8>(bytes)};
        surface = Pixels{.width = layout.width(), .height = layout.height(), .rgba = vector<u8>(bytes)};

        for (u32 face = 0; face < tiles.size(); ++face)
        {
            size_t const left = size_t{face % 3} * side;
            size_t const top  = size_t{face / 3} * side;
            for (u32 row = 0; row < side; ++row)
            {
                size_t const from = size_t{row} * side * 4;
                size_t const to   = ((top + row) * layout.width() + left) * 4;
                std::memcpy(albedo.rgba.data() + to, tiles[face].albedo.data() + from, size_t{side} * 4);
                std::memcpy(surface.rgba.data() + to, tiles[face].surface.data() + from, size_t{side} * 4);
            }
        }
    }
}
