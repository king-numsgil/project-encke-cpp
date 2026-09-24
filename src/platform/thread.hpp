#pragma once

namespace encke
{
    // OS thread controls, through BS::thread_pool's native extensions, which
    // do the per-OS work (Win32 on Windows, pthreads and nice values on
    // Linux). Only platform/thread.cpp includes it, since on Windows it pulls
    // in <windows.h>.
    //
    // WorkerPool lowers its threads with this.

    enum class ThreadPriority
    {
        // Below normal: heavy workers yield to the window and Vulkan thread
        // whenever both want a logical processor.
        Background,
        Normal,
    };

    // Both act on the calling thread, so they work on any std::jthread, from
    // inside its function. Both may fail -- on Linux an unprivileged thread
    // may lower its priority but never raise it again -- and a failure is
    // logged and returned, not fatal.
    bool set_current_thread_priority(ThreadPriority priority);

    // Shown by debuggers, profilers and top. Cut to 15 characters, Linux's
    // limit, on every platform so names look the same everywhere.
    bool set_current_thread_name(string_view name);
}
