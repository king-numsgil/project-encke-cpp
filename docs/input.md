# Input

The goal is a flight sim's input: a HOTAS and other many-button devices,
bound to actions through an editor, with tap, hold, chords, two-stage
triggers and encoders. What exists: the device layer, the tools for looking
at devices, and the action layer with plain pressed, released and held
triggers. The keyboard and mouse drive the engine through it; no default
binding names a joystick yet.

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

From the first mapping log (a guided session, 120 s):

- Stick: hat 0 is 8-way. Axes 0 and 1 are the main X/Y; 3 and 4 move
  together, the mini-stick; 2 swept alone, end to end (twist or the base's
  mini-throttle, unconfirmed). Axes 5 to 7 never moved.
- Stick buttons seen: 0 to 39 but 19 and 22. Button 19 stayed held the
  whole session; 22 and 40 to 43 never appeared. Base switches and the
  selector were likely left alone.
- Encoder candidates, by short uniform presses (pulses run 33-67 ms where
  a hand's presses run 150-900 ms): stick 36 (11 presses, all at most
  50 ms), stick 8 and 9 (5 each, a pair on the base, likely the rotary).
  Stick 25 and 26 alternate like a wheel rolled back and forth but hold up
  to 283 ms. Which pair is the thumbwheel is open.
- Throttle: hat 0 is 8-way; all 14 buttons seen. Axes 0 and 1 move
  together, the thumb stick; 2 and 5 each swept alone, one of them the main
  lever. Axes 3, 4 (resting at +32767), 6 (resting at -3008) and 7 never
  moved.

From binding it in the editor (the author's first `ship` context, kept in
their own `input.json`):

- Stick axis 5 is the twist: it moves, where the first log never saw it.
  Stick axis 2 is then the base's mini-throttle, a slider, which fits it
  resting at -32768.
- Throttle axis 2 is the main lever.
- Stick buttons 23 and 24 are the trigger's two stages; pulling through to
  stage 2 holds stage 1 as well.
- The mini-stick (stick 3 and 4) and the thumb stick (throttle 0 and 1)
  are as the log had them.
- The kit is new and every axis rests at +0.00 in the editor's live
  values, so no binding has a dead zone yet. Revisit as it wears.

What binding it showed is missing:

- The lever runs -1 to 1. The author's driver puts a detent at 50%, which
  reads 0, with forward thrust above it and reverse below, as in Elite
  Dangerous; bound straight, that is the intended mapping. A setup with
  idle at the bottom of the travel wants 0 to 1 instead, and no modifier
  gives that: `scale` has no offset and `split` loses half the travel. It
  would want a `range` modifier.
