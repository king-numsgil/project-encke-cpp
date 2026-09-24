#include "core/pch.hpp"

#include "perlin_reference.hpp"

#include <cmath>

namespace encke::terrain::reference
{
    namespace
    {
        // FastNoise2's Primes and HashMultiplier (Generators/Utils.inl).
        constexpr u32 kPrimeX     = 0xF797C5C7u;
        constexpr u32 kPrimeY     = 0x6C060C89u;
        constexpr u32 kPrimeZ     = 0x465FD04Fu;
        constexpr u32 kMultiplier = 0xB7E0A5F5u;

        // Perlin 3D's kBounding (Generators/Perlin.inl): the output is divided
        // by it to land in [-1, 1].
        constexpr f64 kBounding = 1.0363423824310302734375;

        // The cell, modulo 2^32: all the hash ever sees of it.
        u32 cell_bits(f64 cell)
        {
            return static_cast<u32>(static_cast<u64>(static_cast<i64>(cell)));
        }

        // HashPrimes: xor the primed coordinates into the seed, multiply,
        // then fold the high half down with an arithmetic shift.
        i32 hash(i32 seed, u32 x_primed, u32 y_primed, u32 z_primed)
        {
            u32 const mixed = (bit_cast<u32>(seed) ^ x_primed ^ y_primed ^ z_primed) * kMultiplier;
            i32 const h     = bit_cast<i32>(mixed);
            return (h >> 15) ^ h;
        }

        // GetGradientDotCommon's non-AVX512 path: Perlin's twelve edge
        // gradients, with four repeated, chosen by the low four bits.
        f64 gradient_dot(i32 h, f64 x, f64 y, f64 z)
        {
            i32 const low = h & 13;

            f64 u = (h & 8) != 0 ? y : x;
            f64 v = low == 12 ? x : z;
            v     = low < 2 ? y : v;

            if ((h & 1) != 0)
            {
                u = -u;
            }
            if ((h & 2) != 0)
            {
                v = -v;
            }
            return u + v;
        }

        f64 quintic(f64 t)
        {
            return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
        }

        f64 lerp(f64 a, f64 b, f64 t)
        {
            return a + t * (b - a);
        }
    }

    f64 perlin(i32 seed, f64vec3 const& lattice)
    {
        f64vec3 const cell = glm::floor(lattice);
        f64vec3 const f0   = lattice - cell;
        f64vec3 const f1   = f0 - 1.0;

        u32 const x0 = cell_bits(cell.x) * kPrimeX;
        u32 const y0 = cell_bits(cell.y) * kPrimeY;
        u32 const z0 = cell_bits(cell.z) * kPrimeZ;
        u32 const x1 = x0 + kPrimeX;
        u32 const y1 = y0 + kPrimeY;
        u32 const z1 = z0 + kPrimeZ;

        f64 const sx = quintic(f0.x);
        f64 const sy = quintic(f0.y);
        f64 const sz = quintic(f0.z);

        auto const corner = [&](u32 xp, u32 yp, u32 zp, f64 dx, f64 dy, f64 dz) {
            return gradient_dot(hash(seed, xp, yp, zp), dx, dy, dz);
        };

        f64 const value = lerp(
            lerp(lerp(corner(x0, y0, z0, f0.x, f0.y, f0.z), corner(x1, y0, z0, f1.x, f0.y, f0.z), sx),
                 lerp(corner(x0, y1, z0, f0.x, f1.y, f0.z), corner(x1, y1, z0, f1.x, f1.y, f0.z), sx), sy),
            lerp(lerp(corner(x0, y0, z1, f0.x, f0.y, f1.z), corner(x1, y0, z1, f1.x, f0.y, f1.z), sx),
                 lerp(corner(x0, y1, z1, f0.x, f1.y, f1.z), corner(x1, y1, z1, f1.x, f1.y, f1.z), sx), sy),
            sz);

        return value / kBounding;
    }

    f64 octave(Octave const& octave, f64vec3 const& position)
    {
        return perlin(octave.seed, (octave.rotation * position) * octave.frequency);
    }
}
