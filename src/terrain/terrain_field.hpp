#pragma once

#include "terrain/detail_noise.hpp"
#include "terrain/macro_field.hpp"

#include <cmath>

namespace encke::terrain
{
    // A body's terrain: a planet or an asteroid, one description for both.
    // Everything is body-relative, centred on the body, f64 metres.
    //
    // The field is a signed height distance: |p| - radius - height(p), where
    // height is the macro height plus the detail fBm scaled by the macro
    // amplitude. Negative inside.
    struct BodyTerrain
    {
        f64        radius = 0.0;
        i32        seed   = 0;
        // LOD 0's voxel edge; LOD n's is this times 2^n. A power of two, as
        // MacroSpec explains.
        f64        base_voxel_size = 0.25;
        DetailSpec detail{};
        MacroSpec  macro{};

        f64 voxel_size(u32 lod) const { return std::ldexp(base_voxel_size, static_cast<int>(lod)); }
    };

    // An Earth-radius planet with an FBm graph in every macro channel, for
    // the benchmark and tests until bodies carry authored graphs.
    BodyTerrain example_planet(i32 seed);

    // Grid coordinates are integers in base voxels, body-relative: point g is
    // at g * base_voxel_size metres. Every LOD's samples are grid points, so a
    // point's grid coordinate names it the same way at every LOD.
    //
    // A chunk of `cells`^3 voxels of 2^lod base voxels each, its corner at
    // grid point `origin`, sampled on a (cells + 4)^3 grid: sample (i, j, k)
    // is grid point origin + (i - 2, j - 2, k - 2) * 2^lod, x fastest.
    //
    // The size follows from the mesher (terrain/surface_nets). A chunk owns
    // corners 0 to cells - 1 on each axis and emits the quad of every edge
    // starting at one; the quad joins the four cells around the edge, so
    // vertices are placed in cells -1 to cells - 1, whose corners run from
    // -1 to cells. A central difference at each of those needs one sample
    // either side: -2 to cells + 1, cells + 4 samples.
    //
    // `detail_octaves` is normally octaves_for_voxel at this LOD. A fine chunk
    // sampling the face it shares with a coarser one may pass the coarser
    // chunk's count, and then sees the same field there, bit for bit.
    //
    // `coarse_octaves` is the next-coarser LOD's count, for the coarse output
    // of sample_chunk.
    struct ChunkRequest
    {
        i64vec3 origin{0};
        u32     lod            = 0;
        u32     cells          = 32;
        u32     detail_octaves = 0;
        u32     coarse_octaves = 0;

        u32 samples_per_axis() const { return cells + 4; }
        u32 sample_count() const { return samples_per_axis() * samples_per_axis() * samples_per_axis(); }

        // The samples at even offsets from the origin, 0, 2, ..., cells: the
        // next-coarser LOD's grid points inside the chunk.
        u32 coarse_per_axis() const { return cells / 2 + 1; }
        u32 coarse_count() const { return coarse_per_axis() * coarse_per_axis() * coarse_per_axis(); }
    };

    // The field at a chunk's even samples with the detail cut at
    // coarse_octaves, and its gradient by central differences across the
    // coarse voxel: what the parent LOD's chunk samples there, and the
    // gradient a mesher takes over the parent's samples. For geomorphing
    // toward the parent. Point (i, j, k) is chunk sample (2i + 2, 2j + 2,
    // 2k + 2), x fastest.
    struct CoarseSamples
    {
        span<f32>     values;      // request.coarse_count() each
        span<f32vec3> gradients;
    };

    // (plus - minus) over the two voxels between them. Chunk consumers take
    // gradients through this so theirs match the sampler's to the bit.
    inline f32 central_difference(f32 minus, f32 plus, f64 voxel_size)
    {
        return (plus - minus) / static_cast<f32>(2.0 * voxel_size);
    }

