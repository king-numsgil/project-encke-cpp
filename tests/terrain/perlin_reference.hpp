#pragma once

#include "terrain/detail_noise.hpp"

// The oracle for the detail layer: FastNoise2's Perlin 3D rewritten as scalar
// f64, with its hash, its gradient set, its quintic fade and its output
// scaling, evaluated straight from the absolute position with no lattice
// split. If the SIMD path agrees with this at 6.4e6 m, the split works.
namespace encke::terrain::reference
{
    // Perlin at a position in lattice cells.
    f64 perlin(i32 seed, f64vec3 const& lattice);

    // One detail octave at a body-relative position in metres.
    f64 octave(Octave const& octave, f64vec3 const& position);
}
