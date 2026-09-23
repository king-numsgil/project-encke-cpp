#include "core/pch.hpp"

#include "render/mesh.hpp"

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

        // Rings of `slices + 1` vertices, one ring per polar angle in `polar`
        // (strictly between the poles), capped at each pole. The last column
        // repeats the first in position but not in texture coordinate, and
        // each pole is one vertex per slice, at the middle of its column, so
        // a texture neither wraps backwards across the seam nor converges on
        // a single u. `make(direction, theta, phi)` builds the vertex for a
        // unit direction from the centre at polar angle theta, azimuth phi.
        template<class Make>
        void build_rings(vector<Vertex>& vertices, vector<u32>& indices, span<f64 const> polar,
                         u32 slices, Make const& make)
        {
            vertices.clear();
            indices.clear();

            auto phi_at = [slices](f64 column) {
                return 2.0 * kPi * column / static_cast<f64>(slices);
            };

            for (u32 slice = 0; slice < slices; ++slice)
            {
                f64 const phi = phi_at(static_cast<f64>(slice) + 0.5);
                vertices.push_back(make(f64vec3{0.0, 1.0, 0.0}, 0.0, phi));
            }

            for (f64 const theta : polar)
            {
                for (u32 column = 0; column <= slices; ++column)
                {
                    f64 const phi = phi_at(static_cast<f64>(column));
                    vertices.push_back(make(f64vec3{std::sin(theta) * std::cos(phi), std::cos(theta),
                                                    std::sin(theta) * std::sin(phi)},
                                            theta, phi));
                }
            }

            for (u32 slice = 0; slice < slices; ++slice)
            {
                f64 const phi = phi_at(static_cast<f64>(slice) + 0.5);
                vertices.push_back(make(f64vec3{0.0, -1.0, 0.0}, kPi, phi));
            }

            u32 const rings   = static_cast<u32>(polar.size());
            u32 const columns = slices + 1;
            u32 const south   = slices + rings * columns;

            auto ring_vertex = [slices, columns](u32 ring, u32 column) {
                return slices + ring * columns + column;
            };

            for (u32 slice = 0; slice < slices; ++slice)
            {
                emit_outward(vertices, indices, slice, ring_vertex(0, slice),
                             ring_vertex(0, slice + 1));
                emit_outward(vertices, indices, south + slice, ring_vertex(rings - 1, slice + 1),
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

        for (u32 face = 0; face < 6; ++face)
        {
            Face const& basis = kFaces[face];

            f32vec3 const centre = basis.normal * 0.5f;
            f32vec3 const u      = basis.u * 0.5f;
            f32vec3 const v      = basis.v * 0.5f;

            // normal = cross(u, v) for every face, so seen from outside u is
            // right and v is up: the tangent is u, and cross(normal, u) = v
            // is the top of the image, sign +1. Texture v runs down, from
            // the +v edge at 0 to the -v edge at 1.
            f32vec4 const tangent{basis.u, 1.0f};

            u32 const base = static_cast<u32>(vertices.size());

            vertices.push_back({centre - u - v, basis.normal, tangent, f32vec2{0.0f, 1.0f}});
            vertices.push_back({centre + u - v, basis.normal, tangent, f32vec2{1.0f, 1.0f}});
            vertices.push_back({centre + u + v, basis.normal, tangent, f32vec2{1.0f, 0.0f}});
            vertices.push_back({centre - u + v, basis.normal, tangent, f32vec2{0.0f, 0.0f}});

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

        // u increases westward, against phi, so the texture reads the right
        // way round from outside; the tangent is the direction it increases
        // in. Both coordinates are arc length on the unit-diameter sphere:
        // radius 0.5 times the angle.
        build_rings(vertices, indices, polar, slices,
                    [](f64vec3 const& direction, f64 theta, f64 phi) {
                        f64vec3 const tangent{std::sin(phi), 0.0, -std::cos(phi)};
                        return Vertex{
                            f32vec3{direction * 0.5},
                            f32vec3{direction},
                            f32vec4{f32vec3{tangent}, 1.0f},
                            f32vec2{f64vec2{(2.0 * kPi - phi) * 0.5, theta * 0.5}},
                        };
                    });
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
        // Planar texture coordinates (x, z): the tangent is +x laid into the
        // surface, and cross(normal, +x) at the pole is -z, the top of the
        // image, since v = z runs down it.
        build_rings(vertices, indices, polar, slices,
                    [radius](f64vec3 const& direction, f64, f64) {
                        f64vec3 const local = direction * radius - f64vec3{0.0, radius, 0.0};

                        f64vec3 across = f64vec3{1.0, 0.0, 0.0} - direction * direction.x;
                        if (glm::length(across) < 1e-6)
                        {
                            // Where the normal is +-x, a quarter of the way
                            // round the planet; any tangent will do there.
                            across = f64vec3{0.0, 0.0, 1.0};
                        }

                        return Vertex{
                            f32vec3{local},
                            f32vec3{direction},
                            f32vec4{f32vec3{glm::normalize(across)}, 1.0f},
                            f32vec2{f64vec2{local.x, local.z}},
                        };
                    });
    }

}
