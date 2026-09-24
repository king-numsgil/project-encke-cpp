#pragma once

namespace encke::terrain
{
    // The detail octave count per LOD, then samples per second for 34^3
    // chunks of the example planet at LOD 0 and LOD 4 on one thread: macro
    // and detail layers apart, the apron's share, a mesher's central
    // differences, and the coarse output. Printed to stdout. Returns the
    // process exit code.
    int run_benchmark();
}
