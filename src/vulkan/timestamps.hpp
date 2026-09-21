#pragma once

namespace encke
{
    class VulkanDevice;

    // One timed stretch of a frame's GPU work.
    struct GpuSection
    {
        // Static storage; the pointer is kept, not the characters.
        char const* label = "";
        f64         ms    = 0.0;
    };

    // GPU timestamp queries, one range per frame in flight.
    //
    // A frame is timed as a chain: begin() stamps the start, and each mark()
    // closes the section since the previous stamp under its label. Every stamp
    // is written at ALL_COMMANDS, so it lands once everything recorded before
    // it -- including earlier submissions on the queue -- has finished. The
    // sections therefore tile the frame's GPU time with no overlap, at the
    // price of hiding any overlap the hardware would otherwise find.
    //
    // Results for a slot are read back after that slot's fence has signalled,
    // i.e. one full frames-in-flight cycle late.
    class GpuTimestamps
    {
    public:
        static constexpr u32 kMaxSections = 15;

        GpuTimestamps() = default;
        ~GpuTimestamps();

        GpuTimestamps(GpuTimestamps const&)            = delete;
        GpuTimestamps& operator=(GpuTimestamps const&) = delete;
        GpuTimestamps(GpuTimestamps&&)                 = delete;
        GpuTimestamps& operator=(GpuTimestamps&&)      = delete;

        // Succeeds without timing anything when the graphics queue cannot
        // write timestamps; every call below then does nothing.
        bool init(VulkanDevice const& device, u32 frames_in_flight);
        void shutdown();

        bool supported() const { return pool_ != VK_NULL_HANDLE; }

        // Reads back the results `slot` produced last time it was recorded.
        // Call only once that slot's fence has signalled.
        void collect(u32 slot);

        // Resets the slot's queries and writes the starting stamp. Must be
        // recorded outside any rendering scope.
        void begin(VkCommandBuffer command, u32 slot);

        // Closes the section since the previous stamp. Silently ignored past
        // kMaxSections.
        void mark(VkCommandBuffer command, char const* label);

        // The most recent collected frame. Empty until a slot has come round.
        span<GpuSection const> sections() const { return {latest_.data(), latest_count_}; }

    private:
        static constexpr u32 kQueriesPerSlot = kMaxSections + 1;

        struct Slot
        {
            array<char const*, kMaxSections> labels{};
            u32                              marks    = 0;
            bool                             recorded = false;
        };

        VkDevice    device_ = VK_NULL_HANDLE;
        VkQueryPool pool_   = VK_NULL_HANDLE;

        // Nanoseconds per tick, and the mask of meaningful bits in a stamp.
        f64 period_ns_  = 0.0;
        u64 valid_mask_ = 0;

        vector<Slot> slots_;
        u32          recording_ = 0;

        array<GpuSection, kMaxSections> latest_{};
        size_t                          latest_count_ = 0;
    };
}
