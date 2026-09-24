#pragma once

// FastNoise2, for the terrain sources only; no header outside src/terrain
// includes this, so nothing else compiles against it.

#include <FastNoise/FastNoise.h>

namespace encke::terrain
{
    // Every node is created at AVX2 and no higher. The vcpkg port compiles
    // that feature set alone, with strict floating point, so every machine
    // runs the same instructions and gets the same bits. Encke requires AVX2
    // anyway (-mavx2 -mfma).
    inline constexpr FastSIMD::FeatureSet kFeatureSet = FastSIMD::FeatureSet::AVX2;
}
