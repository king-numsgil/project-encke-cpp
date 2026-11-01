#include "core/pch.hpp"

#include "platform/input_log.hpp"

#include <glaze/glaze.hpp>

template<>
struct glz::meta<encke::input_log::Record>
{
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"device", "removed", "button", "axis", "hat"};
};

namespace encke::input_log
{
    string to_json(Record const& record)
    {
        auto const json = glz::write_json(record);
        return json.has_value() ? *json : string{};
    }
}
