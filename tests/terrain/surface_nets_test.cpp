#include "core/pch.hpp"

#include "terrain/surface_nets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace encke::terrain
{
    namespace
    {
        // A sphere off every grid line, in voxels of 1 m.
        struct Sphere
        {
            f64vec3 centre{3.7, -2.2, 5.1};
            f64     radius = 20.3;

            f64 distance(f64vec3 const& p) const { return glm::length(p - centre) - radius; }
        };

        // A chunk's (cells + 4)^3 samples of the sphere, sample s at corner
        // s - 2 from the origin.
        vector<f32> sample_sphere(Sphere const& sphere, i32vec3 const& origin, u32 cells)
        {
            i32 const   axis = static_cast<i32>(cells) + 4;
            vector<f32> samples;
            size_t const count = static_cast<size_t>(axis);
            samples.reserve(count * count * count);
            for (i32 z = 0; z < axis; ++z)
            {
                for (i32 y = 0; y < axis; ++y)
                {
                    for (i32 x = 0; x < axis; ++x)
                    {
                        samples.push_back(static_cast<f32>(sphere.distance(f64vec3{origin + i32vec3{x, y, z} - i32vec3{2}})));
                    }
                }
            }
            return samples;
        }

        // A chunk's coarse field as sample_chunk lays it out: the sphere at
        // the parent's corners -1 to cells / 2, two voxels apart, and its
        // central differences across them.
        struct Coarse
        {
            vector<f32>     values;
            vector<f32vec3> gradients;
        };

        Coarse sample_coarse(Sphere const& sphere, i32vec3 const& origin, u32 cells)
        {
            // Corners -2 to cells / 2 + 1, for the differences.
            i32 const  wide  = static_cast<i32>(cells) / 2 + 4;
            auto const value = [&](i32vec3 const& q) {
                return static_cast<f32>(sphere.distance(f64vec3{origin + q * 2}));
            };

            i32 const axis = wide - 2;
            Coarse    coarse;
            for (i32 z = -1; z < axis - 1; ++z)
            {
                for (i32 y = -1; y < axis - 1; ++y)
                {
                    for (i32 x = -1; x < axis - 1; ++x)
                    {
                        i32vec3 const q{x, y, z};
                        coarse.values.push_back(value(q));
                        coarse.gradients.push_back(f32vec3{
                            central_difference(value(q - i32vec3{1, 0, 0}), value(q + i32vec3{1, 0, 0}), 2.0),
                            central_difference(value(q - i32vec3{0, 1, 0}), value(q + i32vec3{0, 1, 0}), 2.0),
                            central_difference(value(q - i32vec3{0, 0, 1}), value(q + i32vec3{0, 0, 1}), 2.0),
                        });
                    }
                }
            }
            return coarse;
        }

        // A coarse chunk's (cells + 4)^3 samples, two voxels apart.
        vector<f32> sample_sphere_coarse(Sphere const& sphere, i32vec3 const& origin, u32 cells)
        {
            i32 const   axis = static_cast<i32>(cells) + 4;
            vector<f32> samples;
            for (i32 z = 0; z < axis; ++z)
            {
                for (i32 y = 0; y < axis; ++y)
                {
                    for (i32 x = 0; x < axis; ++x)
                    {
                        samples.push_back(static_cast<f32>(sphere.distance(f64vec3{origin + (i32vec3{x, y, z} - i32vec3{2}) * 2})));
                    }
                }
            }
            return samples;
        }

        // Every chunk's triangles merged, vertices identified across chunks
        // by global cell.
        struct Merged
        {
            vector<f64vec3>             positions;
            vector<f32vec3>             normals;
            vector<u32>                 indices;
            std::map<array<i32, 3>, u32> ids;
            u32                         chunks_with_surface = 0;
        };

        void add_chunk(Merged& merged, SurfaceMesh const& mesh, i32vec3 const& origin)
        {
            vector<u32> local_to_global(mesh.positions.size());
            for (size_t i = 0; i < mesh.positions.size(); ++i)
            {
                i32vec3 const        cell = origin + mesh.cells[i];
                array<i32, 3> const  key{{cell.x, cell.y, cell.z}};
                auto const [found, added] = merged.ids.try_emplace(key, static_cast<u32>(merged.positions.size()));
                if (added)
                {
                    merged.positions.push_back(f64vec3{origin} + f64vec3{mesh.positions[i]});
                    merged.normals.push_back(mesh.normals[i]);
                }
                local_to_global[i] = found->second;
            }
            for (u32 const index : mesh.indices)
            {
                merged.indices.push_back(local_to_global[index]);
            }
            if (!mesh.indices.empty())
            {
                ++merged.chunks_with_surface;
            }
        }

        // Watertight and consistently wound: every directed edge appears once,
        // and so does its reverse.
        u32 open_edges(Merged const& merged)
        {
            std::map<std::pair<u32, u32>, i32> edges;
            for (size_t t = 0; t < merged.indices.size(); t += 3)
            {
                for (size_t e = 0; e < 3; ++e)
                {
                    u32 const a = merged.indices[t + e];
                    u32 const b = merged.indices[t + (e + 1) % 3];
                    ++edges[{a, b}];
                }
            }

            u32 open = 0;
            for (auto const& [edge, count] : edges)
            {
                auto const reverse = edges.find({edge.second, edge.first});
                if (count != 1 || reverse == edges.end() || reverse->second != 1)
                {
                    ++open;
                }
            }
            return open;
        }

        void check_surface(Merged const& merged, Sphere const& sphere)
        {
            REQUIRE(!merged.indices.empty());
            CHECK(open_edges(merged) == 0);

            // Normals against the analytic ones, a degree and a half.
            f64 worst = 1.0;
            for (size_t i = 0; i < merged.positions.size(); ++i)
            {
                f64vec3 const analytic = glm::normalize(merged.positions[i] - sphere.centre);
                worst = std::min(worst, glm::dot(analytic, f64vec3{merged.normals[i]}));
            }
            CAPTURE(worst);
            CHECK(worst > std::cos(1.5 * 3.14159265358979323846 / 180.0));

            // Every triangle faces out of the sphere.
            u32 inward = 0;
            for (size_t t = 0; t < merged.indices.size(); t += 3)
            {
                f64vec3 const a = merged.positions[merged.indices[t]];
                f64vec3 const b = merged.positions[merged.indices[t + 1]];
                f64vec3 const c = merged.positions[merged.indices[t + 2]];
                f64vec3 const facing = glm::cross(b - a, c - a);
                if (glm::dot(facing, (a + b + c) / 3.0 - sphere.centre) <= 0.0)
                {
                    ++inward;
                }
            }
            CHECK(inward == 0);
        }
    }

    TEST_CASE("surface nets closes a sphere inside one chunk", "[terrain][mesh]")
    {
        Sphere const   sphere{};
        u32 const      cells = 64;
        i32vec3 const  origin{-32};
        SurfaceMesh    mesh;
        surface_nets(sample_sphere(sphere, origin, cells), cells, 1.0, mesh);

        Merged merged;
        add_chunk(merged, mesh, origin);
        check_surface(merged, sphere);
    }

    TEST_CASE("surface nets chunks at one LOD close the sphere between them", "[terrain][mesh]")
    {
        Sphere const sphere{};
        u32 const    cells = 16;

        // Every chunk of the grid the sphere touches, and one more each way,
        // which must contribute nothing.
        Merged      merged;
        SurfaceMesh mesh;
        for (i32 z = -3; z <= 2; ++z)
        {
            for (i32 y = -3; y <= 2; ++y)
            {
                for (i32 x = -3; x <= 2; ++x)
                {
                    i32vec3 const origin = i32vec3{x, y, z} * static_cast<i32>(cells);
                    surface_nets(sample_sphere(sphere, origin, cells), cells, 1.0, mesh);
                    add_chunk(merged, mesh, origin);
                }
            }
        }

        // The sphere spans several chunks on every axis, so the seams are
        // exercised on every face orientation.
        CHECK(merged.chunks_with_surface > 8);
        check_surface(merged, sphere);
    }

    TEST_CASE("surface nets morph targets are the parent chunk's own vertices", "[terrain][mesh]")
    {
        Sphere const sphere{};
        u32 const    cells = 16;

        // Fine chunks in every corner of one parent chunk and across its
        // faces, so targets in cell -1, which belong to the parent's
        // neighbours, are checked too.
        for (i32vec3 const& fine_origin : {i32vec3{0, 0, 0}, i32vec3{16, 0, 0}, i32vec3{0, -16, 16}, i32vec3{-16, 0, 0}})
        {
            Coarse              coarse = sample_coarse(sphere, fine_origin, cells);
            CoarseSamples const input{coarse.values, coarse.gradients};
            SurfaceMesh         fine;
            surface_nets(sample_sphere(sphere, fine_origin, cells), cells, 1.0, fine, &input);
            REQUIRE(fine.coarse_positions.size() == fine.positions.size());

            // The parent chunks, two voxels a cell, around the fine one:
            // their vertices by global parent cell corner.
            std::map<array<i32, 3>, f64vec3> parent_vertices;
            for (i32 z = -1; z <= 1; ++z)
            {
                for (i32 y = -1; y <= 1; ++y)
                {
                    for (i32 x = -1; x <= 1; ++x)
                    {
                        i32vec3 const origin = i32vec3{x, y, z} * static_cast<i32>(2 * cells);
                        SurfaceMesh   parent;
                        surface_nets(sample_sphere_coarse(sphere, origin, cells), cells, 2.0, parent);
                        for (size_t i = 0; i < parent.positions.size(); ++i)
                        {
                            i32vec3 const corner = origin + parent.cells[i] * 2;
                            parent_vertices.try_emplace(array<i32, 3>{{corner.x, corner.y, corner.z}},
                                                        f64vec3{origin} + f64vec3{parent.positions[i]});
                        }
                    }
                }
            }

            u32 matched   = 0;
            f64 worst     = 0.0;
            for (size_t i = 0; i < fine.positions.size(); ++i)
            {
                i32vec3 const parent_cell = (fine.cells[i] + i32vec3{2}) / 2 - i32vec3{1};
                i32vec3 const corner      = fine_origin + parent_cell * 2;
                auto const    found       = parent_vertices.find(array<i32, 3>{{corner.x, corner.y, corner.z}});
                if (found == parent_vertices.end())
                {
                    continue;
                }
                ++matched;
                worst = std::max(worst, glm::length(f64vec3{fine_origin} + f64vec3{fine.coarse_positions[i]} - found->second));
            }
            CAPTURE(fine_origin.x, fine_origin.y, fine_origin.z, matched, worst);
            CHECK(matched > 100);
            CHECK(worst < 1e-5);
        }
    }
}
