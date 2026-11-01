#pragma once

#include <filesystem>

namespace encke
{
    class Joysticks;

    // Every connected joystick, live: axes as bars with their raw value and
    // the range seen, buttons lit while held with their press count on hover,
    // hats by direction. Starts and stops the input log, which goes to a new
    // timestamped file in `log_directory`. `open` is cleared when the user
    // closes it.
    void input_window(Joysticks& joysticks, bool& open, std::filesystem::path const& log_directory);
}
