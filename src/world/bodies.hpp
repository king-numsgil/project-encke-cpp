#pragma once

namespace encke
{
    // A star: a point light at its entity's world position, so far from
    // anything it lights that one direction and one illuminance serve a
    // whole scene. Not drawn.
    struct Star
    {
        f64     luminous_intensity = 0.0;  // candela
        f32vec3 colour{1.0f};              // linear tint, multiplied by the illuminance
    };

    // A spherical body -- a planet, a moon -- of `radius` metres. Its centre
    // is `centre` in the entity's frame, which need not be the entity's
    // origin: the test planet's mesh is built around its north pole.
    //
    // The environment is what a surface near the body is filled by and
    // reflects, a stand-in for bounce light until there is any GI: the
    // ground below the horizon, lit by the star, and the sky above it. The
    // ground's radiance is worked out per frame from the star, so only its
    // albedo is here. An airless sky is black, which leaves anything facing
    // up in shadow black too; `sky_fill` gives the sky that fraction of the
    // ground's radiance anyway, for the light nearby objects would bounce,
    // which a two-colour environment cannot know about.
    struct Body
    {
        f64     radius = 0.0;
        f64vec3 centre{0.0};

        f32vec3 ground_albedo{0.0f};   // linear
        f32     sky_fill = 0.0f;
    };

    // The star lighting a point: toward it, unit, and its illuminance there.
    struct Starlight
    {
        f64vec3 direction{0.0, 1.0, 0.0};
        f64     illuminance = 0.0;     // lux: inverse square, no atmosphere
        f32vec3 colour{1.0f};
    };

    // The environment around a point: its body's, with the horizon set by
    // the body's up there.
    struct Surroundings
    {
        f64vec3 up{0.0, 1.0, 0.0};     // away from the body's centre, unit
        f32vec3 ground_albedo{0.0f};
        f32     sky_fill = 0.0f;
    };

    // The star giving the most illuminance at `point`, or nullopt with no
    // star. Reads WorldTransform, so after propagate_transforms.
    optional<Starlight> starlight_at(entt::registry const& registry, f64vec3 const& point);

    // The environment of the body whose surface is nearest `point`, or
    // nullopt with no body. Reads WorldTransform, so after
    // propagate_transforms. Nearest, not a blend: flying from one body to
    // another switches environment halfway.
    optional<Surroundings> surroundings_at(entt::registry const& registry, f64vec3 const& point);
}
