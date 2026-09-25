#pragma once

#include "terrain/terrain_field.hpp"

namespace encke::terrain
{
    // A chunk's surface: one vertex per cell the surface crosses, at the
    // average of the crossings on the cell's edges, and a quad over every
    // edge whose ends differ in sign. Negative is inside.
    //
    // Ownership, which is what makes chunks meet without gaps or overlaps: a
    // chunk owns corners 0 to cells - 1 on each axis, and the quad of every
    // edge that starts at an owned corner and runs along +x, +y or +z. The
    // four cells around such an edge lie in -1 to cells - 1, so vertices are
    // placed in those; the ones in cell -1 repeat the neighbour's own, from
    // the same samples.
    //
    // Positions are metres from the chunk's corner 0, in f32. Normals are the
    // field's gradient -- central differences at the cell's corners,
    // interpolated trilinearly at the vertex -- normalised.
    //
    // Given the parent LOD's field, each vertex also gets a morph target: the
    // vertex the parent LOD's mesher places in the parent cell holding this
    // vertex's cell, with its normal. The parent chunk computes the same
    // vertex from the same values in the same order, so a fully morphed mesh
    // lands on the parent's own vertices, and collapses onto its surface
    // wherever the two agree on which parent cells the surface crosses.
    // Where the parent's cell has no vertex, the target is the vertex itself.
    struct SurfaceMesh
    {
        vector<f32vec3> positions;
        vector<f32vec3> normals;
        // Each vertex's cell, from -1 to cells - 1: with the chunk's grid
        // origin, it names the vertex across chunks.
        vector<i32vec3> cells;
        // Triangles, counter-clockwise seen from outside, two per quad.
        vector<u32>     indices;

        // Morph targets, one per vertex; empty without the parent's field.
        vector<f32vec3> coarse_positions;
        vector<f32vec3> coarse_normals;

        void clear()
        {
            positions.clear();
            normals.clear();
            cells.clear();
            indices.clear();
            coarse_positions.clear();
            coarse_normals.clear();
        }
    };

    // Meshes (cells + 4)^3 samples laid out as ChunkRequest describes:
    // sample s on an axis is corner s - 2, x fastest. With `coarse`, laid out
    // as sample_chunk writes it for the same chunk, also the morph targets.
    void surface_nets(span<f32 const> samples, u32 cells, f64 voxel_size, SurfaceMesh& out,
                      CoarseSamples const* coarse = nullptr);
}
