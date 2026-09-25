#pragma once

#include "platform/thread_load.hpp"
#include "vulkan/timestamps.hpp"

namespace encke
{
    // Frame timing history and the window that shows it.
    //
    // Frames are folded into buckets of at least 1/60 s before they are
    // stored, so the history covers the same span of wall time whether the
    // loop runs at 60 fps or 3000. Each bucket keeps the mean of everything
    // and the worst frame time, so a single hitch still shows.
    class StatsWindow
    {
    public:
        // A row of load dots: one per thread, green idle to red busy.
        struct Threads
        {
            char const*            name = "";
            span<ThreadLoad const> loads;
            size_t                 queued = 0;
        };

        struct Info
        {
            char const* device       = "";
            char const* present_mode = "";
            VkExtent2D  extent{};

            // The frame limiter's toggle, owned by the caller; no checkbox
            // when null.
            bool* limit_frames = nullptr;
            f64   limit_hz     = 0.0;

            // The same groups every frame, or their smoothing restarts.
            span<Threads const> threads;
        };

        // `now` in seconds on any monotonic clock. `blocked_ms` is the part of
        // the frame the CPU spent waiting on the GPU or the presentation
        // engine. `gpu` may be empty while timestamps are not yet available.
        void record(f64 now, f64 frame_ms, f64 blocked_ms, span<GpuSection const> gpu);

        void draw(Info const& info);

    private:
        static constexpr size_t kCapacity      = 1024;
        static constexpr size_t kMaxSections   = GpuTimestamps::kMaxSections;
        static constexpr f64    kBucketSeconds = 1.0 / 60.0;
        static constexpr f64    kPlotSeconds   = 10.0;
        static constexpr f64    kTableSeconds  = 1.0;

        // Time constant of the load dots' exponential smoothing.
        static constexpr f64 kLoadSeconds = 0.25;

        // Per thread across every group, in order: the busy time and clock
        // at the last draw, and the smoothed busy fraction.
        struct LoadSample
        {
            u64 busy_ns  = 0;
            u64 at_ns    = 0;
            f64 fraction = 0.0;
        };

        void draw_threads(span<Threads const> groups);

        struct Accumulator
        {
            f64                        start     = 0.0;
            u32                        frames    = 0;
            f64                        frame_sum = 0.0;
            f64                        frame_max = 0.0;
            f64                        busy_sum  = 0.0;
            u32                        gpu_frames = 0;
            array<f64, kMaxSections>   gpu_sum{};
        };

        void push_bucket(f64 now);
        void clear();

        // Copies the ring into the chronological scratch arrays below and
        // returns how many samples there are.
        size_t unwrap();

        Accumulator pending_;
        bool        started_ = false;

        // Section labels the history was recorded with. If the renderer
        // reports a different set, the history is dropped: plotting old
        // numbers under new names would be wrong.
        array<char const*, kMaxSections> labels_{};
        size_t                           section_count_ = 0;

        // Ring buffers, one entry per bucket.
        array<f64, kCapacity>                           time_{};
        array<f64, kCapacity>                           frame_avg_{};
        array<f64, kCapacity>                           frame_max_{};
        array<f64, kCapacity>                           busy_avg_{};
        array<f64, kCapacity>                           gpu_total_{};
        array<array<f64, kCapacity>, kMaxSections>      gpu_{};
        size_t                                          head_  = 0;
        size_t                                          count_ = 0;

        // Chronological copies for plotting; the GPU rows hold running sums
        // so each section can be drawn as a band stacked on the one below.
        vector<f64> plot_time_;
        vector<f64> plot_frame_avg_;
        vector<f64> plot_frame_max_;
        vector<f64> plot_busy_;
        vector<f64> plot_gpu_total_;
        vector<f64> plot_stack_;

        vector<LoadSample> loads_;
    };
}
