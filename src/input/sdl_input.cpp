#include "core/pch.hpp"

#include "input/sdl_input.hpp"

#include <algorithm>

namespace encke::input
{
    namespace
    {
        constexpr array<HatDirection, 4> kDirections{
            {HatDirection::Up, HatDirection::Right, HatDirection::Down, HatDirection::Left}};

        Control control(ControlKind kind, u32 index, DeviceId const& device = {})
        {
            return Control{.kind = kind, .index = static_cast<u16>(index), .direction = HatDirection::None,
                           .device = device};
        }
    }

    optional<DeviceId> device_id(Joysticks const& joysticks, SDL_JoystickID id)
    {
        span<Joystick const> const devices = joysticks.devices();
        auto const                 found   = std::ranges::find(devices, id, &Joystick::id);
        if (found == devices.end())
        {
            return nullopt;
        }

        // Its place among those opened before it with the same identity.
        auto const same = std::count_if(devices.begin(), found, [&](Joystick const& device) {
            return device.vendor == found->vendor && device.product == found->product;
        });
        return DeviceId{.vendor = found->vendor, .product = found->product, .instance = static_cast<u8>(same)};
    }

    void translate(SDL_Event const& event, Joysticks const& joysticks, bool keys, InputSystem& input)
    {
        u64 const ns = event.common.timestamp;

        switch (event.type)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            // A repeat is not a press; the key is still held.
            if (event.key.repeat || (event.key.down && !keys))
            {
                break;
            }
            input.push(ControlEvent{.control = control(ControlKind::Key, static_cast<u32>(event.key.scancode)),
                                    .value   = event.key.down ? 1.0f : 0.0f,
                                    .ns      = ns});
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            input.push(ControlEvent{.control = control(ControlKind::MouseButton, event.button.button),
                                    .value   = event.button.down ? 1.0f : 0.0f,
                                    .ns      = ns});
            break;

        case SDL_EVENT_MOUSE_MOTION:
            input.push(ControlEvent{.control = control(ControlKind::MouseAxis, 0), .value = event.motion.xrel, .ns = ns});
            input.push(ControlEvent{.control = control(ControlKind::MouseAxis, 1), .value = event.motion.yrel, .ns = ns});
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            input.push(ControlEvent{.control = control(ControlKind::MouseWheel, 0), .value = event.wheel.y, .ns = ns});
            break;

        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
            if (optional<DeviceId> const device = device_id(joysticks, event.jbutton.which))
            {
                input.push(ControlEvent{.control = control(ControlKind::JoyButton, event.jbutton.button, *device),
                                        .value   = event.jbutton.down ? 1.0f : 0.0f,
                                        .ns      = ns});
            }
            break;

        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
            if (optional<DeviceId> const device = device_id(joysticks, event.jaxis.which))
            {
                // -32768 to 32767, as -1 to 1.
                f32 const value = std::max(static_cast<f32>(event.jaxis.value) / 32767.0f, -1.0f);
                input.push(ControlEvent{.control = control(ControlKind::JoyAxis, event.jaxis.axis, *device),
                                        .value   = value,
                                        .ns      = ns});
            }
            break;

        case SDL_EVENT_JOYSTICK_HAT_MOTION:
            if (optional<DeviceId> const device = device_id(joysticks, event.jhat.which))
            {
                // All four, so a move from up to right releases up.
                for (HatDirection const direction : kDirections)
                {
                    Control hat   = control(ControlKind::JoyHat, event.jhat.hat, *device);
                    hat.direction = direction;
                    bool const on = (event.jhat.value & static_cast<u8>(direction)) != 0;
                    input.push(ControlEvent{.control = hat, .value = on ? 1.0f : 0.0f, .ns = ns});
                }
            }
            break;

        case SDL_EVENT_JOYSTICK_REMOVED:
            if (optional<DeviceId> const device = device_id(joysticks, event.jdevice.which))
            {
                input.release_device(*device, ns);
            }
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // SDL drops joystick events in the background, so their releases
            // would never come.
            input.release_all(ns);
            break;

        default:
            break;
        }
    }
}
