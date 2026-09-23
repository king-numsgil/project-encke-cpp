#include "core/pch.hpp"

#include "render/material.hpp"

#include "core/log.hpp"
#include "render/pixels.hpp"

#include <utility>

namespace encke
{
    namespace
    {
        // Part of every file name; see assets/textures/CREDITS.md.
        constexpr char kResolution[] = "1K-JPG";

        struct Definition
        {
            char const* set;    // ambientCG asset id, also the directory name
            f32vec2     tile;   // metres per repeat, across and down
        };

        // Indexed by MaterialKind. const, not constexpr: GLM types are not
        // constexpr-constructible here. See CLAUDE.md.
        Definition const kDefinitions[kMaterialKindCount]{
            {"", f32vec2{1.0f}},
            {"Ground110", f32vec2{2.1f}},
            {"Concrete034", f32vec2{1.1f, 0.55f}},
            {"Planks037A", f32vec2{2.0f}},
            {"PaintedMetal006", f32vec2{1.5f}},
            {"Metal041B", f32vec2{1.0f}},
            {"MetalPlates013", f32vec2{1.6f}},
        };

        Definition const& definition(MaterialKind kind)
        {
            return kDefinitions[static_cast<u32>(kind)];
        }

        string map_path(char const* set, char const* map)
        {
            return asset_path(string{"textures/"} + set + "/" + set + "_" + kResolution + "_" +
                              map + ".jpg");
        }

        bool same_size(Pixels const& a, Pixels const& b, string const& path)
        {
            if (a.width == b.width && a.height == b.height)
            {
                return true;
            }

            log::error("%s is %ux%u, the colour map %ux%u", path.c_str(), b.width, b.height,
                       a.width, a.height);
            return false;
        }

        // An optional single-channel map: its red channel, or nullopt when
        // the set does not include it.
        bool load_optional(char const* set, char const* map, Pixels const& reference,
                           optional<Pixels>& pixels)
        {
            string const path = map_path(set, map);
            if (!SDL_GetPathInfo(path.c_str(), nullptr))
            {
                pixels = nullopt;
                return true;
            }

            pixels.emplace();
            return load_pixels(path, *pixels) && same_size(reference, *pixels, path);
        }
    }

    bool load_material(MaterialKind kind, MaterialMaps& maps)
    {
        if (kind == MaterialKind::None)
        {
            log::error("load_material: None has no maps");
            return false;
        }

        char const* const set = definition(kind).set;

        Pixels colour;
        Pixels normal;
        Pixels roughness;

        string const colour_path    = map_path(set, "Color");
        string const normal_path    = map_path(set, "NormalGL");
        string const roughness_path = map_path(set, "Roughness");

        if (!load_pixels(colour_path, colour) ||
            !load_pixels(normal_path, normal) || !same_size(colour, normal, normal_path) ||
            !load_pixels(roughness_path, roughness) ||
            !same_size(colour, roughness, roughness_path))
        {
            return false;
        }

        optional<Pixels> occlusion;
        optional<Pixels> metalness;
        if (!load_optional(set, "AmbientOcclusion", colour, occlusion) ||
            !load_optional(set, "Metalness", colour, metalness))
        {
            return false;
        }

        size_t const texels = size_t{colour.width} * colour.height;

        Pixels orm{.width = colour.width, .height = colour.height, .rgba = vector<u8>(texels * 4)};
        for (size_t texel = 0; texel < texels; ++texel)
        {
            size_t const at = texel * 4;
            colour.rgba[at + 3] = 255;
            normal.rgba[at + 3] = 255;

            orm.rgba[at + 0] = occlusion.has_value() ? occlusion->rgba[at] : u8{255};
            orm.rgba[at + 1] = roughness.rgba[at];
            orm.rgba[at + 2] = metalness.has_value() ? metalness->rgba[at] : u8{0};
            orm.rgba[at + 3] = 255;
        }

        log::info("material %s: %ux%u%s%s", set, colour.width, colour.height,
                  occlusion.has_value() ? ", occlusion" : "",
                  metalness.has_value() ? ", metalness" : "");

        maps          = MaterialMaps{};
        maps.albedo   = std::move(colour);
        maps.normal   = std::move(normal);
        maps.orm      = std::move(orm);
        return true;
    }

    char const* material_name(MaterialKind kind)
    {
        return kind == MaterialKind::None ? "none" : definition(kind).set;
    }

    f32vec2 material_tile_metres(MaterialKind kind)
    {
        return definition(kind).tile;
    }
}
