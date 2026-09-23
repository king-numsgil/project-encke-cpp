#include "core/pch.hpp"

#include "platform/thread.hpp"

#include "core/log.hpp"

// The OS-level thread functions are opt-in, and only this file sees them.
#define BS_THREAD_POOL_NATIVE_EXTENSIONS
#include <BS_thread_pool.hpp>

namespace encke
{
    namespace
    {
        // Linux keeps 16 bytes including the terminator.
        constexpr size_t kMaxNameLength = 15;

        BS::os_thread_priority to_os(ThreadPriority priority)
        {
            switch (priority)
            {
            case ThreadPriority::Background: return BS::os_thread_priority::below_normal;
            case ThreadPriority::Normal:     return BS::os_thread_priority::normal;
            }
            return BS::os_thread_priority::normal;
        }
    }

    bool set_current_thread_priority(ThreadPriority priority)
    {
        bool const set = BS::this_thread::set_os_thread_priority(to_os(priority));
        if (!set)
        {
            log::warn("could not set this thread's priority to %s",
                      priority == ThreadPriority::Background ? "background" : "normal");
        }
        return set;
    }

    bool set_current_thread_name(string_view name)
    {
        string const cut{name.substr(0, kMaxNameLength)};

        bool const set = BS::this_thread::set_os_thread_name(cut);
        if (!set)
        {
            log::warn("could not name this thread \"%s\"", cut.c_str());
        }
        return set;
    }
}
