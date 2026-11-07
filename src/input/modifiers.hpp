#pragma once

#include <variant>

// What a binding, or an action, does to a value on its way: each modifier
// takes a vector and returns one, in the order listed. A control's value
// arrives in x; a swizzle moves it to another component. Plain aggregates,
// so the binding files read and write them by reflection; the names the
// files use are in input/layer.cpp.
namespace encke::input
{
    // Values in a component below `lower` read as zero, and `lower` to
    // `upper` is stretched to 0 to 1, sign kept. Radial measures the
    // vector's length rather than each component, for a stick: on an
    // action, after its bindings have been summed.
    struct DeadZone
    {
        f32  lower  = 0.2f;
        f32  upper  = 1.0f;
        bool radial = false;

        bool operator==(DeadZone const&) const = default;
    };

    // sign(x) |x|^exponent per component: above 1 softens the centre.
    struct Curve
    {
        f32 exponent = 2.0f;

        bool operator==(Curve const&) const = default;
    };

    // Invert.
    struct Negate
    {
        bool operator==(Negate const&) const = default;
    };

    // Per component. A button as an axis is a button scaled.
    struct Scale
    {
        array<f32, 3> factor{{1.0f, 1.0f, 1.0f}};

        bool operator==(Scale const&) const = default;
    };

    // Where each input component goes, as letters: the first is where x
    // goes. "yxz" puts x in y, so a key bound to a 2D action moves it
    // forward.
    struct Swizzle
    {
        string order = "yxz";

        bool operator==(Swizzle const&) const = default;
    };

    // Full output is reached at `input` of the travel, and full output is
    // `output`: clamp(x / input, -1, 1) * output, per component.
    struct Saturation
    {
        f32 input  = 1.0f;
        f32 output = 1.0f;

        bool operator==(Saturation const&) const = default;
    };

    enum class Half : u8
    {
        Positive,
        Negative,
    };

    // One half of an axis, as 0 to 1: the negative half is flipped. Two
    // actions, or two bindings of one, can share an axis this way.
    struct Split
    {
        Half half = Half::Positive;

        bool operator==(Split const&) const = default;
    };

    // An axis as a button: 1 from when its magnitude reaches `press` until
    // it falls below `release`, else 0. The gap keeps a noisy axis from
    // chattering.
    struct Threshold
    {
        f32 press   = 0.5f;
        f32 release = 0.4f;

        bool operator==(Threshold const&) const = default;
    };

    using Modifier = std::variant<DeadZone, Curve, Negate, Scale, Swizzle, Saturation, Split, Threshold>;

    // What a modifier carries between one value and the next: only a
    // threshold has any. One per modifier in a chain.
    struct ModifierState
    {
        bool on = false;
    };

    f32vec3 apply(Modifier const& modifier, f32vec3 value, ModifierState& state);

    // `states` holds one per modifier.
    f32vec3 apply(span<Modifier const> modifiers, f32vec3 value, span<ModifierState> states);

    // What is wrong with a modifier's settings, or nothing.
    optional<string> check(Modifier const& modifier);

    // A value's largest component, unsigned: what decides whether a binding
    // is actuated.
    f32 magnitude(f32vec3 const& value);
}
