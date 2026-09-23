#pragma once

#include "render/pixels.hpp"

namespace encke
{
    // Every textured material the renderer loads at startup, from
    // assets/textures (see CREDITS.md there). Renderable::material picks one;
    // None draws with the object's flat factors alone.
    enum class MaterialKind : u32
    {
        None         = 0,
        Ground       = 1,
        Concrete     = 2,
        Planks       = 3,
        PaintedMetal = 4,
        RustedMetal  = 5,
        MetalPlates  = 6,
    };

    inline constexpr u32 kMaterialKindCount = 7;

    // One material's maps, decoded and packed for upload, each RGBA8 top row
    // first and each any size. A missing map is nullopt: the renderer puts
    // white in for albedo and ORM, which leaves the factor alone, and skips
    // the normal and emission samples.
    //
    //   albedo    sRGB colour
    //   normal    tangent space, OpenGL convention: green points to the top
    //             of the image, which is glTF's convention too
    //   orm       R ambient occlusion, G roughness, B metalness, glTF's packing
    //   emission  sRGB
    struct MaterialMaps
    {
        optional<Pixels> albedo;
        optional<Pixels> normal;
        optional<Pixels> orm;
        optional<Pixels> emission;
    };

    // Reads and packs the maps of `kind`, which must not be None: albedo,
    // normal and ORM, all one size. A set without an occlusion map packs 1
    // (unoccluded) there; one without a metalness map packs 0, because
    // ambientCG leaves the map out of non-metals. Logs and returns false when
    // a file is missing or the maps disagree in size. Safe on any thread.
    bool load_material(MaterialKind kind, MaterialMaps& maps);

    // Name for logs and debug labels.
    char const* material_name(MaterialKind kind);

    // Metres of surface one repeat of the maps covers, across and down the
    // image.
    f32vec2 material_tile_metres(MaterialKind kind);
}
