#include "core/pch.hpp"

#include "world/bodies.hpp"

#include "world/transform.hpp"

#include <limits>

namespace encke
{
    optional<Starlight> starlight_at(entt::registry const& registry, f64vec3 const& point)
    {
        optional<Starlight> brightest;
        for (auto const [entity, world, star] : registry.view<WorldTransform const, Star const>().each())
        {
            f64vec3 const delta       = world.position - point;
            f64 const     illuminance = star.luminous_intensity / glm::dot(delta, delta);
            if (!brightest.has_value() || illuminance > brightest->illuminance)
            {
                brightest = Starlight{
                    .direction   = glm::normalize(delta),
                    .illuminance = illuminance,
                    .colour      = star.colour,
                };
            }
        }
        return brightest;
    }

    optional<Surroundings> surroundings_at(entt::registry const& registry, f64vec3 const& point)
    {
        optional<Surroundings> nearest;
        f64 altitude = std::numeric_limits<f64>::infinity();
        for (auto const [entity, world, body] : registry.view<WorldTransform const, Body const>().each())
        {
            f64vec3 const centre = world.position + world.rotation * body.centre;
            f64vec3 const away   = point - centre;
            f64 const     above  = glm::length(away) - body.radius;
            if (above < altitude)
            {
                altitude = above;
                nearest  = Surroundings{
                    .up            = glm::normalize(away),
                    .ground_albedo = body.ground_albedo,
                    .sky_fill      = body.sky_fill,
                };
            }
        }
        return nearest;
    }

    optional<AtmosphereView> atmosphere_at(entt::registry const& registry, f64vec3 const& point)
    {
        optional<AtmosphereView> nearest;
        f64 above_top = std::numeric_limits<f64>::infinity();
        for (auto const [entity, world, body, atmosphere] :
             registry.view<WorldTransform const, Body const, Atmosphere const>().each())
        {
            f64vec3 const centre = world.position + world.rotation * body.centre;
            f64 const     above  = glm::length(point - centre) - body.radius - atmosphere.height;
            if (above < above_top)
            {
                above_top = above;
                nearest   = AtmosphereView{
                    .centre        = centre,
                    .radius        = body.radius,
                    .ground_albedo = body.ground_albedo,
                    .atmosphere    = atmosphere,
                };
            }
        }
        return nearest;
    }
}
