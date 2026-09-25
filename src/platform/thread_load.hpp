#pragma once

#include <atomic>
#include <chrono>

namespace encke
{
    // How long one worker thread has spent in jobs, for the stats window's
    // load dots. The worker brackets each job with begin() and end(); any
    // thread may read busy_ns(). A job in progress counts up to `now`, so a
    // job longer than the window's averaging still shows as load while it
    // runs.
    //
    // Display only. end() publishes the job's time and clears its start in
    // two steps, so a read between them can count that job twice; it lasts
    // one sample and the reader clamps the fraction.
    class ThreadLoad
    {
    public:
        static u64 now_ns()
        {
            auto const since = std::chrono::steady_clock::now().time_since_epoch();
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(since).count());
        }

        void begin() { started_.store(now_ns(), std::memory_order_relaxed); }

        void end()
        {
            u64 const start = started_.load(std::memory_order_relaxed);
            busy_.fetch_add(now_ns() - start, std::memory_order_relaxed);
            started_.store(0, std::memory_order_relaxed);
        }

        // Busy nanoseconds since the thread started, up to `now`.
        u64 busy_ns(u64 now) const
        {
            u64 const total = busy_.load(std::memory_order_relaxed);
            u64 const start = started_.load(std::memory_order_relaxed);
            return start != 0 && now > start ? total + (now - start) : total;
        }

    private:
        std::atomic<u64> busy_{0};
        std::atomic<u64> started_{0};   // 0 while idle
    };
}
