#include "core/pch.hpp"

#include "platform/cpu.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <thread>

#include <cpuinfo.h>

namespace encke
{
    CpuInfo query_cpu()
    {
        // cpuinfo_initialize is idempotent and thread-safe; its tables live
        // until process exit, so there is no matching deinitialize.
        if (cpuinfo_initialize())
        {
            return CpuInfo{
                .physical_cores     = std::max(cpuinfo_get_cores_count(), 1u),
                .logical_processors = std::max(cpuinfo_get_processors_count(), 1u),
            };
        }

        u32 const logical = std::max(std::thread::hardware_concurrency(), 1u);
        log::warn("cpuinfo could not read the CPU topology; assuming %u cores, no SMT", logical);
        return CpuInfo{.physical_cores = logical, .logical_processors = logical};
    }
}
