#pragma once

#include <variant>

// When a binding drives its action. A binding is actuated while its value,
// after its modifiers, has a magnitude of at least kActuation; its trigger
// turns that and the edges between into the action's state. Tap, hold,
// double tap, chords and sequences join this variant; each carries its own
// settings, as the modifiers do.
namespace encke::input
{
    inline constexpr f32 kActuation = 0.5f;

    // The value passes through every frame, and each press fires.
    struct Down
    {
        bool operator==(Down const&) const = default;
    };

    // Fires on each press; the value passes only on a frame with one.
    struct Pressed
    {
        bool operator==(Pressed const&) const = default;
    };

    // Fires on each release; the value passes only on a frame with one.
    struct Released
    {
        bool operator==(Released const&) const = default;
    };

    using Trigger = std::variant<Down, Pressed, Released>;
}
