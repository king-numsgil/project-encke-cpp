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

    // Beside a Body: air over it, scattering and absorbing starlight, after
    // Hillaire's "A Scalable and Production Ready Sky and Atmosphere
    // Rendering Technique" (2020). Rayleigh and Mie densities fall off
    // exponentially with altitude above the body's radius; ozone is a tent
    // around `ozone_peak`. SI units: metres, and coefficients per metre. The
    // defaults are the Earth's, from that paper.
    struct Atmosphere
    {
        f64 height = 100'000.0;   // top, above the body's radius

        f32vec3 rayleigh_scattering{5.802e-6f, 13.558e-6f, 33.1e-6f};
        f32     rayleigh_scale_height = 8'000.0f;

        f32vec3 mie_scattering{3.996e-6f};
        f32vec3 mie_absorption{4.40e-7f};
        f32     mie_scale_height = 1'200.0f;
        f32     mie_g            = 0.8f;   // phase asymmetry, forward

        f32vec3 ozone_absorption{0.650e-6f, 1.881e-6f, 0.085e-6f};
        f32     ozone_peak  = 25'000.0f;   // altitude of the tent's apex
        f32     ozone_width = 30'000.0f;   // base of the tent

        bool operator==(Atmosphere const&) const = default;
    };

    // The atmosphere a point sees: the one it is inside, or else the one
    // whose top is nearest. With its body's centre and radius, world space.
    struct AtmosphereView
    {
        f64vec3    centre{0.0};
        f64        radius = 0.0;
        f32vec3    ground_albedo{0.0f};   // the body's, for light it bounces into the air
        Atmosphere atmosphere;
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

    // The atmosphere `point` sees, or nullopt with none: the one whose top is
    // nearest, which is the one it is inside if it is inside any. Reads
    // WorldTransform, so after propagate_transforms. One at a time: a second
    // atmosphere in view is not drawn.
    optional<AtmosphereView> atmosphere_at(entt::registry const& registry, f64vec3 const& point);
}
