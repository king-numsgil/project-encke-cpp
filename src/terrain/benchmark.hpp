#pragma once

namespace encke::terrain
{
    // The detail octave count per LOD, then samples per second for 36^3
    // chunks of the example planet at LOD 0 and LOD 4 on one thread: macro
    // and detail layers apart, the apron's share, a mesher's central
    // differences, and the coarse output. Printed to stdout. Returns the
    // process exit code.
    int run_benchmark();

    // `encke --sweep [chunks] [--threads n]`: every LOD from 0 to 20,
    // `chunks` chunks each (100000 by default), every chunk with a new
    // seed at a random place on the surface. Per LOD: mean, p50 and p99
    // chunk time, the macro, detail and coarse split, and what FastNoise2
    // alone costs for the same work, so the remainder is ours, and a hash of
    // every output bit, which must not change when the sampler is only made
    // faster. Each LOD runs on one thread, which does not change the hash;
    // threads 0 means one per physical core less one.
    int run_sweep(u32 chunks, u32 threads);
}
