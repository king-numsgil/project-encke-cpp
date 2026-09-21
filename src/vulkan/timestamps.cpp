#include "core/pch.hpp"

#include "vulkan/timestamps.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "vulkan/device.hpp"

namespace encke
{
    GpuTimestamps::~GpuTimestamps()
    {
        shutdown();
    }

    bool GpuTimestamps::init(VulkanDevice const& device, u32 frames_in_flight)
    {
        u32 const valid_bits = device.timestamp_valid_bits();
        if (valid_bits == 0)
        {
            log::warn("graphics queue cannot write timestamps -- GPU timings disabled");
            return true;
        }

        device_     = device.handle();
        period_ns_  = static_cast<f64>(device.properties().limits.timestampPeriod);
        valid_mask_ = valid_bits >= 64 ? ~0ULL : (1ULL << valid_bits) - 1ULL;

        VkQueryPoolCreateInfo const info{
            .sType              = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext              = nullptr,
            .flags              = 0,
            .queryType          = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount         = kQueriesPerSlot * frames_in_flight,
            .pipelineStatistics = 0,
        };

        VkResult const result =
            vkCreateQueryPool(device_, &info, memory::vulkan_callbacks(), &pool_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateQueryPool", result);
            return false;
        }

        slots_.assign(frames_in_flight, Slot{});

        log::info("gpu timestamps: %u valid bits, %.3f ns/tick", valid_bits, period_ns_);
        return true;
    }

    void GpuTimestamps::collect(u32 slot)
    {
        if (!supported() || !slots_[slot].recorded)
        {
            return;
        }

        Slot const& source = slots_[slot];
        u32 const   count  = source.marks + 1;

        array<u64, kQueriesPerSlot> ticks{};

        // No WAIT flag: the caller has already waited on this slot's fence, so
        // every query is available. NOT_READY here would mean that promise was
        // broken, and the right response is to skip the frame, not to stall.
        VkResult const result =
            vkGetQueryPoolResults(device_, pool_, slot * kQueriesPerSlot, count,
                                  sizeof(ticks), ticks.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
        if (result != VK_SUCCESS)
        {
            return;
        }

        for (u32 index = 0; index < source.marks; ++index)
        {
            // Masking the difference keeps a counter that wrapped between two
            // stamps correct, provided fewer than 64 bits are valid.
            u64 const delta = (ticks[index + 1] - ticks[index]) & valid_mask_;

            latest_[index] = GpuSection{
                .label = source.labels[index],
                .ms    = static_cast<f64>(delta) * period_ns_ * 1e-6,
            };
        }
        latest_count_ = source.marks;
    }

    void GpuTimestamps::begin(VkCommandBuffer command, u32 slot)
    {
        if (!supported())
        {
            return;
        }

        recording_ = slot;

        Slot& target    = slots_[slot];
        target.marks    = 0;
        target.recorded = true;

        u32 const base = slot * kQueriesPerSlot;
        vkCmdResetQueryPool(command, pool_, base, kQueriesPerSlot);
        vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, base);
    }

    void GpuTimestamps::mark(VkCommandBuffer command, char const* label)
    {
        if (!supported())
        {
            return;
        }

        Slot& target = slots_[recording_];
        if (target.marks >= kMaxSections)
        {
            return;
        }

        target.labels[target.marks] = label;
        ++target.marks;

        vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_,
                             recording_ * kQueriesPerSlot + target.marks);
    }

    void GpuTimestamps::shutdown()
    {
        if (pool_ != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(device_, pool_, memory::vulkan_callbacks());
            pool_ = VK_NULL_HANDLE;
        }

        device_       = VK_NULL_HANDLE;
        slots_.clear();
        latest_count_ = 0;
    }
}
