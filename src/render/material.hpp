#pragma once

#include "render/pixels.hpp"

namespace encke
{
    // The textures a material samples, each RGBA8, top row first, any size.
    // A material without one gets white for albedo and ORM, which leaves the
    // factor alone, and skips the normal and emission samples.
    //
    //   albedo    sRGB colour
    //   normal    tangent space, OpenGL convention: green points to the top
    //             of the image, which is glTF's convention too
    //   orm       R ambient occlusion, G roughness, B metalness, glTF's packing
    //   emission  sRGB

    // The textures an ambientCG set provides.
    enum class AmbientCgMap : u8
    {
        Colour,
        Normal,
        Orm,
    };

    // Reads one texture of the ambientCG set `set`, from
    // assets/textures/<set> (see CREDITS.md there). The ORM is packed from
    // the set's roughness, and its occlusion and metalness maps where it has
    // them: 1 (unoccluded) without occlusion, 0 without metalness, because
    // ambientCG leaves the map out of non-metals. Logs and returns false
    // when a file is missing or the maps packed together disagree in size.
    // Safe on any thread.
    bool load_ambientcg(string const& set, AmbientCgMap map, Pixels& pixels);
}
