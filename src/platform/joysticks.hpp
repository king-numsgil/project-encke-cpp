#pragma once

#include <cstdio>
#include <filesystem>

#include "platform/input_log.hpp"

namespace encke
{
    // One opened joystick: its identity, the state SDL holds for it, and
    // what its events have shown since it was opened.
    struct Joystick
    {
        SDL_JoystickID id     = 0;
        SDL_Joystick*  handle = nullptr;
        string         name;
        u16            vendor  = 0;
        u16            product = 0;

        // Read from SDL each frame by refresh(), for display.
        vector<i16> axes;
        vector<u8>  buttons;
        vector<u8>  hats;

        // The extremes each axis has reached since it was opened, for
        // calibration.
        vector<i16> axis_min;
        vector<i16> axis_max;

        // Button-down events per button. Counted from events, never from
        // state: an encoder's pulse can press and release between two
        // frames, and only the events see it.
        vector<u32> presses;

        // The value each axis last had in the log, so small noise is not
        // logged.
        vector<i16> axis_logged;
    };

    // Every connected joystick, opened as SDL reports it and closed as it
    // leaves, and an optional log of everything they do.
    //
    // The log is how input is examined away from the screen: JSONL (see
    // platform/input_log), one record per button edge, hat change and axis
    // move, stamped with SDL's event time. That is when SDL pumped the
    // device, once a frame, not when the device reported: durations come out
    // in whole frames. It opens with a `device` record per joystick,
    // holding its state at that moment.
    class Joysticks
    {
    public:
        // An axis is logged when it has moved this far, raw, from the value
        // last logged: about 1.5% of its travel.
        static constexpr i32 kAxisLogStep = 512;

        Joysticks() = default;
        ~Joysticks();

        Joysticks(Joysticks const&)            = delete;
        Joysticks& operator=(Joysticks const&) = delete;
        Joysticks(Joysticks&&)                 = delete;
        Joysticks& operator=(Joysticks&&)      = delete;

        // Needs SDL's joystick subsystem started. Events of other kinds are
        // ignored.
        void handle(SDL_Event const& event);

        // Copies SDL's current state into each device, once a frame.
        void refresh();

        span<Joystick const> devices() const { return devices_; }

        // Starts a new log at `path`, replacing one already open.
        bool start_log(std::filesystem::path const& path);
        void stop_log();

        bool                         logging() const { return log_ != nullptr; }
        std::filesystem::path const& log_path() const { return log_path_; }
        u64                          logged_lines() const { return log_lines_; }

        // Closes the log and every device. Must run before SDL_Quit.
        void shutdown();

    private:
        Joystick* find(SDL_JoystickID id);
        void      open(SDL_JoystickID id, u64 timestamp_ns);
        void      close(SDL_JoystickID id, u64 timestamp_ns);

        // Nanoseconds since the log started, for an SDL timestamp.
        u64  since_start(u64 timestamp_ns) const;
        void write(input_log::Record const& record);

        vector<Joystick> devices_;

        std::FILE*            log_ = nullptr;
        std::filesystem::path log_path_;
        u64                   log_start_ns_ = 0;
        u64                   log_lines_    = 0;
    };

    // What SDL reports for an opened joystick now, as the log records it.
    // --input-probe prints the same record as text.
    input_log::Device describe_joystick(SDL_Joystick* joystick);

    char const* hat_name(u8 hat);
}
