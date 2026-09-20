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

    // A unit cube centred on the origin, wound counter-clockwise, with one
    // colour per face so orientation is readable at a glance. Vertices are not
    // shared between faces because the normals differ.
    void build_cube(vector<Vertex>& vertices, vector<u32>& indices);
}