- `look` is a per-frame delta, made for the mouse's pixels. A stick holds a
  value, so a stick on `look` turns by the same amount every frame, faster
  the higher the frame rate. A stick wants a rate action (radians per
  second, times the frame's time) beside the mouse's delta one: an engine
  change, not a binding.

Open: the order the session followed, to name each index; what stick 19
and 40 to 43 are; where the lever's detents sit; which pair is the
thumbwheel.

## The probe

`encke --input-probe` starts SDL's joystick subsystem alone, opens every
device, lets them report for half a second and prints each one's
description (below) as text. It reads state, so it needs no window and no
focus.

## The device monitor and its log

F7 opens "Input"; its Devices tab shows every joystick live, axes as bars with their raw
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

## The action layer

`src/input/`, on Unreal's Enhanced Input split. Game code reads actions,
never devices; everything an action is bound to is data.

- **Controls** (`input/control`) are one thing on one device, named as
  text in files: `key:W` and `key:Left Shift` (SDL's scancode names, so
  physical positions), `mouse:x`, `mouse:wheel`, `mouse:left`,
  `joy:044f:0422:button:36`, `joy:044f:0447:axis:2`,
  `joy:044f:0422:hat:0:up`. A hat is four buttons. `#n` after the product
  (`joy:044f:0422#1:...`) picks the n-th of identical devices in the order
  they were opened; when one of two is unplugged the other's index can
  shift.
- **Control events** carry a value (0 or 1, an axis's -1 to 1, a mouse
  delta) and SDL's stamp. `input/sdl_input` makes them from SDL events; the
  system itself never sees SDL, so tests and replayed logs push events the
  same way.
- **Actions** are named and typed: bool, 1d, 2d, 3d. A layer declares them;
  the engine looks the ones it reads up by name and type once
  (`InputSystem::require`, in `App::load_input`) and refuses to start if
  one is missing. A mod can declare actions the engine never reads.
- **Bindings** map a control to an action through a chain of modifiers,
  then a trigger. A control's value enters in x. Modifiers
  (`input/modifiers`): `dead_zone` (axial, or radial on an action),
  `curve` (an exponent for now; ImPlot points later), `negate`, `scale`,
  `swizzle` (which component x goes to: WASD into a 2D action), `saturation`,
  `split` (one half of an axis as 0 to 1), `threshold` (an axis as a button,
  with hysteresis). Each is a struct in a tagged variant, so adding one is a
  struct, a case and a name.
- **Triggers** (`input/triggers`): a binding is actuated at magnitude 0.5.
  `down` passes its value every frame and fires on each press, `pressed`
  and `released` fire on the edge and pass their value only that frame.
  Tap, hold, chords and sequences join the same variant.
- **Every event goes through its bindings in order**, so an encoder's
  press and release inside one frame fires once, three clicks fire three
  times, and each fire keeps its event's stamp (`ActionState::fired_ns`).
  Mouse deltas are summed over the frame first.
- **Combining**: an axis action sums its bindings, then applies its own
  modifiers; it is active while nonzero, so A and D together leave it idle.
  A bool action is active while a binding is actuated or it fired this
  frame.
- **Contexts** run by priority, highest first. A binding with `consume`
  (the default) hides its control from lower contexts while its own is
  enabled; bindings within one context all see it. Enabling a context
  starts its bindings from their controls' current values without an edge,
  so a held key does not press. The engine's: `engine` (Escape, F3, Alt),
  always on; `debug` (the other F-keys, T, 1 to 6), off under a scenario;
  `on_foot` and `jetpack`, while the mouse is held. Contexts change for the
  next frame's update.
- **Focus**: losing it releases every button, key and hat (SDL drops
  joystick events in the background, so their releases never come); axes
  keep their values. Unplugging a joystick zeroes its controls. Typing into
  an ImGui field drops key presses but not releases, so F-keys and Escape do
  nothing while a text field has focus.
- **Layers** (`input/layer`): `assets/input/default.json`, then
  `input.json` in the user's folder if present (see the editor's saving,
  below). A layer's action replaces
  the one below by name; within a context, an action's binding list
  replaces the one below whole, and an empty list unbinds; a context's
  priority carries through unless restated. Mods will be more layers on the
  stack. `compile` (`input/input_map`) checks the merged result and refuses
  it whole on any mistake: an undeclared action, a control that does not
  parse, a modifier out of range. Comments are allowed, unknown keys are
  not.
- **Scenarios**: the `action` op holds an action at a value for some
  frames; a bool action fires on the first. `walk` injects `walk_move`,
  `sprint` and `jump`. Injection overrides the devices for that frame.
  Under a scenario the jetpack is not flown from actions, since flying
  integrates wall-clock time; the `fly` op moves the camera instead.

## The editor

F7 opens "Input", whose Bindings tab edits the bindings and whose Devices
tab is the device monitor above. `scenarios/input_editor.json` captures it.

- **The model** (`input/bindings`) holds the defaults and the user's layer.
  An edit replaces one action's binding list in one context and goes to the
  user's layer; a list equal to the defaults' leaves no entry there, so the
  user's file holds only what differs and a later change to the defaults
  still reaches everything the user left alone. Bindings are normalized
  first (`normalized`), so a default stated out loud compares equal.
- **Files** leave defaults out: absent modifiers, trigger and consume read
  as none, `down` and true, and are not written. Contexts carry a
  `description`, since a saved file keeps no comments. `save_layer` writes a
  temporary file and renames it over the old.
