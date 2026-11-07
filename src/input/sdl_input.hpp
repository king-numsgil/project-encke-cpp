#pragma once

#include "input/input_system.hpp"
#include "platform/joysticks.hpp"

// SDL's events as control events. The input system never sees SDL; replayed
// logs and tests push control events the same way.
namespace encke::input
{
    // Must see a joystick's REMOVED before `joysticks` closes it, so the
    // device's controls can be let go by its identity.
    //
    // With `keys` false, key presses are dropped and releases still pass, so
    // typing into the UI drives nothing and a key held across it lets go.
    void translate(SDL_Event const& event, Joysticks const& joysticks, bool keys, InputSystem& input);

    // A joystick's identity, by SDL's instance id: vendor:product, and its
    // place among identical devices in the order they were opened.
    optional<DeviceId> device_id(Joysticks const& joysticks, SDL_JoystickID id);
}
