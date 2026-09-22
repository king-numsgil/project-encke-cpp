#include "core/pch.hpp"

#include "render/mesh.hpp"

#include "core/log.hpp"
#include "vulkan/device.hpp"

#include <cmath>

namespace encke
{
    namespace
    {
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

        // const, not constexpr: GLM types are not constexpr-constructible here
        // because GLM_FORCE_AVX2 sets the SIMD arch bit. See CLAUDE.md.
        Face const kFaces[6]{
            {{ 1.0f,  0.0f,  0.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 1.0f,  0.0f}},
            {{-1.0f,  0.0f,  0.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 1.0f,  0.0f}},
            {{ 0.0f,  1.0f,  0.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f, -1.0f}},
            {{ 0.0f, -1.0f,  0.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f,  1.0f}},
            {{ 0.0f,  0.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 1.0f,  0.0f}},
            {{ 0.0f,  0.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 1.0f,  0.0f}},
        };

        constexpr f64 kPi = 3.14159265358979323846;

        // Emits a triangle wound counter-clockwise seen from outside, deciding
        // the order from the geometry rather than trusting the caller's index
        // arithmetic: its face normal must agree with the vertex normals.
        void emit_outward(vector<Vertex> const& vertices, vector<u32>& indices, u32 a, u32 b, u32 c)
        {
            f32vec3 const pa = vertices[a].position;
            f32vec3 const face = glm::cross(vertices[b].position - pa, vertices[c].position - pa);
            f32vec3 const outward = vertices[a].normal + vertices[b].normal + vertices[c].normal;

            indices.push_back(a);
            if (glm::dot(face, outward) >= 0.0f)
            {
                indices.push_back(b);
                indices.push_back(c);
            }
            else
            {
                indices.push_back(c);
                indices.push_back(b);
            }
        }

        // Rings of `slices` vertices, one per polar angle in `polar` (strictly
        // between the poles), capped by a single vertex at each pole. `place`
        // maps a unit direction from the centre to a local position.
        template<class Place>
        void build_rings(vector<Vertex>& vertices, vector<u32>& indices, span<f64 const> polar,
                         u32 slices, Place const& place)
        {
            vertices.clear();
            indices.clear();

            f32vec3 const colour{1.0f};

            auto add = [&](f64vec3 const& direction) {
                vertices.push_back({place(direction), f32vec3{direction}, colour});
            };

            add(f64vec3{0.0, 1.0, 0.0});

            for (f64 const theta : polar)
            {
                for (u32 slice = 0; slice < slices; ++slice)
                {
                    f64 const phi = 2.0 * kPi * static_cast<f64>(slice) / static_cast<f64>(slices);
                    add(f64vec3{std::sin(theta) * std::cos(phi), std::cos(theta),
                                std::sin(theta) * std::sin(phi)});
                }
            }

            add(f64vec3{0.0, -1.0, 0.0});

            u32 const rings = static_cast<u32>(polar.size());
            u32 const north = 0;
            u32 const south = 1 + rings * slices;

            auto ring_vertex = [slices](u32 ring, u32 slice) {
                return 1 + ring * slices + slice % slices;
            };

            for (u32 slice = 0; slice < slices; ++slice)
            {
                emit_outward(vertices, indices, north, ring_vertex(0, slice),
                             ring_vertex(0, slice + 1));
                emit_outward(vertices, indices, south, ring_vertex(rings - 1, slice + 1),
                             ring_vertex(rings - 1, slice));
            }

            for (u32 ring = 0; ring + 1 < rings; ++ring)
            {
                for (u32 slice = 0; slice < slices; ++slice)
                {
                    u32 const a = ring_vertex(ring, slice);
                    u32 const b = ring_vertex(ring + 1, slice);
                    u32 const c = ring_vertex(ring + 1, slice + 1);
                    u32 const d = ring_vertex(ring, slice + 1);
                    emit_outward(vertices, indices, a, b, c);
                    emit_outward(vertices, indices, a, c, d);
                }
            }
        }
    }

    void build_cube(vector<Vertex>& vertices, vector<u32>& indices)
    {
        vertices.clear();
        indices.clear();
        vertices.reserve(24);
        indices.reserve(36);

        // White: colour comes from the object's material, not the mesh.
        f32vec3 const colour{1.0f};

        for (u32 face = 0; face < 6; ++face)
        {
            Face const& basis = kFaces[face];

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

    void build_sphere(vector<Vertex>& vertices, vector<u32>& indices, u32 slices, u32 stacks)
    {
        vector<f64> polar;
        for (u32 stack = 1; stack < stacks; ++stack)
        {
            polar.push_back(kPi * static_cast<f64>(stack) / static_cast<f64>(stacks));
        }

        build_rings(vertices, indices, polar, slices,
                    [](f64vec3 const& direction) { return f32vec3{direction * 0.5}; });
    }

    void build_planet(vector<Vertex>& vertices, vector<u32>& indices, f64 radius, u32 slices,
                      f64 first_ring, f64 ring_growth)
    {
        // The last ring stops short of the south pole by at least a step, so
        // no ring collapses onto the pole vertex.
        vector<f64> polar;
        for (f64 arc = first_ring; arc < kPi * radius / ring_growth; arc *= ring_growth)
        {
            polar.push_back(arc / radius);
        }

        // Composed in f64 relative to the pole, then narrowed: see mesh.hpp.
        build_rings(vertices, indices, polar, slices, [radius](f64vec3 const& direction) {
            return f32vec3{direction * radius - f64vec3{0.0, radius, 0.0}};
        });
    }

    bool Mesh::init(VulkanAllocator const& allocator, VulkanDevice const& device,
                    span<Vertex const> vertices, span<u32 const> indices)
    {
        if (vertices.empty() || indices.empty())
        {
            log::error("mesh needs both vertices and indices");
            return false;
        }

        if (!vertices_.init_device(allocator, device, vertices.size() * sizeof(Vertex),
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data()))
        {
            return false;
        }

        if (!indices_.init_device(allocator, device, indices.size() * sizeof(u32),
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data()))
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
