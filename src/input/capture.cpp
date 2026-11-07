#include "core/pch.hpp"

#include "input/capture.hpp"

#include <cmath>

namespace encke::input
{
    void Capture::start()
    {
        active_ = true;
        first_.clear();
    }

    void Capture::cancel()
    {
        active_ = false;
        first_.clear();
    }

    optional<Control> Capture::feed(span<ControlEvent const> events)
    {
        if (!active_)
        {
            return nullopt;
        }

        for (ControlEvent const& event : events)
        {
            Control const& control = event.control;
            switch (control.kind)
            {
            case ControlKind::Key:
                if (event.value > 0.0f)
                {
                    cancel();
                    if (control.index == SDL_SCANCODE_ESCAPE)
                    {
                        return nullopt;
                    }
                    return control;
                }
                break;

            case ControlKind::JoyButton:
            case ControlKind::JoyHat:
                if (event.value > 0.0f)
                {
                    cancel();
                    return control;
                }
                break;

            case ControlKind::JoyAxis:
            {
                auto const [first, fresh] = first_.try_emplace(control, event.value);
                if (!fresh && std::abs(event.value - first->second) >= kAxisMargin)
                {
                    cancel();
                    return control;
                }
                break;
            }

            case ControlKind::MouseButton:
            case ControlKind::MouseAxis:
            case ControlKind::MouseWheel:
                break;
            }
        }
        return nullopt;
    }
}
