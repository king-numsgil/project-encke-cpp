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

    // Where a vertex goes when its mesh is fully geomorphed toward the next
    // coarser LOD, in a stream parallel to the vertices: the geometry pool
    // keeps one beside its vertex buffer, element for element, and a vertex
    // shader reads it by vertex index. Only terrain chunks carry them.
    struct MorphTarget
    {
        f32vec3 position;
        u32     normal;   // pack_normal
    };

    static_assert(sizeof(MorphTarget) == 16);

    // A unit normal, octahedral, 16 bits a component in [0, 1], x low:
    // shaders/lib/normal.slang's encode_normal, quantised.
    u32 pack_normal(f32vec3 const& normal);

    // A unit cube centred on the origin, wound counter-clockwise. Vertices are
    // not shared between faces because the normals differ. Each face maps the
    // whole unit square, upright on the sides and seen from outside.
    void build_cube(vector<Vertex>& vertices, vector<u32>& indices);

    // A UV sphere of diameter 1 centred on the origin, so it scales like the
    // cube: scale is the object's size in metres. u runs round the equator,
    // v from the north pole; the seam column is duplicated, and each pole is
    // one vertex per slice, so neither pinches the texture across a triangle.
    void build_sphere(vector<Vertex>& vertices, vector<u32>& indices, u32 slices, u32 stacks);
}
