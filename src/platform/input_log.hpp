#pragma once

#include <variant>

// The records of the joystick log, one JSON object per line (JSONL), tagged
// by "type". Plain aggregates so glaze reflects them; the tag names are in
// input_log.cpp.
//
// Times are `ns`, nanoseconds since the log started, from SDL's event
// timestamps: when SDL pumped the device, so quantised to the pumps (see
// App::idle_until).
// `device` is SDL's instance id, which a `device` record introduces.

namespace encke::input_log
{
    // A joystick's identity and its state when it was described: at the
    // log's start, or when it was plugged in.
    struct Device
    {
        u64    ns     = 0;
        u32    device = 0;
        string name;
        u16    vendor  = 0;
        u16    product = 0;
        u16    version  = 0;
        u16    firmware = 0;
        string serial;
        string path;
        string guid;
        string kind;        // SDL's joystick type
        bool   gamepad = false;   // SDL also maps it as a gamepad
        string connection;
        i32    balls = 0;

        // One entry per control: the counts are the sizes.
        vector<i16>    axes;
        vector<u8>     buttons;   // 1 held
        vector<string> hats;      // hat_name()
    };

    struct Removed
    {
        u64 ns     = 0;
        u32 device = 0;
    };

    struct Button
    {
        u64  ns     = 0;
        u32  device = 0;
        i32  button = 0;
        bool down   = false;
    };

    struct Axis
    {
        u64 ns     = 0;
        u32 device = 0;
        i32 axis   = 0;
        i16 value  = 0;
    };

    struct Hat
    {
        u64    ns     = 0;
        u32    device = 0;
        i32    hat    = 0;
        string value;   // hat_name()
    };

    using Record = std::variant<Device, Removed, Button, Axis, Hat>;

    // One line, without its newline.
    string to_json(Record const& record);
}
