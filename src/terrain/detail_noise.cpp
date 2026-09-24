#include "core/pch.hpp"

#include "terrain/detail_noise.hpp"

#include "terrain/fastnoise.hpp"

#include <cmath>
#include <stdexcept>

namespace encke::terrain
{
    namespace
    {
        // A 32-bit integer finaliser (Chris Wellons' lowbias32): spreads a
        // small counter into unrelated-looking bits.
        u32 mix(u32 x)
        {
            x ^= x >> 16;
            x *= 0x7feb352du;
            x ^= x >> 15;
            x *= 0x846ca68bu;
            x ^= x >> 16;
            return x;
        }

        // The rotation of an integer quaternion (w, x, y, z), which need not
        // be unit: every entry is a quadratic over the squared norm, so the
        // matrix is exactly orthonormal up to one rounding per entry, and no
        // square root or trigonometry is involved.
        f64mat3 rotation_from(i64 w, i64 x, i64 y, i64 z)
        {
            f64 const n = static_cast<f64>(w * w + x * x + y * y + z * z);

            auto const entry = [n](i64 value) { return static_cast<f64>(value) / n; };

            // GLM is column-major: m[column][row].
            f64mat3 m{1.0};
            m[0][0] = entry(w * w + x * x - y * y - z * z);
            m[1][0] = entry(2 * (x * y - w * z));
            m[2][0] = entry(2 * (x * z + w * y));
            m[0][1] = entry(2 * (x * y + w * z));
            m[1][1] = entry(w * w - x * x + y * y - z * z);
            m[2][1] = entry(2 * (y * z - w * x));
            m[0][2] = entry(2 * (x * z - w * y));
            m[1][2] = entry(2 * (y * z + w * x));
            m[2][2] = entry(w * w - x * x - y * y + z * z);
            return m;
        }

        i32 wrap_to_i32(f64 cell)
        {
            // Two's complement truncation: the hash multiplies the cell by a
            // prime modulo 2^32, so only the low 32 bits ever matter.
            return bit_cast<i32>(static_cast<u32>(static_cast<u64>(static_cast<i64>(cell))));
        }
    }

    vector<Octave> make_octaves(DetailSpec const& spec, i32 seed)
    {
        vector<Octave> octaves;
        octaves.reserve(spec.octave_count);

        f64 frequency = 1.0 / spec.largest_wavelength;
        u32 counter   = 0;
        for (u32 index = 0; index < spec.octave_count; ++index)
        {
            // Components in [-7, 7]; skip the zero quaternion, which has no
            // rotation.
            array<i64, 4> q{};
            do
            {
                for (i64& component : q)
                {
                    component = static_cast<i64>(mix(counter++) % 15u) - 7;
                }
            } while (q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0);

            octaves.push_back(Octave{
                .rotation  = rotation_from(q[0], q[1], q[2], q[3]),
                .frequency = frequency,
                .seed      = bit_cast<i32>(bit_cast<u32>(seed) + index * 0x9E3779B9u),
            });
            frequency *= spec.lacunarity;
        }
        return octaves;
    }

    u32 octaves_for_voxel(span<Octave const> octaves, f64 voxel_size)
    {
        u32 count = 0;
        for (Octave const& octave : octaves)
        {
            if (1.0 / octave.frequency < 2.0 * voxel_size)
            {
                break;
            }
            ++count;
        }
        return count;
    }

    LatticeSplit split_lattice(Octave const& octave, f64vec3 const& origin)
    {
        f64vec3 const scaled = (octave.rotation * origin) * octave.frequency;
        f64vec3 const cell   = glm::floor(scaled);
        return LatticeSplit{
            .base     = i32vec3{wrap_to_i32(cell.x), wrap_to_i32(cell.y), wrap_to_i32(cell.z)},
            .fraction = f32vec3{scaled - cell},
        };
    }

    i64 anchor_block(Octave const& octave, f64 base_voxel_size)
    {
        // Doubling rather than log2, so no libm call decides it.
        f64 const widest = 16.0 / octave.frequency;
        i64       edge   = 1;
        while (static_cast<f64>(edge * 2) * base_voxel_size <= widest)
        {
            edge *= 2;
        }
        return edge;
    }

    struct DetailNoise::Nodes
    {
        FastNoise::SmartNode<FastNoise::Perlin> perlin;
    };

    DetailNoise::DetailNoise(DetailSpec const& spec, i32 seed)
        : octaves_(make_octaves(spec, seed))
        , nodes_(std::make_unique<Nodes>())
    {
        nodes_->perlin = FastNoise::New<FastNoise::Perlin>(kFeatureSet);
        if (!nodes_->perlin)
        {
            throw std::runtime_error("FastNoise2 has no AVX2 Perlin on this CPU");
        }

        // One lattice cell per unit: the octave's frequency is applied here,
        // in f64, before the split.
        nodes_->perlin->SetScale(1.0f);
    }

    DetailNoise::~DetailNoise() = default;

    void DetailNoise::octave(u32 index, f64vec3 const& origin,
                             span<f32 const> x, span<f32 const> y, span<f32 const> z,
                             span<f32> out)
    {
        Octave const&      octave = octaves_[index];
        LatticeSplit const split  = split_lattice(octave, origin);

        // Offsets are metres and small, so rotating and scaling them in f32
        // loses nothing that matters; the large part went through f64 above.
        f32mat3 const to_lattice{octave.rotation * octave.frequency};

        size_t const count = out.size();
        scratch_x_.resize(count);
        scratch_y_.resize(count);
        scratch_z_.resize(count);
        for (size_t i = 0; i < count; ++i)
        {
            f32vec3 const lattice = to_lattice * f32vec3{x[i], y[i], z[i]};
            scratch_x_[i] = lattice.x;
            scratch_y_[i] = lattice.y;
            scratch_z_[i] = lattice.z;
        }

        nodes_->perlin->SetLatticeOffset(split.base.x, split.base.y, split.base.z);
        nodes_->perlin->GenPositionArray3D(out.data(), static_cast<int>(count),
                                           scratch_x_.data(), scratch_y_.data(), scratch_z_.data(),
                                           split.fraction.x, split.fraction.y, split.fraction.z,
                                           octave.seed);
    }
}
