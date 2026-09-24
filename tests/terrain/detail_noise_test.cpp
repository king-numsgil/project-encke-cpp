#include "core/pch.hpp"

#include "terrain/detail_noise.hpp"
#include "terrain/fastnoise.hpp"

#include "perlin_reference.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>

namespace encke::terrain
{
    namespace
    {
        constexpr i32 kSeed = 1337;

        // SIMD f32 against the f64 oracle, per octave, on noise in [-1, 1].
        // What separates them is f32 rounding of positions a few cells
        // across and of the arithmetic, both around 1e-6.
        constexpr f64 kTolerance = 1e-4;

        // About the Earth's radius, off every axis.
        f64vec3 far_origin()
        {
            return glm::normalize(f64vec3{0.53, 0.71, -0.46}) * 6.4e6;
        }

        struct Offsets
        {
            vector<f32> x;
            vector<f32> y;
            vector<f32> z;
        };

        // Scattered through a 34-voxel cube at LOD 0's 0.25 m voxels.
        Offsets random_offsets(size_t count)
        {
            std::mt19937_64                   random{42};
            std::uniform_real_distribution<f32> within{-0.25f, 8.25f};

            Offsets offsets;
            for (size_t i = 0; i < count; ++i)
            {
                offsets.x.push_back(within(random));
                offsets.y.push_back(within(random));
                offsets.z.push_back(within(random));
            }
            return offsets;
        }

        // Largest difference over every octave between DetailNoise at origin
        // + offsets and the oracle there.
        f64 worst_error(f64vec3 const& origin)
        {
            DetailNoise   noise{DetailSpec{}, kSeed};
            Offsets const offsets = random_offsets(257);
            vector<f32>   values(offsets.x.size());

            f64 worst = 0.0;
            for (u32 index = 0; index < noise.octaves().size(); ++index)
            {
                noise.octave(index, origin, offsets.x, offsets.y, offsets.z, values);
                for (size_t i = 0; i < values.size(); ++i)
                {
                    f64vec3 const position = origin + f64vec3{offsets.x[i], offsets.y[i], offsets.z[i]};
                    f64 const     expected = reference::octave(noise.octaves()[index], position);
                    worst = std::max(worst, std::abs(static_cast<f64>(values[i]) - expected));
                }
            }
            return worst;
        }

        // FastNoise2 fed f32 absolute lattice positions, as it would be
        // without the split: the path that breaks far from the origin.
        f64 worst_naive_error(f64vec3 const& origin, u32 octave_index)
        {
            vector<Octave> const octaves = make_octaves(DetailSpec{}, kSeed);
            Octave const&        octave  = octaves[octave_index];

            auto perlin = FastNoise::New<FastNoise::Perlin>(kFeatureSet);
            perlin->SetScale(1.0f);

            Offsets const offsets = random_offsets(257);
            f64           worst   = 0.0;
            for (size_t i = 0; i < offsets.x.size(); ++i)
            {
                f64vec3 const position = origin + f64vec3{offsets.x[i], offsets.y[i], offsets.z[i]};
                f32vec3 const lattice{(octave.rotation * position) * octave.frequency};
                f32 const     value = perlin->GenSingle3D(lattice.x, lattice.y, lattice.z, octave.seed);
                worst = std::max(worst, std::abs(static_cast<f64>(value) - reference::octave(octave, position)));
            }
            return worst;
        }

        // The last octave: 512 m / 2.0137^11, about 23 cm.
        constexpr u32 kFinestOctave = 11;
    }

