#pragma once

namespace encke
{
    // A decoded image, tightly packed RGBA8, top row first. A greyscale
    // source comes back with the grey in R, G and B.
    struct Pixels
    {
        u32        width  = 0;
        u32        height = 0;
        vector<u8> rgba;
    };

    // Any format SDL_image was built with: JPG and PNG here. Both log the
    // reason and return false on failure; `what` names the image in the log.
    bool load_pixels(string const& path, Pixels& pixels);
    bool decode_pixels(span<byte const> encoded, char const* what, Pixels& pixels);

    // Where assets/ is: the source tree, baked in by CMake. See CLAUDE.md.
    string asset_path(string_view relative);
}
