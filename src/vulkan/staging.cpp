#include "core/pch.hpp"

#include "vulkan/staging.hpp"

namespace encke
{
    bool StagingArena::init(VulkanAllocator const& allocator, VkDeviceSize bytes_per_slot, u32 slots)
    {
        bytes_per_slot_ = bytes_per_slot;
        head_           = 0;
        slot_end_       = 0;
        return buffer_.init_mapped(allocator, bytes_per_slot * slots,
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }

    void StagingArena::shutdown()
    {
        buffer_.shutdown();
        bytes_per_slot_ = 0;
        head_           = 0;
        slot_end_       = 0;
    }

    void StagingArena::begin(u32 slot)
    {
        head_     = bytes_per_slot_ * slot;
        slot_end_ = head_ + bytes_per_slot_;
    }

    optional<StagingArena::Allocation> StagingArena::allocate(VkDeviceSize size,
                                                               VkDeviceSize alignment)
    {
        VkDeviceSize const offset = (head_ + alignment - 1) & ~(alignment - 1);
        if (offset + size > slot_end_)
        {
            return nullopt;
        }

        head_ = offset + size;
        return Allocation{
            .buffer = buffer_.handle(),
            .offset = offset,
            .data   = static_cast<u8*>(buffer_.mapped()) + offset,
        };
    }
}
