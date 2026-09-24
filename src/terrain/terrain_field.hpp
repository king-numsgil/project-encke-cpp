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

    // A chunk of `cells`^3 voxels at `origin`, sampled on a (cells + 2)^3
    // grid: sample (i, j, k) sits at origin + (i - 1, j - 1, k - 1) *
    // voxel_size, x fastest. The extra sample on each side is there for
    // central differences. Origins should be whole multiples of the voxel
    // size.
    //
    // `detail_octaves` is normally octaves_for_voxel at this voxel size. A
    // fine chunk sampling the face it shares with a coarser one passes the
    // coarser chunk's count there, so both see the same field.
    struct ChunkRequest
    {
        f64vec3 origin{0.0};
        f64     voxel_size     = 0.0;
        u32     cells          = 32;
        u32     detail_octaves = 0;

        u32 samples_per_axis() const { return cells + 2; }
        u32 sample_count() const { return samples_per_axis() * samples_per_axis() * samples_per_axis(); }
    };

    // The value, and its gradient by central differences one voxel wide:
    // the same differences a mesher takes over a chunk's samples.
    struct PointSample
    {
        f32     value = 0.0f;
        f32vec3 gradient{0.0f};
    };

    // Seconds spent in each layer by the last call.
    struct LayerTiming
    {
        f64 macro  = 0.0;
        f64 detail = 0.0;
    };

    // Samples one body's terrain. One instance per thread: it owns
    // FastNoise2 nodes and scratch buffers. The chunk and point paths run the
    // same code over different sets of points, so they agree to float
    // rounding.
    class TerrainSampler
    {
    public:
        explicit TerrainSampler(BodyTerrain const& terrain);

        BodyTerrain const& terrain() const { return terrain_; }
        span<Octave const> octaves() const { return detail_.octaves(); }

        ChunkRequest chunk(f64vec3 const& origin, u32 lod) const;

        // Fills out, which must hold request.sample_count() values.
        void sample_chunk(ChunkRequest const& request, span<f32> out);

        // At p, with the voxel size and octave count a chunk there would use.
        PointSample sample_point(f64vec3 const& p, f64 voxel_size, u32 detail_octaves);

        LayerTiming const& last_timing() const { return timing_; }

    private:
        // The field at origin + (x[i], y[i], z[i]).
        void evaluate(f64vec3 const& origin, f64 voxel_size, u32 detail_octaves,
                      span<f32 const> x, span<f32 const> y, span<f32 const> z,
                      span<f32> out);

        BodyTerrain  terrain_;
        DetailNoise  detail_;
        MacroField   macro_;
        MacroValues  macro_values_;
        LayerTiming  timing_;

        vector<f32> offset_x_;
        vector<f32> offset_y_;
        vector<f32> offset_z_;
        vector<f32> octave_values_;
        vector<f32> detail_sum_;
        vector<f32> weight_;
    };
}
