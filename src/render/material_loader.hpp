#pragma once

#include "render/material.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace encke
{
    // Decodes materials on a worker thread, so nothing waits on JPG or PNG
    // decoding. Anything that can produce MaterialMaps submits a job -- an
    // ambientCG set, a glTF material -- and the renderer collects what is
    // finished each frame and uploads it within the frame's staging budget.
    //
    // Nothing here touches Vulkan: a job only reads files or bytes and packs
    // texels, and must be safe to run off the main thread.
    class MaterialLoader
    {
    public:
        struct Job
        {
            u32                               material = 0;   // renderer material id
            string                            name;           // for logs
            function<bool(MaterialMaps& maps)> decode;        // false: it logged why
        };

        struct Decoded
        {
            u32          material = 0;
            string       name;
            bool         ok = false;
            MaterialMaps maps;
        };

        MaterialLoader() = default;
        ~MaterialLoader();

        MaterialLoader(MaterialLoader const&)            = delete;
        MaterialLoader& operator=(MaterialLoader const&) = delete;
        MaterialLoader(MaterialLoader&&)                 = delete;
        MaterialLoader& operator=(MaterialLoader&&)      = delete;

        // Starts the worker; jobs submitted before are kept.
        void start();

        // Asks the worker to stop after the job in hand and joins it. Jobs
        // not yet started are dropped.
        void stop();

        // Queues a job, decoded in submission order. Any time before stop().
        void submit(Job job);

        // Everything finished since the last call, in completion order.
        vector<Decoded> take();

        // Every submitted job has been decoded and taken.
        bool idle() const;

    private:
        void run(std::stop_token const& stop);

        mutable std::mutex          mutex_;
        std::condition_variable_any wake_;
        vector<Job>                 queued_;
        vector<Decoded>             finished_;
        size_t                      outstanding_ = 0;   // submitted, not yet taken

        std::jthread thread_;
    };
}
