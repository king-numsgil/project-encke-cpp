#pragma once

#include <compare>

// A control is one thing on one device that a binding can read: a key, a
// mouse axis, a joystick's button, axis or hat direction. Bindings name
// them as text ("key:W", "joy:044f:0422:button:5"), so a file written on
// one machine reads on another with the same devices.
namespace encke::input
{
    enum class ControlKind : u8
    {
        Key,           // index: SDL scancode
        MouseButton,   // index: SDL_BUTTON_*
        MouseAxis,     // index: 0 x, 1 y; relative, pixels
        MouseWheel,    // relative, notches, positive away from the user
        JoyButton,     // index: button
        JoyAxis,       // index: axis; -1 to 1
        JoyHat,        // index: hat; direction: one of the four
    };

    // A hat reads as four buttons; a diagonal holds two.
    enum class HatDirection : u8
    {
        None  = 0,
        Up    = 1,   // SDL_HAT_UP's bit, and so on
        Right = 2,
        Down  = 4,
        Left  = 8,
    };

    // A joystick by vendor:product, which survives a change of USB port where
    // SDL's path and instance id do not. `instance` tells identical devices
    // apart, in the order they were opened.
    struct DeviceId
    {
        u16 vendor   = 0;
        u16 product  = 0;
        u8  instance = 0;

        auto operator<=>(DeviceId const&) const = default;
    };

    struct Control
    {
        ControlKind  kind      = ControlKind::Key;
        u16          index     = 0;
        HatDirection direction = HatDirection::None;
        DeviceId     device;   // joysticks only

        auto operator<=>(Control const&) const = default;

        // Mouse motion and the wheel report deltas, summed per frame; every
        // other control holds a value until it changes.
        bool relative() const { return kind == ControlKind::MouseAxis || kind == ControlKind::MouseWheel; }

        bool joystick() const
        {
            return kind == ControlKind::JoyButton || kind == ControlKind::JoyAxis || kind == ControlKind::JoyHat;
        }
    };

    // The text form:
    //   key:<SDL scancode name>              key:W, key:Left Shift, key:F1
    //   mouse:x, mouse:y, mouse:wheel
    //   mouse:left|middle|right|x1|x2
    //   joy:<vvvv>:<pppp>[#n]:button:<i>     hex vendor and product
    //   joy:<vvvv>:<pppp>[#n]:axis:<i>
    //   joy:<vvvv>:<pppp>[#n]:hat:<i>:up|right|down|left
    optional<Control> parse_control(string_view text);
    string            format_control(Control const& control);

    // A change to one control, in the order the pump saw it. `value` is 0 or
    // 1 for buttons, keys and hat directions, -1 to 1 for a joystick axis,
    // and a delta for mouse motion and the wheel.
    struct ControlEvent
    {
        Control control;
        f32     value = 0.0f;
        u64     ns    = 0;   // SDL's timestamp: the pump's time
    };
}