- **Saving**: Save writes the user's layer to `input.json` in the user's
  folder, `SDL_GetPrefPath("", "encke")`: `%APPDATA%\encke\` on Windows,
  `~/.local/share/encke/` on Linux, shared by every build and preset and
  untouched by a clean one. Save as defaults writes the merged layer to
  `assets/input/default.json` in the source tree and empties the user's
  layer, its file too: how the repo's defaults are authored.
- **Applying**: the engine compiles after every edit and checks the new map
  still has every action and context it reads (`App::apply_bindings`,
  through `InputMap::require`) before swapping it in; on any failure the
  old map keeps running. Ids are looked up again each time, since a new
  action shifts the ids after it by name.
- **Press-to-bind** (`input/capture`) takes the first key, joystick button
  or hat press, or an axis once it has moved half its range from where it
  first reported, since a resting throttle is noisy. The mouse is never
  taken, or clicking `+` would bind the click; mouse controls are typed.
  Escape cancels. While it waits, the engine acts on no action, so Escape
  does not quit and F-keys toggle nothing.
- **Components**: a control's value enters in x, and on a 2D or 3D action
  each binding picks the component it drives, x, y or z. The pick is a
  swizzle last in the binding's chain (`component_of`, `set_component`),
  shown as `-> y` and not listed with the other modifiers. Press-to-bind
  gives an axis the first component no other axis binding of that action
  takes, so three axes bound in a row are x, y and z; a button stays on x,
  since two buttons are usually one component's two directions.
- **Live**: each action's value is the engine's. A gameplay context is off
  while the mouse is free for the UI, and a context the engine does not
  know is never on, so their actions read zero; Preview keeps the selected
  context on while the window is open (walking too, on foot). Each binding
  lights while its control is held, whatever is enabled.
- New actions and contexts go to the user's layer, and only those can be
  deleted (right-click an action's name; "delete context"): the defaults
  hold everything the engine reads. An action goes with its bindings in
  every context. Bound here only hides the defaults' actions this context
  does not touch; the user's own always show.
- Under a scenario the user's layer is not read, and the UI takes no mouse
  or keys: a capture must not depend on what this machine has bound, or
  on where the cursor rests.
- Not yet: curves in ImPlot, control groups, detent calibration, editing
  an action's own modifiers or a context's priority.

## Plan: the rest of the action layer

Agreed in design, not built.

- **Device identity** is vendor:product; the path only tells identical
  devices apart. No serial on the SOL-R 4.
- **Control groups** the editor declares over raw indices, since the
  engine only sees buttons:
  - a wheel: two buttons marked as a +/- pair, read as a signed count of
    clicks per step, like the mouse wheel; no tap or hold on it;
  - a selector or latched switch: N buttons with one held, read as a
    position, bound on change and while held, never press and release;
  - detents: a threshold on an axis with hysteresis, calibrated in the
    editor by moving to the detent, since they are removable and vary.
- **Arbitration** is the hard part:
  - tap and hold on one button: the tap fires on release before the
    threshold, so it is late by design; a button bound only to a tap fires
    on press;
  - a chord consumes its members, and the higher-priority binding wins:
    shift plus B means X, so B alone must not also fire Y;
  - a two-stage trigger is two buttons, stage 2 closing only while stage 1
    is held: a chord with an order, and the binding says whether stage 1's
    action continues or is replaced;
  - events from one pump are simultaneous: a chord pressed with two
    fingers is one pump's worth, so chords group by pump, not by a window
    of time.
- **Timing** reads event stamps (sub-frame, above), never frame time, so a
  tap is a tap at any frame rate the limiter leaves idle time in.
- **Commands per physics step**: actions are sampled into a command per
  step, not read per frame. Jolt is deterministic given the same calls, so
  recorded commands replay, and later carry multiplayer.
- **The editor**, past its first version: curve editing in ImPlot with the
  live input as a point on the curve, and the control groups above.
- **Tests** replay recorded logs (`input-*.jsonl`) through the mapping
  layer, so arbitration is checked against real hands' timing.
- LEDs and backlights, if ever wanted, go through `SDL_hid_*`, outside the
  action layer.

Order: actions and bindings with modifiers and plain pressed and released
(built), a first editor (built), then the HOTAS bound with it and the
torchship's flight controls on those actions, then tap, hold, chords and
arbitration once there is flying to test them against, then curves and
control groups in the editor.
