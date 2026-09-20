#include "core/pch.hpp"

#include "render/mesh.hpp"

#include "core/log.hpp"
#include "vulkan/device.hpp"

namespace encke
{
    namespace
    {
        // Linear values; see shaders/lib/colour.slang for why these are not the
        // numbers you would name the colours with.
        // const, not constexpr: GLM types are not constexpr-constructible here
        // because GLM_FORCE_AVX2 sets the SIMD arch bit. See CLAUDE.md.
        f32vec3 const kFaceColours[6]{
            {0.640f, 0.055f, 0.070f},   // +X  red
            {0.055f, 0.250f, 0.640f},   // -X  blue
            {0.070f, 0.450f, 0.120f},   // +Y  green
            {0.360f, 0.180f, 0.020f},   // -Y  amber
            {0.500f, 0.450f, 0.055f},   // +Z  yellow
            {0.300f, 0.055f, 0.400f},   // -Z  violet
        };

        // Face basis vectors: normal, then the two in-plane axes. Emitting
        // corners as normal + (-u -v, +u -v, +u +v, -u +v) gives a
        // counter-clockwise winding seen from outside, which is what the
        // pipeline's frontFace expects.
        struct Face
        {
            f32vec3 normal;
            f32vec3 u;
            f32vec3 v;
        };

        Face const kFaces[6]{
            {{ 1.0f,  0.0f,  0.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 1.0f,  0.0f}},
            {{-1.0f,  0.0f,  0.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 1.0f,  0.0f}},
            {{ 0.0f,  1.0f,  0.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f, -1.0f}},
            {{ 0.0f, -1.0f,  0.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f,  1.0f}},
            {{ 0.0f,  0.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 1.0f,  0.0f}},
            {{ 0.0f,  0.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 1.0f,  0.0f}},
        };
    }

    void build_cube(vector<Vertex>& vertices, vector<u32>& indices)
    {
        vertices.clear();
        indices.clear();
        vertices.reserve(24);
        indices.reserve(36);

        for (u32 face = 0; face < 6; ++face)
        {
            Face const&    basis  = kFaces[face];
            f32vec3 const& colour = kFaceColours[face];

            f32vec3 const centre = basis.normal * 0.5f;
            f32vec3 const u      = basis.u * 0.5f;
            f32vec3 const v      = basis.v * 0.5f;

            u32 const base = static_cast<u32>(vertices.size());

            vertices.push_back({centre - u - v, basis.normal, colour});
            vertices.push_back({centre + u - v, basis.normal, colour});
            vertices.push_back({centre + u + v, basis.normal, colour});
            vertices.push_back({centre - u + v, basis.normal, colour});

            indices.push_back(base + 0);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base + 0);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        }
    }

    bool Mesh::init(VulkanAllocator const& allocator, VulkanDevice const& device,
                    span<Vertex const> vertices, span<u32 const> indices)
    {
        if (vertices.empty() || indices.empty())
        {
            log::error("mesh needs both vertices and indices");
            return false;
        }

        if (!vertices_.init(allocator, device, vertices.data(),
                            vertices.size() * sizeof(Vertex),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        {
            return false;
        }

        if (!indices_.init(allocator, device, indices.data(), indices.size() * sizeof(u32),
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        {
            return false;
        }

        index_count_ = static_cast<u32>(indices.size());
        log::info("mesh uploaded: %zu vertices, %u indices", vertices.size(), index_count_);
        return true;
    }

    void Mesh::draw(VkCommandBuffer command) const
    {
        VkBuffer const     buffer = vertices_.handle();
        VkDeviceSize const offset = 0;

        vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
        vkCmdBindIndexBuffer(command, indices_.handle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command, index_count_, 1, 0, 0, 0);
    }

    void Mesh::shutdown()
    {
        indices_.shutdown();
        vertices_.shutdown();
        index_count_ = 0;
    }
}
