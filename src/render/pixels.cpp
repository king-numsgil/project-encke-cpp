#include "core/pch.hpp"

#include "render/pixels.hpp"

#include "core/log.hpp"

#include <SDL3_image/SDL_image.h>

#include <cstring>

namespace encke
{
    namespace
    {
        // Takes ownership of `loaded`.
        bool to_rgba(SDL_Surface* loaded, char const* what, Pixels& pixels)
        {
            if (loaded == nullptr)
            {
                log::error("%s: %s", what, SDL_GetError());
                return false;
            }

            SDL_Surface* const rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(loaded);
            if (rgba == nullptr)
            {
                log::error("%s: conversion to RGBA failed: %s", what, SDL_GetError());
                return false;
            }

            pixels.width  = static_cast<u32>(rgba->w);
            pixels.height = static_cast<u32>(rgba->h);

            // The surface's rows may be padded; the upload wants them packed.
            size_t const row = size_t{pixels.width} * 4;
            pixels.rgba.resize(row * pixels.height);
            auto const* const source = static_cast<u8 const*>(rgba->pixels);
            for (u32 y = 0; y < pixels.height; ++y)
            {
                std::memcpy(pixels.rgba.data() + row * y,
                            source + static_cast<size_t>(rgba->pitch) * y, row);
            }

            SDL_DestroySurface(rgba);
            return true;
        }
    }

    bool load_pixels(string const& path, Pixels& pixels)
    {
        return to_rgba(IMG_Load(path.c_str()), path.c_str(), pixels);
    }

    bool decode_pixels(span<byte const> encoded, char const* what, Pixels& pixels)
    {
        SDL_IOStream* const stream = SDL_IOFromConstMem(encoded.data(), encoded.size());
        if (stream == nullptr)
        {
            log::error("%s: %s", what, SDL_GetError());
            return false;
        }

        // true: IMG_Load_IO closes the stream, success or not.
        return to_rgba(IMG_Load_IO(stream, true), what, pixels);
    }

    string asset_path(string_view relative)
    {
        return string{ENCKE_ASSET_DIR} + "/" + string{relative};
    }
}
