#pragma once

namespace encke
{
    // Every textured material the renderer loads at startup, from
    // assets/textures (see CREDITS.md there). SceneObject::material picks one;
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

    // One material's maps, decoded and packed for upload. Every map is
    // width x height RGBA8, top row first.
    //
    //   albedo  sRGB colour, alpha 1
    //   normal  tangent space, OpenGL convention: green points to the top of
    //           the image
    //   orm     R ambient occlusion, G roughness, B metalness, the glTF packing
    //
    // A set without an occlusion map packs 1 (unoccluded) there; one without a
    // metalness map packs 0, because ambientCG leaves the map out of
    // non-metals.
    struct MaterialImages
    {
        u32        width  = 0;
        u32        height = 0;
        vector<u8> albedo;
        vector<u8> normal;
        vector<u8> orm;
    };

    // Reads and packs the maps of `kind`, which must not be None. Logs and
    // returns false when a file is missing or the maps disagree in size.
    bool load_material(MaterialKind kind, MaterialImages& images);

    // Name for logs and debug labels.
    char const* material_name(MaterialKind kind);

    // Metres of surface one repeat of the maps covers, across and down the
    // image.
    f32vec2 material_tile_metres(MaterialKind kind);
}
