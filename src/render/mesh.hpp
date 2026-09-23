#pragma once

namespace encke
{
    // Packed, and it must stay that way: GLM's default gentypes are packed in
    // this project precisely so a vertex array uploads without reinterpretation.
    // See the alignment notes in CLAUDE.md before changing any member's type.
    //
    // The tangent frame follows glTF: `tangent` points along +u, and
    // cross(normal, tangent.xyz) * tangent.w points to the TOP of the image,
    // which is -v because v runs down the image from its top row. That is
    // the direction an OpenGL-convention normal map's green channel means.
    //
    // UVs are in metres of surface at the mesh's own unit scale, not 0..1.
    // The G-buffer pass stretches them by the object's scale along the
    // tangent and bitangent and divides by the material's tile size, so one
    // mesh serves every size of box without the texture stretching.
    struct Vertex
    {
        f32vec3 position;
        f32vec3 normal;
        f32vec4 tangent;   // xyz along +u, w bitangent sign
        f32vec2 uv;        // metres at unit scale
    };

    static_assert(sizeof(Vertex) == 48, "Vertex must stay tightly packed");
    static_assert(offsetof(Vertex, position) == 0);
    static_assert(offsetof(Vertex, normal) == 12);
    static_assert(offsetof(Vertex, tangent) == 24);
    static_assert(offsetof(Vertex, uv) == 40);

    // A unit cube centred on the origin, wound counter-clockwise. Vertices are
    // not shared between faces because the normals differ. Each face maps the
    // whole unit square, upright on the sides and seen from outside.
    void build_cube(vector<Vertex>& vertices, vector<u32>& indices);

    // A UV sphere of diameter 1 centred on the origin, so it scales like the
    // cube: scale is the object's size in metres. u runs round the equator,
    // v from the north pole; the seam column is duplicated, and each pole is
    // one vertex per slice, so neither pinches the texture across a triangle.
    void build_sphere(vector<Vertex>& vertices, vector<u32>& indices, u32 slices, u32 stacks);

    // A sphere of `radius` metres whose local origin is its NORTH POLE, not its
    // centre: the centre is at (0, -radius, 0). An f32 vertex at Earth radius
    // from the centre is only good to about half a metre, which would make the
    // ground under the camera visibly faceted and stepped. Measured from the
    // pole, vertices near it are small numbers with full precision, and the
    // imprecise ones are thousands of kilometres away where nothing can tell.
    //
    // Rings are spaced geometrically in polar angle, starting at
    // `first_ring` metres of arc and growing by `ring_growth` each ring, so
    // the tessellation is fine underfoot and coarse at the horizon. A flat
    // facet between rings sags below the true sphere by about spacing^2 / 8R,
    // which stays under a millimetre within a few kilometres of the pole.
    //
    // Texture coordinates are planar, the local x and z in metres, which is
    // exact on the near-flat ground around the pole and degrades only where
    // nothing can see the texture. Far from the pole they are large numbers
    // with little f32 precision left, and by then every sample is from the
    // smallest mips.
    void build_planet(vector<Vertex>& vertices, vector<u32>& indices, f64 radius, u32 slices,
                      f64 first_ring, f64 ring_growth);
}
