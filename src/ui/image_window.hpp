#pragma once

namespace encke
{
    // A UI window showing one bindless sampled image, fitted to the window
    // with its aspect ratio kept. `open` is cleared when the user closes it.
    // `first_position` places it the first time it ever appears; after that
    // imgui.ini remembers. `controls`, when given, draws above the image.
    //
    // The image must be in READ_ONLY_OPTIMAL, with its writes made visible to
    // the fragment stage, by the time the overlay pass runs.
    void image_window(char const* title, bool& open, u32 bindless_handle, VkExtent2D extent,
                      f32vec2 first_position, function<void()> const& controls = {});
}
