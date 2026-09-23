#include "core/pch.hpp"

#include "render/material_loader.hpp"

#include <utility>

namespace encke
{
    MaterialLoader::~MaterialLoader()
    {
        stop();
    }

    void MaterialLoader::start()
    {
        thread_ = std::jthread{[this](std::stop_token const& stop) { run(stop); }};
    }

    void MaterialLoader::stop()
    {
        if (thread_.joinable())
        {
            thread_.request_stop();
            thread_.join();
        }
    }

    void MaterialLoader::submit(Job job)
    {
        {
            std::lock_guard const lock{mutex_};
            queued_.push_back(std::move(job));
            ++outstanding_;
        }
        wake_.notify_one();
    }

    vector<MaterialLoader::Decoded> MaterialLoader::take()
    {
        std::lock_guard const lock{mutex_};

        vector<Decoded> taken = std::move(finished_);
        finished_.clear();
        outstanding_ -= taken.size();
        return taken;
    }

    bool MaterialLoader::idle() const
    {
        std::lock_guard const lock{mutex_};
        return outstanding_ == 0;
    }

    void MaterialLoader::run(std::stop_token const& stop)
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

            Decoded decoded;
            decoded.material = job.material;
            decoded.name     = std::move(job.name);
            decoded.ok       = job.decode(decoded.maps);

            std::lock_guard const lock{mutex_};
            finished_.push_back(std::move(decoded));
        }
    }
}
