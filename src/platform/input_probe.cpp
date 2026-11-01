#include "core/pch.hpp"

#include "platform/input_probe.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"

#include <cstdio>
#include <vector>

namespace encke
{
    namespace
    {
        // Long enough for every backend to deliver its first input report
        // after the device is opened; before it, state reads as zero.
        constexpr u64 kSettleMs = 500;

        char const* type_name(SDL_JoystickType type)
        {
            switch (type)
            {
            case SDL_JOYSTICK_TYPE_GAMEPAD:      return "gamepad";
            case SDL_JOYSTICK_TYPE_WHEEL:        return "wheel";
            case SDL_JOYSTICK_TYPE_ARCADE_STICK: return "arcade stick";
            case SDL_JOYSTICK_TYPE_FLIGHT_STICK: return "flight stick";
            case SDL_JOYSTICK_TYPE_DANCE_PAD:    return "dance pad";
            case SDL_JOYSTICK_TYPE_GUITAR:       return "guitar";
            case SDL_JOYSTICK_TYPE_DRUM_KIT:     return "drum kit";
            case SDL_JOYSTICK_TYPE_ARCADE_PAD:   return "arcade pad";
            case SDL_JOYSTICK_TYPE_THROTTLE:     return "throttle";
            default:                             return "unknown";
            }
        }

        char const* connection_name(SDL_JoystickConnectionState state)
        {
            switch (state)
            {
            case SDL_JOYSTICK_CONNECTION_WIRED:    return "wired";
            case SDL_JOYSTICK_CONNECTION_WIRELESS: return "wireless";
            case SDL_JOYSTICK_CONNECTION_UNKNOWN:  return "unknown";
            default:                               return "invalid";
            }
        }

        char const* hat_name(u8 hat)
        {
            switch (hat)
            {
            case SDL_HAT_CENTERED:  return "centred";
            case SDL_HAT_UP:        return "up";
            case SDL_HAT_RIGHTUP:   return "up-right";
            case SDL_HAT_RIGHT:     return "right";
            case SDL_HAT_RIGHTDOWN: return "down-right";
            case SDL_HAT_DOWN:      return "down";
            case SDL_HAT_LEFTDOWN:  return "down-left";
            case SDL_HAT_LEFT:      return "left";
            case SDL_HAT_LEFTUP:    return "up-left";
            default:                return "?";
            }
        }

        char const* or_none(char const* text)
        {
            return text != nullptr && text[0] != '\0' ? text : "(none)";
        }

        void print_device(SDL_Joystick* joystick)
        {
            SDL_JoystickID const id = SDL_GetJoystickID(joystick);

            char guid[33] = {};
            SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid, sizeof(guid));

            std::printf("device %u: %s\n", static_cast<unsigned>(id), or_none(SDL_GetJoystickName(joystick)));
            std::printf("  vendor:product  %04x:%04x, version %04x, firmware %04x\n",
                        SDL_GetJoystickVendor(joystick), SDL_GetJoystickProduct(joystick),
                        SDL_GetJoystickProductVersion(joystick),
                        SDL_GetJoystickFirmwareVersion(joystick));
            std::printf("  serial          %s\n", or_none(SDL_GetJoystickSerial(joystick)));
            std::printf("  path            %s\n", or_none(SDL_GetJoystickPath(joystick)));
            std::printf("  guid            %s\n", guid);
            std::printf("  type            %s%s, %s\n", type_name(SDL_GetJoystickType(joystick)),
                        SDL_IsGamepad(id) ? " (SDL also maps it as a gamepad)" : "",
                        connection_name(SDL_GetJoystickConnectionState(joystick)));

            int const axes    = SDL_GetNumJoystickAxes(joystick);
            int const buttons = SDL_GetNumJoystickButtons(joystick);
            int const hats    = SDL_GetNumJoystickHats(joystick);
            int const balls   = SDL_GetNumJoystickBalls(joystick);
            std::printf("  counts          %d axes, %d buttons, %d hats, %d balls\n", axes, buttons,
                        hats, balls);

            // Raw values are what bindings will calibrate against; the
            // normalised one is only for reading.
            for (int axis = 0; axis < axes; ++axis)
            {
                i16 const value = SDL_GetJoystickAxis(joystick, axis);
                f32 const norm  = value < 0 ? static_cast<f32>(value) / 32768.0f
                                            : static_cast<f32>(value) / 32767.0f;
                std::printf("  axis %-2d         %6d  (%+.4f)\n", axis, static_cast<int>(value),
                            static_cast<f64>(norm));
            }

            std::vector<int> held;
            for (int button = 0; button < buttons; ++button)
            {
                if (SDL_GetJoystickButton(joystick, button))
                {
                    held.push_back(button);
                }
            }
            std::printf("  held at rest    ");
            if (held.empty())
            {
                std::printf("none");
            }
            for (size_t index = 0; index < held.size(); ++index)
            {
                std::printf(index == 0 ? "%d" : ", %d", held[index]);
            }
            std::printf("\n");

            for (int hat = 0; hat < hats; ++hat)
            {
                std::printf("  hat %-2d          %s\n", hat, hat_name(SDL_GetJoystickHat(joystick, hat)));
            }
        }
    }

    int run_input_probe()
    {
        log::init();
        memory::install_sdl_allocator();

        if (!SDL_Init(SDL_INIT_JOYSTICK))
        {
            log::sdl_error("SDL_Init(SDL_INIT_JOYSTICK)");
            return EXIT_FAILURE;
        }

        int             count = 0;
        SDL_JoystickID* ids   = SDL_GetJoysticks(&count);
        if (ids == nullptr)
        {
            log::sdl_error("SDL_GetJoysticks");
            SDL_Quit();
            return EXIT_FAILURE;
        }

        int const version = SDL_GetVersion();
        std::printf("SDL %d.%d.%d, %d joystick%s\n\n", SDL_VERSIONNUM_MAJOR(version),
                    SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version), count,
                    count == 1 ? "" : "s");

        std::vector<SDL_Joystick*> opened;
        for (int index = 0; index < count; ++index)
        {
            SDL_Joystick* const joystick = SDL_OpenJoystick(ids[index]);
            if (joystick == nullptr)
            {
                log::sdl_error("SDL_OpenJoystick");
                continue;
            }
            opened.push_back(joystick);
        }
        SDL_free(ids);

        u64 const until = SDL_GetTicks() + kSettleMs;
        while (SDL_GetTicks() < until)
        {
            SDL_UpdateJoysticks();
            SDL_Delay(10);
        }
        SDL_UpdateJoysticks();

        for (SDL_Joystick* const joystick : opened)
        {
            print_device(joystick);
            std::printf("\n");
            SDL_CloseJoystick(joystick);
        }

        SDL_Quit();
        return EXIT_SUCCESS;
    }
}
