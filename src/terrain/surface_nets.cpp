#include "core/pch.hpp"

#include "terrain/surface_nets.hpp"

#include "terrain/terrain_field.hpp"

#include <limits>

namespace encke::terrain
{
    namespace
    {
        constexpr u32 kNoVertex = std::numeric_limits<u32>::max();

        // A cell's corners, numbered by bits: x in bit 0, y in 1, z in 2.
        // const, not constexpr: no GLM type is constexpr under GLM_FORCE_AVX2.
        array<i32vec3, 8> const kCorners{{
            {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
            {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1},
        }};

        // Its twelve edges, as pairs of corners differing in one bit.
        array<u8vec2, 12> const kEdges{{
            {0, 1}, {2, 3}, {4, 5}, {6, 7},
            {0, 2}, {1, 3}, {4, 6}, {5, 7},
            {0, 4}, {1, 5}, {2, 6}, {3, 7},
        }};
    }

    void surface_nets(span<f32 const> samples, u32 cells, f64 voxel_size, SurfaceMesh& out)
    {
        out.clear();

        i32 const n    = static_cast<i32>(cells);
        i32 const axis = n + 4;

        // Corner (x, y, z), each from -2 to cells + 1.
        auto const value = [&](i32 x, i32 y, i32 z) {
            return samples[static_cast<size_t>(((z + 2) * axis + (y + 2)) * axis + (x + 2))];
        };
        auto const gradient = [&](i32vec3 const& c) {
            return f32vec3{
                central_difference(value(c.x - 1, c.y, c.z), value(c.x + 1, c.y, c.z), voxel_size),
                central_difference(value(c.x, c.y - 1, c.z), value(c.x, c.y + 1, c.z), voxel_size),
                central_difference(value(c.x, c.y, c.z - 1), value(c.x, c.y, c.z + 1), voxel_size),
            };
        };

        // Vertex cells run from -1 to cells - 1.
        i32 const span_cells = n + 1;
        auto const cell_slot = [&](i32 x, i32 y, i32 z) {
            return static_cast<size_t>(((z + 1) * span_cells + (y + 1)) * span_cells + (x + 1));
        };
        size_t const slots = static_cast<size_t>(span_cells);
        vector<u32>  vertex_of(slots * slots * slots, kNoVertex);

        f32 const voxel = static_cast<f32>(voxel_size);

        for (i32 z = -1; z < n; ++z)
        {
            for (i32 y = -1; y < n; ++y)
            {
                for (i32 x = -1; x < n; ++x)
                {
                    array<f32, 8> v{};
                    u32           inside = 0;
                    for (u32 c = 0; c < 8; ++c)
                    {
                        v[c] = value(x + kCorners[c].x, y + kCorners[c].y, z + kCorners[c].z);
                        if (v[c] < 0.0f)
                        {
                            inside |= 1u << c;
                        }
                    }
                    if (inside == 0 || inside == 0xFF)
                    {
                        continue;
                    }

                    // The average of the crossings, in the cell's [0, 1]^3.
                    f32vec3 sum{0.0f};
                    f32     crossings = 0.0f;
                    for (u8vec2 const& edge : kEdges)
                    {
                        bool const a_inside = ((inside >> edge.x) & 1u) != 0;
                        bool const b_inside = ((inside >> edge.y) & 1u) != 0;
                        if (a_inside == b_inside)
                        {
                            continue;
                        }
                        f32 const     t = v[edge.x] / (v[edge.x] - v[edge.y]);
                        f32vec3 const a{kCorners[edge.x]};
                        f32vec3 const b{kCorners[edge.y]};
                        sum       += a + t * (b - a);
                        crossings += 1.0f;
                    }
                    f32vec3 const local = sum / crossings;

                    // The corners' gradients, trilinear at the vertex.
                    i32vec3 const cell{x, y, z};
                    auto const    g = [&](u32 c) { return gradient(cell + kCorners[c]); };
                    f32vec3 const x00 = glm::mix(g(0), g(1), local.x);
                    f32vec3 const x10 = glm::mix(g(2), g(3), local.x);
                    f32vec3 const x01 = glm::mix(g(4), g(5), local.x);
                    f32vec3 const x11 = glm::mix(g(6), g(7), local.x);
                    f32vec3 const normal = glm::mix(glm::mix(x00, x10, local.y), glm::mix(x01, x11, local.y), local.z);
                    f32 const     length = glm::length(normal);

                    vertex_of[cell_slot(x, y, z)] = static_cast<u32>(out.positions.size());
                    out.positions.push_back((f32vec3{cell} + local) * voxel);
                    out.normals.push_back(length > 0.0f ? normal / length : f32vec3{0.0f, 1.0f, 0.0f});
                    out.cells.push_back(cell);
                }
            }
        }

        // Every owned edge that changes sign: a quad over the four cells
        // around it, counter-clockwise about the direction out of the surface.
        for (glm::length_t a = 0; a < 3; ++a)
        {
            glm::length_t const u = (a + 1) % 3;
            glm::length_t const w = (a + 2) % 3;

            i32vec3 step_a{0};
            i32vec3 step_u{0};
            i32vec3 step_w{0};
            step_a[a] = 1;
            step_u[u] = 1;
            step_w[w] = 1;

            for (i32 z = 0; z < n; ++z)
            {
                for (i32 y = 0; y < n; ++y)
                {
                    for (i32 x = 0; x < n; ++x)
                    {
                        i32vec3 const corner{x, y, z};
                        i32vec3 const next = corner + step_a;

                        bool const low_inside  = value(corner.x, corner.y, corner.z) < 0.0f;
                        bool const high_inside = value(next.x, next.y, next.z) < 0.0f;
                        if (low_inside == high_inside)
                        {
                            continue;
                        }

                        auto const vertex = [&](i32vec3 const& c) { return vertex_of[cell_slot(c.x, c.y, c.z)]; };
                        u32 const q0 = vertex(corner - step_u - step_w);
                        u32 const q1 = vertex(corner - step_w);
                        u32 const q2 = vertex(corner);
                        u32 const q3 = vertex(corner - step_u);

                        // Inside at the low end: the surface faces +a.
                        if (low_inside)
                        {
                            out.indices.insert(out.indices.end(), {q0, q1, q2, q0, q2, q3});
                        }
                        else
                        {
                            out.indices.insert(out.indices.end(), {q0, q3, q2, q0, q2, q1});
                        }
                    }
                }
            }
        }
    }
}
