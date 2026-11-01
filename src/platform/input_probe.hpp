#pragma once

namespace encke
{
    // `encke --input-probe`: starts SDL's joystick subsystem alone, opens
    // every connected joystick and prints what SDL reports for it -- identity
    // (name, vendor:product, serial, path, GUID), type, axis, button and hat
    // counts, and the state at rest: every axis value, every button already
    // held (a latched switch reads as held) and every hat off centre. The
    // same record the input log opens with (platform/input_log), as text.
    // Reads state rather than events, so it needs no window and no focus.
    // Printed to stdout. Returns the process exit code.
    int run_input_probe();
}
