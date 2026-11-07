#include "core/pch.hpp"

#include "input/modifiers.hpp"

#include <algorithm>
#include <cmath>

namespace encke::input
{
    namespace
    {
        f32 stretch(f32 x, f32 lower, f32 upper)
        {
            f32 const size = std::abs(x);
            if (size < lower)
            {
                return 0.0f;
            }
            return std::copysign(std::min((size - lower) / (upper - lower), 1.0f), x);
        }

        template<class F>
        f32vec3 each(f32vec3 const& v, F const& f)
        {
            return f32vec3{f(v.x), f(v.y), f(v.z)};
        }

        f32vec3 dead_zone(DeadZone const& zone, f32vec3 const& value)
        {
            if (!zone.radial)
            {
                return each(value, [&](f32 x) { return stretch(x, zone.lower, zone.upper); });
            }
            f32 const length = glm::length(value);
            if (length <= 0.0f)
            {
                return value;
            }
            return value * (stretch(length, zone.lower, zone.upper) / length);
        }

        size_t component(char letter)
        {
            return static_cast<size_t>(letter - 'x');
        }
    }

    f32 magnitude(f32vec3 const& value)
    {
        return std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
    }

    f32vec3 apply(Modifier const& modifier, f32vec3 value, ModifierState& state)
    {
        return std::visit(
            [&](auto const& m) -> f32vec3 {
                using M = std::decay_t<decltype(m)>;

                if constexpr (std::is_same_v<M, DeadZone>)
                {
                    return dead_zone(m, value);
                }
                else if constexpr (std::is_same_v<M, Curve>)
                {
                    return each(value, [&](f32 x) { return std::copysign(std::pow(std::abs(x), m.exponent), x); });
                }
                else if constexpr (std::is_same_v<M, Negate>)
                {
                    return -value;
                }
                else if constexpr (std::is_same_v<M, Scale>)
                {
                    return value * f32vec3{m.factor[0], m.factor[1], m.factor[2]};
                }
                else if constexpr (std::is_same_v<M, Swizzle>)
                {
                    // Validated by check(): three of x, y, z.
                    f32vec3 out{0.0f};
                    for (size_t axis = 0; axis < 3; ++axis)
                    {
                        out[component(m.order[axis])] = value[axis];
                    }
                    return out;
                }
                else if constexpr (std::is_same_v<M, Saturation>)
                {
                    return each(value, [&](f32 x) { return std::clamp(x / m.input, -1.0f, 1.0f) * m.output; });
                }
                else if constexpr (std::is_same_v<M, Split>)
                {
                    f32 const sign = m.half == Half::Positive ? 1.0f : -1.0f;
                    return each(value, [&](f32 x) { return std::max(x * sign, 0.0f); });
                }
                else
                {
                    static_assert(std::is_same_v<M, Threshold>);
                    f32 const size = magnitude(value);
                    state.on       = state.on ? size >= m.release : size >= m.press;
                    return f32vec3{state.on ? 1.0f : 0.0f, 0.0f, 0.0f};
                }
            },
            modifier);
    }

    f32vec3 apply(span<Modifier const> modifiers, f32vec3 value, span<ModifierState> states)
    {
        for (size_t index = 0; index < modifiers.size(); ++index)
        {
            value = apply(modifiers[index], value, states[index]);
        }
        return value;
    }

    optional<string> check(Modifier const& modifier)
    {
        return std::visit(
            [](auto const& m) -> optional<string> {
                using M = std::decay_t<decltype(m)>;

                if constexpr (std::is_same_v<M, DeadZone>)
                {
                    if (!(m.lower >= 0.0f && m.lower < m.upper))
                    {
                        return "dead_zone wants 0 <= lower < upper";
                    }
                }
                else if constexpr (std::is_same_v<M, Curve>)
                {
                    if (!(m.exponent > 0.0f))
                    {
                        return "curve wants a positive exponent";
                    }
                }
                else if constexpr (std::is_same_v<M, Swizzle>)
                {
                    string sorted = m.order;
                    std::ranges::sort(sorted);
                    if (sorted != "xyz")
                    {
                        return "swizzle wants an order of x, y and z, each once, got \"" + m.order + "\"";
                    }
                }
                else if constexpr (std::is_same_v<M, Saturation>)
                {
                    if (!(m.input > 0.0f))
                    {
                        return "saturation wants a positive input";
                    }
                }
                else if constexpr (std::is_same_v<M, Threshold>)
                {
                    if (!(m.release >= 0.0f && m.release <= m.press))
                    {
                        return "threshold wants 0 <= release <= press";
                    }
                }
                return nullopt;
            },
            modifier);
    }
}
