#pragma once

namespace encke
{
    // The processor's core counts, from pytorch/cpuinfo, which does the
    // per-OS topology reading. Hyperthreads on one physical core share its
    // vector units and caches, so heavy SIMD workers are sized by physical
    // cores, not logical ones.
    //
    // This is the hardware, not what the process may use: cpuinfo ignores
    // CPU affinity and container limits.
    struct CpuInfo
    {
        u32 physical_cores     = 1;
        u32 logical_processors = 1;
    };

    // Falls back to std::thread::hardware_concurrency() for both counts,
    // logged, when cpuinfo cannot read the topology.
    CpuInfo query_cpu();
}