    // The value, and its gradient by central differences one voxel wide:
    // the same differences a mesher takes over a chunk's samples.
    struct PointSample
    {
        f32     value = 0.0f;
        f32vec3 gradient{0.0f};
    };

    // Seconds spent by the last call: in each layer over the chunk's own
    // samples, and on the coarse output (the parent grid points past the
    // chunk's far faces, and the gradients).
    struct LayerTiming
    {
        f64 macro  = 0.0;
        f64 detail = 0.0;
        f64 coarse = 0.0;
    };

    // Samples one body's terrain. One instance per thread: it owns
    // FastNoise2 nodes and scratch buffers.
    //
    // Chunk samples are bit-exact: a grid point has one value whichever
    // chunk, at whichever LOD, samples it, given the same octave count. Point
    // queries take arbitrary positions and so split each octave about the
    // point itself; they agree with chunks to f32 rounding.
    class TerrainSampler
    {
    public:
        explicit TerrainSampler(BodyTerrain const& terrain);

        BodyTerrain const& terrain() const { return terrain_; }
        span<Octave const> octaves() const { return detail_.octaves(); }

        ChunkRequest chunk(i64vec3 const& origin, u32 lod) const;

        // Fills out, which must hold request.sample_count() values, and the
        // coarse output if given.
        void sample_chunk(ChunkRequest const& request, span<f32> out, CoarseSamples const* coarse = nullptr);

        // At p, with the voxel size and octave count a chunk there would use.
        PointSample sample_point(f64vec3 const& p, f64 voxel_size, u32 detail_octaves);

        // sample_point's value alone, a seventh of the work.
        f32 sample_value(f64vec3 const& p, f64 voxel_size, u32 detail_octaves);

        LayerTiming const& last_timing() const { return timing_; }

    private:
        // A box of grid points: first + (i, j, k) * stride.
        struct GridBox
        {
            i64vec3 first{0};
            i64     stride = 1;
            u32vec3 count{0};

            size_t size() const { return static_cast<size_t>(count.x) * count.y * count.z; }
        };

        // Samples one run of a box axis that falls in one anchor block.
        struct Run
        {
            i64 block = 0;
            u32 first = 0;
            u32 last  = 0;
        };

        // The field over a box, octaves in block-anchored splits. With a
        // snapshot, also the field with the detail cut at snapshot_octaves.
        LayerTiming evaluate_box(GridBox const& box, u32 detail_octaves,
                                 u32 snapshot_octaves, span<f32> snapshot,
                                 span<f32> out);

        // One octave over a box, block by block, into octave_values_.
        void box_octave(GridBox const& box, u32 octave);

        // The macro layer, the detail octaves and the composition, over the
        // points origin + (x[i], y[i], z[i]). fill_octave(k) leaves octave k's
        // noise at every point in octave_values_.
        LayerTiming combine(f64vec3 const& origin, f64 voxel_size,
                            span<f32 const> x, span<f32 const> y, span<f32 const> z,
                            u32 detail_octaves, function<void(u32)> const& fill_octave,
                            u32 snapshot_octaves, span<f32> snapshot,
                            span<f32> out);

        void axis_runs(i64 first, i64 stride, u32 count, i64 block, vector<Run>& runs) const;

        BodyTerrain  terrain_;
        DetailNoise  detail_;
        MacroField   macro_;
        vector<i64>  anchor_blocks_;
        MacroValues  macro_values_;
        LayerTiming  timing_;

        vector<f32> offset_x_;
        vector<f32> offset_y_;
        vector<f32> offset_z_;
        vector<f32> octave_values_;
        vector<f32> detail_sum_;
        vector<f32> weight_;

        vector<Run>    runs_x_;
        vector<Run>    runs_y_;
        vector<Run>    runs_z_;
        vector<f32>    block_x_;
        vector<f32>    block_y_;
        vector<f32>    block_z_;
        vector<f32>    block_values_;
        vector<size_t> block_index_;

        vector<f32> partial_;
        vector<f32> coarse_grid_;
        vector<f32> ring_;
    };
}
