#pragma once

namespace encke::terrain
{
    // Samples per second for 34^3 chunks of the example planet at LOD 0 and
    // LOD 4 on one thread, macro and detail layers reported apart. Printed
    // to stdout. Returns the process exit code.
    int run_benchmark();
}
