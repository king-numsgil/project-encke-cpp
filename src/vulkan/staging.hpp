#pragma once

#include "vulkan/buffer.hpp"

namespace encke
{
    class VulkanAllocator;

    // Host-visible memory for uploads recorded into a frame's own command
    // buffer: one fixed arena per frame in flight, bump-allocated and rewound
    // when that slot comes round again. The fence that lets the slot be
    // reused also proves the GPU has finished copying out of its arena, so
    // nothing is freed piecemeal and nothing waits.
    //
    // The arena size is the per-frame upload budget. Whatever does not fit
    // waits for a later frame; that is the point, not a failure.
    class StagingArena
    {
    public:
        struct Allocation
        {
            VkBuffer     buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            void*        data   = nullptr;   // host pointer to write through
        };

        bool init(VulkanAllocator const& allocator, VkDeviceSize bytes_per_slot, u32 slots);
        void shutdown();

        // Rewinds `slot`'s arena and allocates from it until the next call.
        // Only once that slot's fence has signalled.
        void begin(u32 slot);

        // Nullopt when this frame's budget cannot fit `size`. Offsets are
        // aligned to `alignment`, which must be a power of two.
        optional<Allocation> allocate(VkDeviceSize size, VkDeviceSize alignment = 16);

        VkDeviceSize remaining() const { return slot_end_ - head_; }
        VkDeviceSize bytes_per_slot() const { return bytes_per_slot_; }

    private:
        Buffer       buffer_;
        VkDeviceSize bytes_per_slot_ = 0;
        VkDeviceSize head_           = 0;
        VkDeviceSize slot_end_       = 0;
    };
}
