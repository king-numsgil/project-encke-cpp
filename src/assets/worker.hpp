#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

namespace encke
{
    // The one asset thread. A job's work runs here -- reading files,
    // decoding images, parsing models -- and returns a Finish, which the
    // owner runs on its own thread when it takes it. So the work never
    // touches shared state, and whatever it produced is applied where
    // everything else is.
    //
    // One thread on purpose: the cores are for the SDF workers. See
    // CLAUDE.md. Nothing here touches Vulkan.
    class AssetWorker
    {
    public:
        // std::function, not move_only_function, which clang64's libc++ does
        // not have; so everything a job captures must be copyable.
        using Finish = function<void()>;

        struct Job
        {
            string            name;     // for logs
            function<Finish()> work;

            // Taken instead of work's Finish when work throws: an
            // exception leaving a jthread's function is std::terminate,
            // and one bad asset should fail itself, not the program.
            Finish failed;
        };

        AssetWorker() = default;
        ~AssetWorker();

        AssetWorker(AssetWorker const&)            = delete;
        AssetWorker& operator=(AssetWorker const&) = delete;
        AssetWorker(AssetWorker&&)                 = delete;
        AssetWorker& operator=(AssetWorker&&)      = delete;

        // Starts the thread; jobs submitted before are kept.
        void start();

        // Asks the thread to stop after the job in hand and joins it. Jobs
        // not yet started, and Finishes not yet taken, are dropped.
        void stop();

        // Queues a job, run in submission order. Any time before stop().
        void submit(Job job);

        // Every Finish produced since the last call, in completion order.
        vector<Finish> take();

        // Every submitted job has run and been taken.
        bool idle() const;

    private:
        void run(std::stop_token const& stop);

        mutable std::mutex          mutex_;
        std::condition_variable_any wake_;
        vector<Job>                 queued_;
        vector<Finish>              finished_;
        size_t                      outstanding_ = 0;   // submitted, not yet taken

        std::jthread thread_;
    };
}
