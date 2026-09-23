#include "core/pch.hpp"

#include "render/material_loader.hpp"

#include "core/log.hpp"

#include <exception>
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

            // An exception leaving a jthread's function is std::terminate,
            // and a corrupt or huge image can throw bad_alloc on the way
            // through a decoder. One bad asset fails its material, not the
            // program.
            try
            {
                decoded.ok = job.decode(decoded.maps);
            }
            catch (std::exception const& error)
            {
                log::error("material %s: decode threw: %s", decoded.name.c_str(), error.what());
                decoded.ok   = false;
                decoded.maps = MaterialMaps{};
            }
            catch (...)
            {
                log::error("material %s: decode threw", decoded.name.c_str());
                decoded.ok   = false;
                decoded.maps = MaterialMaps{};
            }

            std::lock_guard const lock{mutex_};
            finished_.push_back(std::move(decoded));
        }
    }
}
