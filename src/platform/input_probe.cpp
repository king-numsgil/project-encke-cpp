#include "core/pch.hpp"

#include "platform/input_probe.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "platform/joysticks.hpp"

#include <cstdio>

namespace encke
{
    namespace
    {
        // Long enough for every backend to deliver its first input report
        // after the device is opened; before it, state reads as zero.
        constexpr u64 kSettleMs = 500;

        char const* or_none(string const& text)
        {
            return text.empty() ? "(none)" : text.c_str();
        }

        void print_device(input_log::Device const& device)
        {
            std::printf("device %u: %s\n", device.device, or_none(device.name));
            std::printf("  vendor:product  %04x:%04x, version %04x, firmware %04x\n", device.vendor,
                        device.product, device.version, device.firmware);
            std::printf("  serial          %s\n", or_none(device.serial));
            std::printf("  path            %s\n", or_none(device.path));
            std::printf("  guid            %s\n", device.guid.c_str());
            std::printf("  type            %s%s, %s\n", device.kind.c_str(),
                        device.gamepad ? " (SDL also maps it as a gamepad)" : "",
                        device.connection.c_str());
            std::printf("  counts          %zu axes, %zu buttons, %zu hats, %d balls\n",
                        device.axes.size(), device.buttons.size(), device.hats.size(), device.balls);

            // Raw values are what bindings will calibrate against; the
            // normalised one is only for reading.
            for (size_t axis = 0; axis < device.axes.size(); ++axis)
            {
                i16 const value = device.axes[axis];
                f32 const norm  = value < 0 ? static_cast<f32>(value) / 32768.0f
                                            : static_cast<f32>(value) / 32767.0f;
                std::printf("  axis %-2zu         %6d  (%+.4f)\n", axis, static_cast<int>(value),
                            static_cast<f64>(norm));
            }

            std::printf("  held at rest    ");
            bool any = false;
            for (size_t button = 0; button < device.buttons.size(); ++button)
            {
                if (device.buttons[button] != 0)
                {
                    std::printf(any ? ", %zu" : "%zu", button);
                    any = true;
                }
            }
            std::printf(any ? "\n" : "none\n");

            for (size_t hat = 0; hat < device.hats.size(); ++hat)
            {
                std::printf("  hat %-2zu          %s\n", hat, device.hats[hat].c_str());
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

        vector<SDL_Joystick*> opened;
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
            print_device(describe_joystick(joystick));
            std::printf("\n");
            SDL_CloseJoystick(joystick);
        }

        SDL_Quit();
        return EXIT_SUCCESS;
    }
}
