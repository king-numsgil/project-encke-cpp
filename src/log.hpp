#pragma once

#if defined(__GNUC__) || defined(__clang__)
#   define ENCKE_PRINTF_FORMAT(fmt_index, first_arg) \
        __attribute__((format(printf, fmt_index, first_arg)))
#else
#   define ENCKE_PRINTF_FORMAT(fmt_index, first_arg)
#endif

namespace encke::log
{
    // Makes diagnostics unbuffered. Without this a crash mid-frame eats the
    // last few lines, which are usually the interesting ones.
    void init();

    void info(char const* fmt, ...)  ENCKE_PRINTF_FORMAT(1, 2);
    void warn(char const* fmt, ...)  ENCKE_PRINTF_FORMAT(1, 2);
    void error(char const* fmt, ...) ENCKE_PRINTF_FORMAT(1, 2);

    // Vulkan calls that return a status rather than a pointer.
    void vk_error(char const* what, VkResult result);

    // SDL failures carry their detail in SDL_GetError().
    void sdl_error(char const* what);
}
