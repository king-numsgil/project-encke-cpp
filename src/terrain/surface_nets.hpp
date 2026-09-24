#pragma once

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
    struct SurfaceMesh
    {
        vector<f32vec3> positions;
        vector<f32vec3> normals;
        // Each vertex's cell, from -1 to cells - 1: with the chunk's grid
        // origin, it names the vertex across chunks.
        vector<i32vec3> cells;
        // Triangles, counter-clockwise seen from outside, two per quad.
        vector<u32>     indices;

        void clear()
        {
            positions.clear();
            normals.clear();
            cells.clear();
            indices.clear();
        }
    };

    // Meshes (cells + 4)^3 samples laid out as ChunkRequest describes:
    // sample s on an axis is corner s - 2, x fastest.
    void surface_nets(span<f32 const> samples, u32 cells, f64 voxel_size, SurfaceMesh& out);
}
