#pragma once

#include <memory>

namespace encke::terrain
{
    // The detail layer: fBm from metres to hundreds of metres, summed by hand
    // one Perlin evaluation per octave rather than through FastNoise2's FBm
    // node. Each octave gets its own lattice split, and a split cannot be
    // scaled from one octave to the next: floor(o * f * L) is not
    // L * floor(o * f).
    struct DetailSpec
    {
        u32 octave_count       = 12;
        f64 largest_wavelength = 512.0;    // metres, octave 0
        // Slightly off 2, so no octave's lattice lines up with another's.
        f64 lacunarity         = 2.0137;
    };

    // One octave: noise at world position p is Perlin(rotation * p * frequency),
    // in lattice cells. The rotation is fixed per octave, so the axis-aligned
    // grain of each lattice points a different way.
    struct Octave
    {
        f64mat3 rotation{1.0};
        f64     frequency = 0.0;   // lattice cells per metre
        i32     seed      = 0;
    };

    // Deterministic across compilers and platforms: the rotations are rational
    // (integer quaternions, divided once), and the frequencies are repeated
    // multiplication, so no libm call decides a bit of either.
    vector<Octave> make_octaves(DetailSpec const& spec, i32 seed);

    // How many of the leading octaves have a wavelength of at least two
    // voxels. Finer octaves alias at that voxel size, so they are skipped.
    u32 octaves_for_voxel(span<Octave const> octaves, f64 voxel_size);

    // Splits an octave's lattice position about an f64 origin: the integer
    // cell goes to the hash as an int32 offset, the remainder stays a small
    // float. Wraps modulo 2^32, as the hash does anyway.
    struct LatticeSplit
    {
        i32vec3 base{0};
        f32vec3 fraction{0.0f};
    };
    LatticeSplit split_lattice(Octave const& octave, f64vec3 const& origin);

    // Evaluates octaves over a set of points given as small f32 offsets, in
    // metres, from one f64 origin. One instance per thread: it owns a
    // FastNoise2 node whose lattice offset it changes per octave, plus scratch.
    class DetailNoise
    {
    public:
        DetailNoise(DetailSpec const& spec, i32 seed);
        ~DetailNoise();

        DetailNoise(DetailNoise const&)            = delete;
        DetailNoise& operator=(DetailNoise const&) = delete;

        span<Octave const> octaves() const { return octaves_; }

        // Octave `index` at origin + (x[i], y[i], z[i]), raw Perlin in about
        // [-1, 1], into out[i].
        void octave(u32 index, f64vec3 const& origin,
                    span<f32 const> x, span<f32 const> y, span<f32 const> z,
                    span<f32> out);

    private:
        struct Nodes;

        vector<Octave>         octaves_;
        std::unique_ptr<Nodes> nodes_;
        vector<f32>            scratch_x_;
        vector<f32>            scratch_y_;
        vector<f32>            scratch_z_;
    };
}
