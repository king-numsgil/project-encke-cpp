#include "core/pch.hpp"

#include "platform/joysticks.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cstdlib>

namespace encke
{
    namespace
    {
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

        string or_empty(char const* text)
        {
            return text != nullptr ? string{text} : string{};
        }

        size_t count_of(int count)
        {
            return count > 0 ? static_cast<size_t>(count) : 0;
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

    input_log::Device describe_joystick(SDL_Joystick* joystick)
    {
        SDL_JoystickID const id = SDL_GetJoystickID(joystick);

        char guid[33] = {};
        SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid, sizeof(guid));

        input_log::Device device{
            .ns         = 0,
            .device     = static_cast<u32>(id),
            .name       = or_empty(SDL_GetJoystickName(joystick)),
            .vendor     = SDL_GetJoystickVendor(joystick),
            .product    = SDL_GetJoystickProduct(joystick),
            .version    = SDL_GetJoystickProductVersion(joystick),
            .firmware   = SDL_GetJoystickFirmwareVersion(joystick),
            .serial     = or_empty(SDL_GetJoystickSerial(joystick)),
            .path       = or_empty(SDL_GetJoystickPath(joystick)),
            .guid       = guid,
            .kind       = type_name(SDL_GetJoystickType(joystick)),
            .gamepad    = SDL_IsGamepad(id),
            .connection = connection_name(SDL_GetJoystickConnectionState(joystick)),
            .balls      = SDL_GetNumJoystickBalls(joystick),
            .axes       = {},
            .buttons    = {},
            .hats       = {},
        };

        for (int axis = 0; axis < SDL_GetNumJoystickAxes(joystick); ++axis)
        {
            device.axes.push_back(SDL_GetJoystickAxis(joystick, axis));
        }
        for (int button = 0; button < SDL_GetNumJoystickButtons(joystick); ++button)
        {
            device.buttons.push_back(SDL_GetJoystickButton(joystick, button) ? 1 : 0);
        }
        for (int hat = 0; hat < SDL_GetNumJoystickHats(joystick); ++hat)
        {
            device.hats.emplace_back(hat_name(SDL_GetJoystickHat(joystick, hat)));
        }
        return device;
    }

    Joysticks::~Joysticks()
    {
        shutdown();
    }

    Joystick* Joysticks::find(SDL_JoystickID id)
    {
        auto const found = std::ranges::find(devices_, id, &Joystick::id);
        return found != devices_.end() ? &*found : nullptr;
    }

    void Joysticks::open(SDL_JoystickID id, u64 timestamp_ns)
    {
        if (find(id) != nullptr)
        {
            return;
        }

        SDL_Joystick* const handle = SDL_OpenJoystick(id);
        if (handle == nullptr)
        {
            log::sdl_error("SDL_OpenJoystick");
            return;
        }

        size_t const axes    = count_of(SDL_GetNumJoystickAxes(handle));
        size_t const buttons = count_of(SDL_GetNumJoystickButtons(handle));
        size_t const hats    = count_of(SDL_GetNumJoystickHats(handle));

        Joystick device{
            .id          = id,
            .handle      = handle,
            .name        = or_empty(SDL_GetJoystickName(handle)),
            .vendor      = SDL_GetJoystickVendor(handle),
            .product     = SDL_GetJoystickProduct(handle),
            .axes        = vector<i16>(axes, 0),
            .buttons     = vector<u8>(buttons, 0),
            .hats        = vector<u8>(hats, 0),
            .axis_min    = vector<i16>(axes, 0),
            .axis_max    = vector<i16>(axes, 0),
            .presses     = vector<u32>(buttons, 0),
            .axis_logged = vector<i16>(axes, 0),
        };

        for (size_t axis = 0; axis < axes; ++axis)
        {
            i16 const value          = SDL_GetJoystickAxis(handle, static_cast<int>(axis));
            device.axes[axis]        = value;
            device.axis_min[axis]    = value;
            device.axis_max[axis]    = value;
            device.axis_logged[axis] = value;
        }

        log::info("joystick %u: %s (%04x:%04x), %zu axes, %zu buttons, %zu hats",
                  static_cast<unsigned>(id), device.name.c_str(), device.vendor, device.product,
                  axes, buttons, hats);
        devices_.push_back(std::move(device));

        if (logging())
        {
            input_log::Device record = describe_joystick(handle);
            record.ns                = since_start(timestamp_ns);
            write(record);
        }
    }

    void Joysticks::close(SDL_JoystickID id, u64 timestamp_ns)
    {
        auto const found = std::ranges::find(devices_, id, &Joystick::id);
        if (found == devices_.end())
        {
            return;
        }

        log::info("joystick %u removed: %s", static_cast<unsigned>(id), found->name.c_str());
        SDL_CloseJoystick(found->handle);
        devices_.erase(found);

        write(input_log::Removed{.ns = since_start(timestamp_ns), .device = static_cast<u32>(id)});
    }

