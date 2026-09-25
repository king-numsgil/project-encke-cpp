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

        struct CellVertex
        {
            f32vec3 local{0.0f};   // in the cell's [0, 1]^3
            f32vec3 normal{0.0f};  // unit
        };

        // A cell's vertex from its corners' values, or nullopt when the
        // surface misses it: the average of the crossings on its edges, and
        // the corners' gradients, from `gradient(corner)`, trilinear there.
        // Fine and parent cells both come through here, so a child computes
        // its parent's vertices with exactly the parent's operations.
        template <typename Gradient>
        optional<CellVertex> cell_vertex(array<f32, 8> const& v, Gradient const& gradient)
        {
            u32 inside = 0;
            for (u32 c = 0; c < 8; ++c)
            {
                if (v[c] < 0.0f)
                {
                    inside |= 1u << c;
                }
            }
            if (inside == 0 || inside == 0xFF)
            {
                return nullopt;
            }

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

            f32vec3 const x00    = glm::mix(gradient(0), gradient(1), local.x);
            f32vec3 const x10    = glm::mix(gradient(2), gradient(3), local.x);
            f32vec3 const x01    = glm::mix(gradient(4), gradient(5), local.x);
            f32vec3 const x11    = glm::mix(gradient(6), gradient(7), local.x);
            f32vec3 const normal = glm::mix(glm::mix(x00, x10, local.y), glm::mix(x01, x11, local.y), local.z);
            f32 const     length = glm::length(normal);

            return CellVertex{
                .local  = local,
                .normal = length > 0.0f ? normal / length : f32vec3{0.0f, 1.0f, 0.0f},
            };
        }
    }

    void surface_nets(span<f32 const> samples, u32 cells, f64 voxel_size, SurfaceMesh& out,
                      CoarseSamples const* coarse)
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
                    i32vec3 const cell{x, y, z};
                    array<f32, 8> v{};
                    for (u32 c = 0; c < 8; ++c)
                    {
                        v[c] = value(x + kCorners[c].x, y + kCorners[c].y, z + kCorners[c].z);
                    }
                    optional<CellVertex> const vertex =
                        cell_vertex(v, [&](u32 c) { return gradient(cell + kCorners[c]); });
                    if (!vertex.has_value())
                    {
                        continue;
                    }

                    vertex_of[cell_slot(x, y, z)] = static_cast<u32>(out.positions.size());
                    out.positions.push_back((f32vec3{cell} + vertex->local) * voxel);
                    out.normals.push_back(vertex->normal);
                    out.cells.push_back(cell);
                }
            }
        }

        if (coarse != nullptr)
        {
            // Parent cells -1 to cells / 2 - 1 hold every vertex cell; their
            // corners, -1 to cells / 2, are the coarse points, offset by one.
            i32 const  parent_cells = n / 2 + 1;
            i32 const  coarse_axis  = n / 2 + 2;
            auto const coarse_index = [&](i32vec3 const& q) {
                return static_cast<size_t>(((q.z + 1) * coarse_axis + (q.y + 1)) * coarse_axis + (q.x + 1));
            };
            f32 const parent_voxel = static_cast<f32>(2.0 * voxel_size);

            // Each parent cell's vertex once, on first need.
            size_t const                 parent_slots = static_cast<size_t>(parent_cells);
            vector<u8>                   known(parent_slots * parent_slots * parent_slots, 0);
            vector<optional<CellVertex>> parents(known.size());

            out.coarse_positions.resize(out.positions.size());
            out.coarse_normals.resize(out.positions.size());
            for (size_t i = 0; i < out.positions.size(); ++i)
            {
                // Floor division by two, so cell -1 is in parent cell -1.
                i32vec3 const parent = (out.cells[i] + i32vec3{2}) / 2 - i32vec3{1};
                size_t const  slot   = static_cast<size_t>(((parent.z + 1) * parent_cells + (parent.y + 1)) * parent_cells +
                                                           (parent.x + 1));
                if (known[slot] == 0)
                {
                    array<f32, 8> v{};
                    for (u32 c = 0; c < 8; ++c)
                    {
                        v[c] = coarse->values[coarse_index(parent + kCorners[c])];
                    }
                    parents[slot] = cell_vertex(v, [&](u32 c) { return coarse->gradients[coarse_index(parent + kCorners[c])]; });
                    known[slot]   = 1;
                }

                if (optional<CellVertex> const& target = parents[slot])
                {
                    out.coarse_positions[i] = (f32vec3{parent} + target->local) * parent_voxel;
                    out.coarse_normals[i]   = target->normal;
                }
                else
                {
                    out.coarse_positions[i] = out.positions[i];
                    out.coarse_normals[i]   = out.normals[i];
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
