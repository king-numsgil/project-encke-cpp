#pragma once

#include "input/control.hpp"

#include <map>

// Press-to-bind: which control the user means, from the events that follow
// starting it. A key, a joystick button or a hat direction is taken on its
// press. An axis is taken once it has moved kAxisMargin from where it
// first reported, since a resting throttle is noisy and an axis only
// reports when it moves. The mouse is never taken: clicking the editor
// would bind the click. Escape cancels.
namespace encke::input
{
    class Capture
    {
    public:
        // A quarter of an axis's travel.
        static constexpr f32 kAxisMargin = 0.5f;

        void start();
        void cancel();
        bool active() const { return active_; }

        // This frame's events; the control once one is taken, which ends
        // the capture.
        optional<Control> feed(span<ControlEvent const> events);

    private:
        bool                   active_ = false;
        std::map<Control, f32> first_;
    };
}
