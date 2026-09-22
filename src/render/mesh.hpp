#pragma once

#include "vulkan/buffer.hpp"

namespace encke
{
    class VulkanAllocator;
    class VulkanDevice;

    // Packed, and it must stay that way: GLM's default gentypes are packed in
    // this project precisely so a vertex array uploads without reinterpretation.
    // See the alignment notes in CLAUDE.md before changing any member's type.
    struct Vertex
    {
        f32vec3 position;
        f32vec3 normal;
        f32vec3 colour;   // linear, not sRGB
    };

    static_assert(sizeof(Vertex) == 36, "Vertex must stay tightly packed");
    static_assert(offsetof(Vertex, position) == 0);
    static_assert(offsetof(Vertex, normal) == 12);
    static_assert(offsetof(Vertex, colour) == 24);

    class Mesh
    {
    public:
        Mesh() = default;
        ~Mesh() = default;

        Mesh(Mesh const&)            = delete;
        Mesh& operator=(Mesh const&) = delete;
        Mesh(Mesh&&)                 = delete;
        Mesh& operator=(Mesh&&)      = delete;

        bool init(VulkanAllocator const& allocator, VulkanDevice const& device,
                  span<Vertex const> vertices, span<u32 const> indices);

        void shutdown();

        // Binds the buffers and issues the indexed draw.
        void draw(VkCommandBuffer command) const;

    private:
        Buffer vertices_;
        Buffer indices_;
        u32    index_count_ = 0;
    };

    // Every mesh the renderer builds at startup. SceneObject::mesh picks one.
    enum class MeshKind : u32
    {
        Cube   = 0,
        Sphere = 1,
        Planet = 2,
    };

    inline constexpr u32 kMeshKindCount = 3;

    // A unit cube centred on the origin, wound counter-clockwise, with one
    // colour per face so orientation is readable at a glance. Vertices are not
    // shared between faces because the normals differ.
    void build_cube(vector<Vertex>& vertices, vector<u32>& indices);

    // A UV sphere of diameter 1 centred on the origin, so it scales like the
    // cube: scale is the object's size in metres.
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
    void build_planet(vector<Vertex>& vertices, vector<u32>& indices, f64 radius, u32 slices,
                      f64 first_ring, f64 ring_growth);
}
