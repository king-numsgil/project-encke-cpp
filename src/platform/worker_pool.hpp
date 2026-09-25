#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "platform/thread_load.hpp"

namespace encke
{
    // A pool of std::jthreads at background priority, for heavy CPU work:
    // terrain meshing now, streaming and LOD later. A job runs on a worker and
    // returns a completion, which drain() runs on the main thread; so a job
    // touches only what it owns or shares, and anything main-thread-only
    // (the registry, the asset manager) is reached from its completion.
    //
    // Jobs are std::function, so everything they capture must be copyable,
    // as with AssetWorker; bulky results go through a shared_ptr.
    //
    // A free worker takes the most urgent queued job, not the oldest: the one
    // whose priority is lowest when it looks, oldest first among equals. A
    // priority is shared with the submitter, who may change it while the job
    // waits -- a chunk's distance to a moving camera -- so the order is
    // decided at the last moment. The queue is scanned whole each time, which
    // is nothing next to a job's own work at the sizes it holds.
    class WorkerPool
    {
    public:
        // Run on the main thread by drain(). Empty for nothing to do.
        using Completion = function<void()>;

        // Given the index of the worker running it, from 0 to size() - 1, so
        // a job can reach per-worker state without locking.
        using Job = function<Completion(u32 worker)>;

        // Lower is more urgent. Null counts as 0.
        using Priority = std::shared_ptr<std::atomic<f64> const>;

        WorkerPool() = default;
        ~WorkerPool();

        WorkerPool(WorkerPool const&)            = delete;
        WorkerPool& operator=(WorkerPool const&) = delete;
        WorkerPool(WorkerPool&&)                 = delete;
        WorkerPool& operator=(WorkerPool&&)      = delete;

        // Starts `count` workers, each named `name` plus its index.
        void start(u32 count, string_view name);

        // Discards queued jobs, lets running ones finish, joins every worker
        // and drops undrained completions. Idempotent.
        void stop();

        u32 size() const { return static_cast<u32>(threads_.size()); }

        void submit(Job job, Priority priority = nullptr);

        // Runs every completion that has arrived, on the calling thread, in
        // the order the jobs finished. Returns how many.
        u32 drain();

        // Submitted and not yet drained.
        u32 outstanding() const;

        // One per worker, for the stats window.
        span<ThreadLoad const> loads() const { return {loads_.get(), threads_.size()}; }

    private:
        struct Queued
        {
            Job      job;
            Priority priority;
        };

        void run(std::stop_token const& stop, u32 index, string const& name);

        mutable std::mutex          mutex_;
        std::condition_variable_any wake_;
        std::deque<Queued>          queued_;
        vector<Completion>          finished_;
        u32                         outstanding_ = 0;

        // Before the threads, so it outlives them. An array, since atomics
        // cannot move and a vector of them cannot be sized after the fact.
        std::unique_ptr<ThreadLoad[]> loads_;

        vector<std::jthread> threads_;
    };
}