    void Joysticks::handle(SDL_Event const& event)
    {
        switch (event.type)
        {
        case SDL_EVENT_JOYSTICK_ADDED:
            open(event.jdevice.which, event.jdevice.timestamp);
            break;

        case SDL_EVENT_JOYSTICK_REMOVED:
            close(event.jdevice.which, event.jdevice.timestamp);
            break;

        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
        {
            Joystick* const device = find(event.jbutton.which);
            if (device == nullptr || event.jbutton.button >= device->presses.size())
            {
                break;
            }
            if (event.jbutton.down)
            {
                ++device->presses[event.jbutton.button];
            }
            write(input_log::Button{
                .ns     = since_start(event.jbutton.timestamp),
                .device = static_cast<u32>(device->id),
                .button = static_cast<i32>(event.jbutton.button),
                .down   = event.jbutton.down,
            });
            break;
        }

        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        {
            Joystick* const device = find(event.jaxis.which);
            if (device == nullptr || event.jaxis.axis >= device->axes.size())
            {
                break;
            }
            size_t const axis  = event.jaxis.axis;
            i16 const    value = event.jaxis.value;
            device->axis_min[axis] = std::min(device->axis_min[axis], value);
            device->axis_max[axis] = std::max(device->axis_max[axis], value);

            if (std::abs(static_cast<i32>(value) - static_cast<i32>(device->axis_logged[axis])) >=
                kAxisLogStep)
            {
                device->axis_logged[axis] = value;
                write(input_log::Axis{
                    .ns     = since_start(event.jaxis.timestamp),
                    .device = static_cast<u32>(device->id),
                    .axis   = static_cast<i32>(axis),
                    .value  = value,
                });
            }
            break;
        }

        case SDL_EVENT_JOYSTICK_HAT_MOTION:
        {
            Joystick* const device = find(event.jhat.which);
            if (device == nullptr || event.jhat.hat >= device->hats.size())
            {
                break;
            }
            write(input_log::Hat{
                .ns     = since_start(event.jhat.timestamp),
                .device = static_cast<u32>(device->id),
                .hat    = static_cast<i32>(event.jhat.hat),
                .value  = hat_name(event.jhat.value),
            });
            break;
        }

        default:
            break;
        }
    }

    void Joysticks::refresh()
    {
        for (Joystick& device : devices_)
        {
            for (size_t axis = 0; axis < device.axes.size(); ++axis)
            {
                device.axes[axis] = SDL_GetJoystickAxis(device.handle, static_cast<int>(axis));
            }
            for (size_t button = 0; button < device.buttons.size(); ++button)
            {
                device.buttons[button] =
                    SDL_GetJoystickButton(device.handle, static_cast<int>(button)) ? 1 : 0;
            }
            for (size_t hat = 0; hat < device.hats.size(); ++hat)
            {
                device.hats[hat] = SDL_GetJoystickHat(device.handle, static_cast<int>(hat));
            }
        }
    }

    bool Joysticks::start_log(std::filesystem::path const& path)
    {
        stop_log();

        std::FILE* const file = std::fopen(path.string().c_str(), "wb");
        if (file == nullptr)
        {
            log::error("input log: cannot open %s", path.string().c_str());
            return false;
        }

        log_          = file;
        log_path_     = path;
        log_start_ns_ = SDL_GetTicksNS();
        log_lines_    = 0;
        log::info("input log: %s", path.string().c_str());

        // Every device's state now, so what follows reads as changes to it.
        for (Joystick& device : devices_)
        {
            write(describe_joystick(device.handle));
            for (size_t axis = 0; axis < device.axes.size(); ++axis)
            {
                device.axis_logged[axis] = SDL_GetJoystickAxis(device.handle, static_cast<int>(axis));
            }
        }
        return true;
    }

    void Joysticks::stop_log()
    {
        if (log_ == nullptr)
        {
            return;
        }

        std::fclose(log_);
        log_ = nullptr;
        log::info("input log closed: %llu lines in %s", static_cast<unsigned long long>(log_lines_),
                  log_path_.string().c_str());
    }

    u64 Joysticks::since_start(u64 timestamp_ns) const
    {
        return timestamp_ns > log_start_ns_ ? timestamp_ns - log_start_ns_ : 0;
    }

    void Joysticks::write(input_log::Record const& record)
    {
        if (log_ == nullptr)
        {
            return;
        }

        string const line = input_log::to_json(record);
        std::fwrite(line.data(), 1, line.size(), log_);
        std::fputc('\n', log_);
        // Flushed per line, so the file can be read while the engine runs.
        std::fflush(log_);
        ++log_lines_;
    }

    void Joysticks::shutdown()
    {
        stop_log();
        for (Joystick const& device : devices_)
        {
            SDL_CloseJoystick(device.handle);
        }
        devices_.clear();
    }
}
