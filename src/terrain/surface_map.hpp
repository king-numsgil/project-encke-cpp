#pragma once

#include "render/pixels.hpp"
#include "terrain/ground.hpp"
#include "terrain/terrain_field.hpp"

namespace encke::terrain
{
    // A body's look from far off, baked once: its ground's colour, its
    // relief as a normal, and its roughness, over a cube around its centre.
    // It stands in for the terrain where the relief is under a pixel (an
    // impostor sphere), and shades the coarsest chunks, whose voxels are
    // wider than its texels.
    //
    // Face f looks along axis a = f / 2, positive for even f: 0 +x, 1 -x,
    // 2 +y, 3 -y, 4 +z, 5 -z. Its s and t run along axes (a + 1) % 3 and
    // (a + 2) % 3, whatever the sign, so a direction d on it is
    // (d[a + 1], d[a + 2]) / |d[a]|, from -1 to 1. shaders/lib/surface_map.slang
    // mirrors all of this; change one, change the other.
    //
    // Each face is a tile of `face` texels a side inside a `border` of the
    // same projection carried on past the face's edge, which is the
    // neighbouring faces' ground seen from this face. So filtering and mips
    // never reach across into another tile until they are `border` texels
    // wide. The six tiles sit three across and two down, face f at column
    // f % 3, row f / 3; t runs down the image, as rows do.
    struct SurfaceMapLayout
    {
        u32 face   = 1024;
        u32 border = 16;

        u32 tile() const { return face + 2 * border; }
        u32 width() const { return 3 * tile(); }
        u32 height() const { return 2 * tile(); }
    };

    // The unit direction at (s, t) on `face`.
    f64vec3 face_direction(u32 face, f64 s, f64 t);

    // The face a direction points through, the one of its largest
    // component (x first, then y, on a tie), and its (s, t) there.
    struct FacePoint
    {
        u32     face = 0;
        f64vec2 st{0.0};
    };
    FacePoint face_point(f64vec3 const& direction);

    // The tangent along s at `direction`, unit, in the face's plane of s
    // and the direction. With cross(direction, it), the frame a baked
    // normal is stored in: right-handed about the outward direction, so
    // the second axis runs along +t on the positive faces and -t on the
    // negative ones.
    f64vec3 face_tangent(u32 face, f64vec3 const& direction);

    // Every ground set's colour and roughness seen from far enough that its
    // maps have averaged to one texel: the mean of its colour map, decoded
    // to linear, and of its roughness remapped as the G-buffer remaps it,
    // with the bump its normal map averages away added back as roughness,
    // as the G-buffer's Toksvig does. A set whose maps do not load keeps
    // its flat colour and roughness floor, logged. Reads files; any thread.
    struct GroundLook
    {
        array<f32vec3, kGroundSetCount> albedo{};
        array<f32, kGroundSetCount>     roughness{};
    };
    GroundLook measure_ground();

    // One face's tile, `layout.tile()` texels a side, x fastest, RGBA8.
    //
    //   albedo   sRGB colour, the ground sets' looks weighted as
    //            ground_materials weighs them at each texel's surface
    //   surface  R, G the surface normal in the face's tangent frame at the
    //            texel (face_tangent, then along t), plus a half, so slopes
    //            to 30 degrees; B roughness; A the height over the radius,
    //            across map_height_range
    //
    // Each texel averages 2x2 sub-samples, since the macro layer's finest
    // spurs are under a texel and one sample each would alias them into
    // speckle wherever slope decides the ground. At each, the surface is
    // the macro height read at the radius, the detail octaves being far
    // under a texel, and the normal is taken across the neighbouring
    // sub-samples. The climate is read once, at the texel's centre. Any
    // thread; it makes its own FastNoise2 nodes.
    struct SurfaceTile
    {
        vector<u8> albedo;
        vector<u8> surface;
    };
    void bake_face(BodyTerrain const& terrain, GroundLook const& look, SurfaceMapLayout const& layout, u32 face,
                   SurfaceTile& tile);

    // The lowest height the macro layer can reach, and the span from it to
    // the highest: its bias less and plus its scale, since the graphs stay
    // within [-1, 1]. The surface map's alpha runs across it.
    f64vec2 map_height_range(BodyTerrain const& terrain);

    // The six tiles laid out as one atlas each.
    void assemble_atlas(span<SurfaceTile const> tiles, SurfaceMapLayout const& layout, Pixels& albedo,
                        Pixels& surface);
}
