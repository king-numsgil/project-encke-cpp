#include "core/pch.hpp"

#include "core/log.hpp"

#include <chrono>
#include <cstdarg>

namespace encke::log
{
    namespace
    {
        std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

        // Formatted whole, then written in one call: stderr is unbuffered,
        // so three separate writes from two threads could interleave
        // mid-line. The material loader logs from its worker.
        void emit(char const* level, char const* fmt, std::va_list args)
        {
            char line[1024];
            int  length = std::snprintf(line, sizeof(line), "[%6lld ms] [%s] ",
                                        static_cast<long long>(elapsed_ms()), level);
            if (length < 0)
            {
                return;
            }

            size_t const used = std::min(static_cast<size_t>(length), sizeof(line) - 1);
            int const    body = std::vsnprintf(line + used, sizeof(line) - used, fmt, args);
            size_t const end =
                body < 0 ? used : std::min(used + static_cast<size_t>(body), sizeof(line) - 2);

            line[end]     = '\n';
            line[end + 1] = '\0';
            std::fputs(line, stderr);
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
