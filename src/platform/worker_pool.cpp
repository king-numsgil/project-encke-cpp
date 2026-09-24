#include "core/pch.hpp"

#include "platform/worker_pool.hpp"

#include "core/log.hpp"
#include "platform/thread.hpp"

#include <exception>
#include <utility>

namespace encke
{
    WorkerPool::~WorkerPool()
    {
        stop();
    }

    void WorkerPool::start(u32 count, string_view name)
    {
        threads_.reserve(count);
        for (u32 index = 0; index < count; ++index)
        {
            string thread_name = string{name} + std::to_string(index);
            threads_.emplace_back([this, index, thread_name = std::move(thread_name)](std::stop_token const& stop) {
                run(stop, index, thread_name);
            });
        }
    }

    void WorkerPool::stop()
    {
        {
            std::lock_guard const lock{mutex_};
            queued_.clear();
        }

        // Each request wakes its thread through the stop_token overload of
        // wait; a thread mid-job finishes that job first.
        for (std::jthread& thread : threads_)
        {
            thread.request_stop();
        }
        threads_.clear();

        std::lock_guard const lock{mutex_};
        finished_.clear();
        outstanding_ = 0;
    }

    void WorkerPool::submit(Job job)
    {
        {
            std::lock_guard const lock{mutex_};
            queued_.push_back(std::move(job));
            ++outstanding_;
        }
        wake_.notify_one();
    }

    u32 WorkerPool::drain()
    {
        vector<Completion> finished;
        {
            std::lock_guard const lock{mutex_};
            finished.swap(finished_);
            outstanding_ -= static_cast<u32>(finished.size());
        }

        for (Completion const& completion : finished)
        {
            if (completion)
            {
                completion();
            }
        }
        return static_cast<u32>(finished.size());
    }

    u32 WorkerPool::outstanding() const
    {
        std::lock_guard const lock{mutex_};
        return outstanding_;
    }

    void WorkerPool::run(std::stop_token const& stop, u32 index, string const& name)
    {
        // Below the window and Vulkan thread, so a busy pool never makes a
        // frame late.
        set_current_thread_name(name);
        set_current_thread_priority(ThreadPriority::Background);

        while (true)
        {
            Job job;
            {
                std::unique_lock lock{mutex_};
                if (!wake_.wait(lock, stop, [this] { return !queued_.empty(); }))
                {
                    return;
                }
                job = std::move(queued_.front());
                queued_.pop_front();
            }

            // A job that throws still counts as done, with nothing to finish:
            // an exception leaving a jthread's function is std::terminate.
            Completion completion;
            try
            {
                completion = job(index);
            }
            catch (std::exception const& error)
            {
                log::error("worker %u: job threw: %s", index, error.what());
            }
            catch (...)
            {
                log::error("worker %u: job threw", index);
            }

            std::lock_guard const lock{mutex_};
            finished_.push_back(std::move(completion));
        }
    }
}
