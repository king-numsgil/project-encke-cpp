#include "core/pch.hpp"

#include "assets/worker.hpp"

#include "core/log.hpp"

#include <exception>
#include <utility>

namespace encke
{
    AssetWorker::~AssetWorker()
    {
        stop();
    }

    void AssetWorker::start()
    {
        thread_ = std::jthread{[this](std::stop_token const& stop) { run(stop); }};
    }

    void AssetWorker::stop()
    {
        if (thread_.joinable())
        {
            thread_.request_stop();
            thread_.join();
        }
    }

    void AssetWorker::submit(Job job)
    {
        {
            std::lock_guard const lock{mutex_};
            queued_.push_back(std::move(job));
            ++outstanding_;
        }
        wake_.notify_one();
    }

    vector<AssetWorker::Finish> AssetWorker::take()
    {
        std::lock_guard const lock{mutex_};

        vector<Finish> taken = std::move(finished_);
        finished_.clear();
        outstanding_ -= taken.size();
        return taken;
    }

    bool AssetWorker::idle() const
    {
        std::lock_guard const lock{mutex_};
        return outstanding_ == 0;
    }

    void AssetWorker::run(std::stop_token const& stop)
    {
        while (true)
        {
            Job job;
            {
                std::unique_lock lock{mutex_};

                // Wakes on a submission or on request_stop(), which the
                // stop_token overload listens for.
                if (!wake_.wait(lock, stop, [this] { return !queued_.empty(); }))
                {
                    return;
                }

                job = std::move(queued_.front());
                queued_.erase(queued_.begin());
            }

            // A corrupt or huge image can throw bad_alloc on the way through
            // a decoder.
            Finish finish;
            try
            {
                finish = job.work();
            }
            catch (std::exception const& error)
            {
                log::error("asset %s: threw: %s", job.name.c_str(), error.what());
                finish = std::move(job.failed);
            }
            catch (...)
            {
                log::error("asset %s: threw", job.name.c_str());
                finish = std::move(job.failed);
            }

            std::lock_guard const lock{mutex_};
            finished_.push_back(std::move(finish));
        }
    }
}
