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

        string map_path(string const& set, char const* map)
        {
            return asset_path("textures/" + set + "/" + set + "_" + kResolution + "_" + map +
                              ".jpg");
        }

        bool same_size(Pixels const& a, Pixels const& b, string const& path)
        {
            if (a.width == b.width && a.height == b.height)
            {
                return true;
            }

            log::error("%s is %ux%u, the roughness map %ux%u", path.c_str(), b.width, b.height,
                       a.width, a.height);
            return false;
        }

        // An optional single-channel map: its red channel, or nullopt when
        // the set does not include it.
        bool load_optional(string const& set, char const* map, Pixels const& reference,
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

        // Its alpha is whatever the decoder made of an opaque JPG; make sure.
        bool load_opaque(string const& path, Pixels& pixels)
        {
            if (!load_pixels(path, pixels))
            {
                return false;
            }
            for (size_t at = 3; at < pixels.rgba.size(); at += 4)
            {
                pixels.rgba[at] = 255;
            }
            return true;
        }

        bool load_orm(string const& set, Pixels& pixels)
        {
            Pixels       roughness;
            string const roughness_path = map_path(set, "Roughness");
            if (!load_pixels(roughness_path, roughness))
            {
                return false;
            }

            optional<Pixels> occlusion;
            optional<Pixels> metalness;
            if (!load_optional(set, "AmbientOcclusion", roughness, occlusion) ||
                !load_optional(set, "Metalness", roughness, metalness))
            {
                return false;
            }

            size_t const texels = size_t{roughness.width} * roughness.height;

            Pixels orm{.width  = roughness.width,
                       .height = roughness.height,
                       .rgba   = vector<u8>(texels * 4)};
            for (size_t texel = 0; texel < texels; ++texel)
            {
                size_t const at  = texel * 4;
                orm.rgba[at + 0] = occlusion.has_value() ? occlusion->rgba[at] : u8{255};
                orm.rgba[at + 1] = roughness.rgba[at];
                orm.rgba[at + 2] = metalness.has_value() ? metalness->rgba[at] : u8{0};
                orm.rgba[at + 3] = 255;
            }

            log::info("material %s: %ux%u%s%s", set.c_str(), roughness.width, roughness.height,
                      occlusion.has_value() ? ", occlusion" : "",
                      metalness.has_value() ? ", metalness" : "");

            pixels = std::move(orm);
            return true;
        }
    }

    bool load_ambientcg(string const& set, AmbientCgMap map, Pixels& pixels)
    {
        switch (map)
        {
        case AmbientCgMap::Colour: return load_opaque(map_path(set, "Color"), pixels);
        case AmbientCgMap::Normal: return load_opaque(map_path(set, "NormalGL"), pixels);
        case AmbientCgMap::Orm:    return load_orm(set, pixels);
        }
        return false;
    }
}
