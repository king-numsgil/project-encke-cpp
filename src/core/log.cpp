#include "core/pch.hpp"

#include "core/log.hpp"

#include <chrono>
#include <cstdarg>

namespace encke::log
{
    namespace
    {
        std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

        void emit(char const* level, char const* fmt, std::va_list args)
        {
            std::fprintf(stderr, "[%6lld ms] [%s] ",
                         static_cast<long long>(elapsed_ms()), level);
            std::vfprintf(stderr, fmt, args);
            std::fputc('\n', stderr);
        }
    }

    i64 elapsed_ms()
    {
        auto const now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - g_start).count();
    }

    void init()
    {
        g_start = std::chrono::steady_clock::now();

        // Everything goes to stderr so the ordering between levels is real
        // rather than an artefact of two independently buffered streams.
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }

    void info(char const* fmt, ...)
    {
        std::va_list args;
        va_start(args, fmt);
        emit("info", fmt, args);
        va_end(args);
    }

    void warn(char const* fmt, ...)
    {
        std::va_list args;
        va_start(args, fmt);
        emit("warn", fmt, args);
        va_end(args);
    }

    void error(char const* fmt, ...)
    {
        std::va_list args;
        va_start(args, fmt);
        emit("error", fmt, args);
        va_end(args);
    }

    void vk_error(char const* what, VkResult result)
    {
        error("%s failed: VkResult %d", what, static_cast<int>(result));
    }

    void sdl_error(char const* what)
    {
        error("%s failed: %s", what, SDL_GetError());
    }
}
