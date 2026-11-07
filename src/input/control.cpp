#include "core/pch.hpp"

#include "input/control.hpp"

#include <charconv>
#include <cstdio>

namespace encke::input
{
    namespace
    {
        struct Named
        {
            string_view name;
            u16         index;
        };

        constexpr array<Named, 5> kMouseButtons{{
            {.name = "left", .index = SDL_BUTTON_LEFT},
            {.name = "middle", .index = SDL_BUTTON_MIDDLE},
            {.name = "right", .index = SDL_BUTTON_RIGHT},
            {.name = "x1", .index = SDL_BUTTON_X1},
            {.name = "x2", .index = SDL_BUTTON_X2},
        }};

        struct NamedHat
        {
            string_view  name;
            HatDirection direction;
        };

        constexpr array<NamedHat, 4> kHatDirections{{
            {.name = "up", .direction = HatDirection::Up},
            {.name = "right", .direction = HatDirection::Right},
            {.name = "down", .direction = HatDirection::Down},
            {.name = "left", .direction = HatDirection::Left},
        }};

        // Takes text up to the next ':' (or the end) off the front of `text`.
        string_view take(string_view& text)
        {
            size_t const colon = text.find(':');
            string_view  part  = text.substr(0, colon);
            text               = colon == string_view::npos ? string_view{} : text.substr(colon + 1);
            return part;
        }

        template<class T>
        optional<T> number(string_view text, int base)
        {
            T value{};
            auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
            if (error != std::errc{} || end != text.data() + text.size() || text.empty())
            {
                return nullopt;
            }
            return value;
        }

        optional<Control> parse_joystick(string_view rest)
        {
            optional<u16> const vendor = number<u16>(take(rest), 16);
            string_view         second = take(rest);

            u8 instance = 0;
            if (size_t const hash = second.find('#'); hash != string_view::npos)
            {
                optional<u8> const n = number<u8>(second.substr(hash + 1), 10);
                if (!n.has_value())
                {
                    return nullopt;
                }
                instance = *n;
                second   = second.substr(0, hash);
            }
            optional<u16> const product = number<u16>(second, 16);
            if (!vendor.has_value() || !product.has_value())
            {
                return nullopt;
            }

            string_view const   kind  = take(rest);
            optional<u16> const index = number<u16>(take(rest), 10);
            if (!index.has_value())
            {
                return nullopt;
            }

            Control control{
                .kind      = ControlKind::JoyButton,
                .index     = *index,
                .direction = HatDirection::None,
                .device    = DeviceId{.vendor = *vendor, .product = *product, .instance = instance},
            };
            if (kind == "button" && rest.empty())
            {
                return control;
            }
            if (kind == "axis" && rest.empty())
            {
                control.kind = ControlKind::JoyAxis;
                return control;
            }
            if (kind == "hat")
            {
                for (NamedHat const& hat : kHatDirections)
                {
                    if (rest == hat.name)
                    {
                        control.kind      = ControlKind::JoyHat;
                        control.direction = hat.direction;
                        return control;
                    }
                }
            }
            return nullopt;
        }
    }

    optional<Control> parse_control(string_view text)
    {
        string_view       rest   = text;
        string_view const device = take(rest);

        if (device == "key")
        {
            // SDL's names, which have spaces in them; a table lookup that
            // needs no SDL_Init.
            string const          name{rest};
            SDL_Scancode const    code = SDL_GetScancodeFromName(name.c_str());
            if (code == SDL_SCANCODE_UNKNOWN)
            {
                return nullopt;
            }
            return Control{.kind      = ControlKind::Key,
                           .index     = static_cast<u16>(code),
                           .direction = HatDirection::None,
                           .device    = {}};
        }

        if (device == "mouse")
        {
            Control control{.kind = ControlKind::MouseAxis, .index = 0, .direction = HatDirection::None, .device = {}};
            if (rest == "x")
            {
                return control;
            }
            if (rest == "y")
            {
                control.index = 1;
                return control;
            }
            if (rest == "wheel")
            {
                control.kind = ControlKind::MouseWheel;
                return control;
            }
            for (Named const& button : kMouseButtons)
            {
                if (rest == button.name)
                {
                    control.kind  = ControlKind::MouseButton;
                    control.index = button.index;
                    return control;
                }
            }
            return nullopt;
        }

        if (device == "joy")
        {
            return parse_joystick(rest);
        }
        return nullopt;
    }

    string format_control(Control const& control)
    {
        switch (control.kind)
        {
        case ControlKind::Key:
            return "key:"s + SDL_GetScancodeName(static_cast<SDL_Scancode>(control.index));

        case ControlKind::MouseAxis:
            return control.index == 0 ? "mouse:x" : "mouse:y";

        case ControlKind::MouseWheel:
            return "mouse:wheel";

        case ControlKind::MouseButton:
            for (Named const& button : kMouseButtons)
            {
                if (button.index == control.index)
                {
                    return "mouse:"s + string{button.name};
                }
            }
            return "mouse:?";

        case ControlKind::JoyButton:
        case ControlKind::JoyAxis:
        case ControlKind::JoyHat:
            break;
        }

        char device[32];
        if (control.device.instance == 0)
        {
            std::snprintf(device, sizeof(device), "joy:%04x:%04x", control.device.vendor, control.device.product);
        }
        else
        {
            std::snprintf(device, sizeof(device), "joy:%04x:%04x#%u", control.device.vendor,
                          control.device.product, static_cast<unsigned>(control.device.instance));
        }

        string text = device;
        if (control.kind == ControlKind::JoyButton)
        {
            return text + ":button:" + std::to_string(control.index);
        }
        if (control.kind == ControlKind::JoyAxis)
        {
            return text + ":axis:" + std::to_string(control.index);
        }

        text += ":hat:" + std::to_string(control.index) + ":";
        for (NamedHat const& hat : kHatDirections)
        {
            if (hat.direction == control.direction)
            {
                return text + string{hat.name};
            }
        }
        return text + "?";
    }
}
