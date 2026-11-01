# Input

The goal is a flight sim's input: a HOTAS and other many-button devices,
bound to actions through an editor, with tap, hold, chords, two-stage
triggers and encoders. What exists so far is the device layer and the tools
for looking at devices; no game code reads a joystick yet.

## Devices

- Joysticks are read through SDL's raw `SDL_Joystick` API, never
  `SDL_Gamepad`: the gamepad API maps to an Xbox layout and drops what does
  not fit, which on a HOTAS is most of it.
- `Window::init` starts `SDL_INIT_JOYSTICK` with video. SDL then sends one
  `SDL_EVENT_JOYSTICK_ADDED` per device already connected, so startup and
  hotplug are the same path: `platform/joysticks` opens on ADDED and closes
  on REMOVED.
- **Count events, never sample state.** Button presses are counted from
  `SDL_EVENT_JOYSTICK_BUTTON_DOWN`. An encoder's pulse can press and release
  between two frames, and reading `SDL_GetJoystickButton` once a frame
  misses it, more often the lower the frame rate. State, read by
  `Joysticks::refresh()`, is for display only.
- SDL drops joystick events while the window is in the background (its
  default), as it does keyboard ones. Anything that watches events runs with
  the engine focused.
- Event timestamps are SDL's, in nanoseconds, but **they are the pump's
  time, not the device's**: SDL reads joysticks, and the OS's queue, when
  events are pumped. Pumped once a frame, as at first, every press duration
  on the SOL-R 4 at the 60 Hz limiter was a multiple of 16.7 ms.
- **Sub-frame event stamping**: the limiter pumps in its idle time
  (`App::idle_until`), on a grid of at
  most 120 Hz from the frame's start. Pumping only queues events;
  `Window::poll` still takes them all at the top of the frame, in order, so
  ImGui and everything else sees what it did before with finer stamps. It
  covers the keyboard and mouse as well as joysticks, on any SDL backend.
- Where there is no idle time -- uncapped, or a frame whose work fills its
  period, as in the sanitize build -- nothing pumps there and stamps fall
  back to the frame. Against tap and hold thresholds of a few hundred ms
  either is fine. A dedicated thread calling `SDL_UpdateJoysticks` would be
  frame-independent everywhere but covers joysticks only, and only on
  backends that poll; it was not chosen.
- Pulses are counted from events whatever the stamps: at a low frame rate
  one falls inside a single pump.

## The SOL-R 4 HOTAS

As the probe reports it on the author's machine:

| Device | SDL name | vendor:product | Axes | Buttons | Hats |
| --- | --- | --- | --- | --- | --- |
| stick | Thrustmaster Sol-R [R] Flightstick | `044f:0422` | 8 | 44 | 1 |
| throttle | Thrustmaster Sol-R 6 Throttle | `044f:0447` | 8 | 14 | 1 |

- Each half reports under its own product's name; the stick's path has
  `MI_00`, interface 0 of a composite device.
- Neither has a serial number, and the path carries a Windows instance id
  that changes with the USB port. Identify a device by vendor:product; the
  path matters only between two identical devices.
- SDL calls both type `unknown` and maps neither as a gamepad.
- The stick sees all 44 buttons. Its grip has two hats and SDL reports one,
  so the other arrives as buttons.
- The stick holds button 19 at rest: a latched switch or selector position.
  Axis 2 rests at -32768, a slider at its end, likely the base's
  mini-throttle.
- The throttle has 8 axes where the specification lists 5.
- The stick's thumbwheel is an encoder: one button press per detent click.

Which physical control each index is remains to be mapped with the log.

## The probe

`encke --input-probe` starts SDL's joystick subsystem alone, opens every
device, lets them report for half a second and prints each one's
description (below) as text. It reads state, so it needs no window and no
focus.

## The device monitor and its log

F7 opens "Input devices": every joystick live, axes as bars with their raw
value and the range reached since it was opened, buttons lit while held
with their press count on hover, hats by direction.

"Start log" writes `input-YYYYMMDD-HHMMSS.jsonl` beside the executable,
flushed per line so it can be read while the engine runs. One JSON object
per line, tagged by `type` (`platform/input_log`, pinned by
`tests/platform/input_log_test`):

- `device`: identity (name, vendor, product, serial, path, GUID, SDL's
  type), and its state: `axes` raw, `buttons` 1 held, `hats` by name. One per
  device when the log starts, and one when a device is plugged in.
- `button` (`button`, `down`), `axis` (`axis`, `value` raw), `hat`
  (`hat`, `value` by name), `removed`.

Every event carries `ns`, nanoseconds since the log started from SDL's
timestamps (pump time, see above), and `device`, SDL's instance id, which a `device` record
introduces. An axis is logged when it has moved 512 raw (about 1.5%) from
the value last logged, so resting noise stays out.

The probe prints the same `device` record: `describe_joystick` builds it
for both.