    TEST_CASE("the lattice offset shifts the hashed cell and nothing else", "[terrain]")
    {
        auto shifted = FastNoise::New<FastNoise::Perlin>(kFeatureSet);
        auto plain   = FastNoise::New<FastNoise::Perlin>(kFeatureSet);
        REQUIRE(shifted);
        REQUIRE(plain);
        shifted->SetScale(1.0f);
        plain->SetScale(1.0f);

        shifted->SetLatticeOffset(3, -5, 7);

        // Positions whose fractions are exact in f32 either way, so the two
        // must agree bit for bit.
        for (f32 const t : {0.0f, 0.125f, 0.25f, 0.5f, 0.875f})
        {
            f32 const a = shifted->GenSingle3D(t, 0.5f - t, 0.75f, kSeed);
            f32 const b = plain->GenSingle3D(3.0f + t, -5.0f + (0.5f - t), 7.75f, kSeed);
            CHECK(a == b);
        }
    }

    TEST_CASE("octave frequencies and rotations are exact and deterministic", "[terrain]")
    {
        vector<Octave> const octaves = make_octaves(DetailSpec{}, kSeed);
        REQUIRE(octaves.size() == DetailSpec{}.octave_count);

        for (Octave const& octave : octaves)
        {
            f64mat3 const identity = glm::transpose(octave.rotation) * octave.rotation;
            for (glm::length_t c = 0; c < 3; ++c)
            {
                for (glm::length_t r = 0; r < 3; ++r)
                {
                    CHECK(std::abs(identity[c][r] - (c == r ? 1.0 : 0.0)) < 1e-15);
                }
            }
            CHECK(glm::determinant(octave.rotation) > 0.0);

            // A power of two of base voxels, at most sixteen wavelengths
            // wide, and more than eight.
            i64 const block = anchor_block(octave, 0.25);
            CHECK(std::has_single_bit(static_cast<u64>(block)));
            CHECK(static_cast<f64>(block) * 0.25 <= 16.0 / octave.frequency);
            CHECK(static_cast<f64>(block) * 0.25 > 8.0 / octave.frequency);
        }

        // Two builds of the same spec are the same bits.
        vector<Octave> const again = make_octaves(DetailSpec{}, kSeed);
        for (size_t i = 0; i < octaves.size(); ++i)
        {
            CHECK(octaves[i].rotation == again[i].rotation);
            CHECK(octaves[i].frequency == again[i].frequency);
            CHECK(octaves[i].seed == again[i].seed);
        }
    }

    TEST_CASE("octaves under two voxels are cut", "[terrain]")
    {
        vector<Octave> const octaves = make_octaves(DetailSpec{}, kSeed);

        u32 const at_lod0 = octaves_for_voxel(octaves, 0.25);
        u32 const at_lod4 = octaves_for_voxel(octaves, 4.0);
        CHECK(at_lod0 == 10);
        CHECK(at_lod4 == 6);

        for (u32 i = 0; i < at_lod4; ++i)
        {
            CHECK(1.0 / octaves[i].frequency >= 8.0);
        }
        CHECK(1.0 / octaves[at_lod4].frequency < 8.0);
    }

    TEST_CASE("detail noise matches the f64 reference near the origin", "[terrain]")
    {
        f64 const worst = worst_error(f64vec3{3.5, -12.25, 40.0});
        CAPTURE(worst);
        CHECK(worst < kTolerance);
    }

    TEST_CASE("detail noise matches the f64 reference at 6.4e6 m", "[terrain]")
    {
        f64 const worst = worst_error(far_origin());
        CAPTURE(worst);
        CHECK(worst < kTolerance);
    }

    TEST_CASE("naive f32 positions match near the origin and fail at 6.4e6 m", "[terrain]")
    {
        // Near the origin f32 holds the position fine, so the naive path is
        // as good as the split: the comparison itself is fair.
        f64 const near = worst_naive_error(f64vec3{3.5, -12.25, 40.0}, kFinestOctave);
        CAPTURE(near);
        CHECK(near < kTolerance);

        // At 6.4e6 m the finest octave is 2.8e7 cells out, where f32 steps in
        // whole cells: the noise is unrelated to what it should be.
        f64 const far = worst_naive_error(far_origin(), kFinestOctave);
        CAPTURE(far);
        CHECK(far > 0.25);
    }
}
