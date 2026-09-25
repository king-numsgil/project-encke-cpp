// Atmospheric scattering after Hillaire, "A Scalable and Production Ready
// Sky and Atmosphere Rendering Technique" (EGSR 2020): Rayleigh and Mie
// scattering, Mie and ozone absorption, single scattering marched along the
// view ray and every higher order read from a multiple-scattering LUT.
//
// Kilometres throughout. Positions come in two kinds: view space, the camera
// at the origin, and "from the centre", relative to the body's centre.
// Altitudes near the camera are worked out from its own altitude, which the
// CPU computed in f64, rather than as a length of a vector millions of
// metres long minus the radius: that difference in f32 is off by a metre,
// and the ground sphere's intersections with it by far more.

module atmosphere;

import lib.bindless;
import lib.gpu_types;

public static const float kPi = 3.14159265358979f;

// The body's radius is a reference sphere its terrain rises and falls about
// by kilometres; the solid that shadows the air and ends a view ray is taken
// to lie this far inside it, so a valley below the radius is not in the
// planet's own shadow. Anything between is air at sea-level density, dark
// enough along a grazing ray that the difference does not show.
public static const float kGroundDepth = 10.0f;

public bool atmosphere_present(GpuFrame frame)
{
    return frame.atmosphere.x != kNoAtmosphere;
}

public GpuAtmosphere load_atmosphere(GpuFrame frame)
{
    return g_buffers[NonUniformResourceIndex(frame.atmosphere.x)].Load<GpuAtmosphere>(0);
}

public float bottom_radius(GpuAtmosphere a) { return a.planet.w; }
public float top_radius(GpuAtmosphere a) { return a.shell.x; }
public float thickness(GpuAtmosphere a) { return a.shell.x - a.planet.w; }
public float ground_radius(GpuAtmosphere a) { return a.planet.w - kGroundDepth; }

// The camera, from the centre.
public float3 camera_position(GpuAtmosphere a) { return -a.planet.xyz; }
public float camera_altitude(GpuAtmosphere a) { return a.shell.y; }

// |o|^2 - radius^2 for a point `above` a sphere of `radius`: exact where
// the subtraction of two squares would cancel.
public float squared_excess(float above, float radius)
{
    return above * (2.0f * radius + above);
}

// Near and far t along a ray from o, unit direction d, through a sphere
// about the centre, given b = dot(o, d) and c = |o|^2 - radius^2. Both -1
// when it misses.
public float2 sphere_hits(float b, float c)
{
    float discriminant = b * b - c;
    if (discriminant < 0.0f)
    {
        return float2(-1.0f, -1.0f);
    }
    float root = sqrt(discriminant);
    return float2(-b - root, -b + root);
}

// Altitude above the radius of the view-space point p, precise near the
// camera: from the camera's own altitude plus the change along p.
public float altitude_at(GpuAtmosphere a, float3 p)
{
    float3 o      = camera_position(a);
    float  from_o = bottom_radius(a) + camera_altitude(a);
    float  from_p = length(o + p);
    return camera_altitude(a) + (2.0f * dot(o, p) + dot(p, p)) / (from_p + from_o);
}

public struct Medium
{
    public float3 rayleigh;     // scattering
    public float3 mie;          // scattering
    public float3 scattering;   // both
    public float3 extinction;
};

public Medium medium_at(GpuAtmosphere a, float altitude)
{
    float h                = max(altitude, 0.0f);
    float rayleigh_density = exp(-h / a.rayleigh.w);
    float mie_density      = exp(-h / a.mie.w);
    float ozone_density    = max(0.0f, 1.0f - abs(h - a.ozone.w) / a.ground.w);

    Medium medium;
    medium.rayleigh   = a.rayleigh.rgb * rayleigh_density;
    medium.mie        = a.mie.rgb * mie_density;
    medium.scattering = medium.rayleigh + medium.mie;
    medium.extinction = medium.scattering + a.mie_absorption.rgb * mie_density +
                        a.ozone.rgb * ozone_density;
    return medium;
}

public float rayleigh_phase(float cos_theta)
{
    return 3.0f / (16.0f * kPi) * (1.0f + cos_theta * cos_theta);
}

// Cornette-Shanks, as Hillaire uses.
public float mie_phase(float g, float cos_theta)
{
    float k     = 3.0f / (8.0f * kPi) * (1.0f - g * g) / (2.0f + g * g);
    float denom = max(1.0f + g * g - 2.0f * g * cos_theta, 1e-4f);
    return k * (1.0f + cos_theta * cos_theta) / (denom * sqrt(denom));
}

// -- transmittance LUT ------------------------------------------------------
// Transmittance from a point to the top of the atmosphere, by altitude and
// the cosine of the direction's zenith angle there, in Bruneton's
// parameterisation: v is the distance to the horizon over its maximum, u
// the distance to the top between its least and greatest at that altitude.
// The ground is ignored; below the horizon the ray runs on through air at
// sea-level density, which leaves nothing.

public float2 transmittance_uv(GpuAtmosphere a, float altitude, float mu)
{
    float R   = bottom_radius(a);
    float top = top_radius(a);
    float alt = clamp(altitude, 0.0f, thickness(a));
    float r   = R + alt;

    float H   = sqrt(squared_excess(thickness(a), R));
    float rho = sqrt(squared_excess(alt, R));

    float c     = squared_excess(alt - thickness(a), top);
    float d     = max(0.0f, -r * mu + sqrt(max(r * r * mu * mu - c, 0.0f)));
    float d_min = top - r;
    float d_max = rho + H;
    return float2((d - d_min) / max(d_max - d_min, 1e-6f), rho / H);
}

public void transmittance_parameters(GpuAtmosphere a, float2 uv, out float altitude, out float mu)
{
    float R   = bottom_radius(a);
    float top = top_radius(a);
    float H   = sqrt(squared_excess(thickness(a), R));
    float rho = H * uv.y;
    float r   = sqrt(rho * rho + R * R);

    altitude    = rho * rho / (r + R);
    float d_min = top - r;
    float d_max = rho + H;
    float d     = d_min + uv.x * (d_max - d_min);
    mu          = d == 0.0f ? 1.0f : clamp((H * H - rho * rho - d * d) / (2.0f * r * d), -1.0f, 1.0f);
}

public float3 transmittance_lut(GpuFrame frame, GpuAtmosphere a, float altitude, float mu)
{
    float2 uv = transmittance_uv(a, altitude, mu);
    return g_textures[NonUniformResourceIndex(frame.atmosphere.y)]
        .SampleLevel(g_samplers[NonUniformResourceIndex(frame.atmosphere.w)], uv, 0.0f)
        .rgb;
}

// -- multiple-scattering LUT -------------------------------------------------
// Psi_ms: the luminance every order of scattering above the first adds, per
// unit of scattering coefficient and unit illuminance, by the cosine of the
// sun's zenith angle and the altitude.

public float2 multiscatter_uv(GpuAtmosphere a, float altitude, float mu_sun)
{
    return float2(mu_sun * 0.5f + 0.5f, clamp(altitude / thickness(a), 0.0f, 1.0f));
}

public float3 multiscatter_lut(GpuFrame frame, GpuAtmosphere a, float altitude, float mu_sun)
{
    float2 uv = multiscatter_uv(a, altitude, mu_sun);
    return g_textures[NonUniformResourceIndex(frame.atmosphere.z)]
        .SampleLevel(g_samplers[NonUniformResourceIndex(frame.atmosphere.w)], uv, 0.0f)
        .rgb;
}

// -- sky-view LUT ---------------------------------------------------------------
// The sky around the camera, for a camera inside the air: u is the azimuth
// from the sun's, 0 toward it and 1 away (the sky is symmetric about the
// sun's vertical plane), v the zenith angle, split at the horizon of the
// ground sphere with each half squared toward it, where the sky changes
// fastest. Texel centres map to the ends of both ranges (Hillaire's "sub
// UVs"), so the zenith, nadir and horizon are sampled exactly.

float to_sub_uv(float u, float resolution)
{
    return (u + 0.5f / resolution) * (resolution / (resolution + 1.0f));
}

float from_sub_uv(float u, float resolution)
{
    return (u - 0.5f / resolution) * (resolution / (resolution - 1.0f));
}

// The zenith angle of the camera's horizon on the ground sphere, and how far
// below the horizontal that horizon dips.
float2 horizon_angles(GpuAtmosphere a)
{
    float r         = bottom_radius(a) + camera_altitude(a);
    float to_ground = sqrt(max(squared_excess(camera_altitude(a) + kGroundDepth, ground_radius(a)), 0.0f));
    float dip       = acos(clamp(to_ground / r, -1.0f, 1.0f));
    return float2(kPi - dip, dip);
}

// The camera's local frame: up away from the centre, and the horizontal
// direction toward the sun.
public void sky_frame(GpuFrame frame, GpuAtmosphere a, out float3 up, out float3 toward_sun)
{
    up           = normalize(camera_position(a));
    float3 sun   = frame.sun_direction.xyz;
    float3 level = sun - up * dot(sun, up);
    toward_sun   = dot(level, level) > 1e-8f ? normalize(level)
                                             : normalize(cross(up, abs(up.x) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0)));
}

public void sky_view_parameters(GpuAtmosphere a, float2 uv, float2 size, out float view_zenith_cos,
                                out float light_view_cos)
{
    uv            = float2(from_sub_uv(uv.x, size.x), from_sub_uv(uv.y, size.y));
    float2 angles = horizon_angles(a);
    if (uv.y < 0.5f)
    {
        float c         = 1.0f - 2.0f * uv.y;
        view_zenith_cos = cos(angles.x * (1.0f - c * c));
    }
    else
    {
        float c         = uv.y * 2.0f - 1.0f;
        view_zenith_cos = cos(angles.x + angles.y * c * c);
    }
    light_view_cos = -(uv.x * uv.x * 2.0f - 1.0f);
}

public float2 sky_view_uv(GpuAtmosphere a, float2 size, float view_zenith_cos, float light_view_cos, bool below_horizon)
{
    float2 angles = horizon_angles(a);
    float  zenith = acos(clamp(view_zenith_cos, -1.0f, 1.0f));

    float2 uv;
    if (!below_horizon)
    {
        uv.y = (1.0f - sqrt(1.0f - saturate(zenith / angles.x))) * 0.5f;
    }
    else
    {
        uv.y = sqrt(saturate((zenith - angles.x) / angles.y)) * 0.5f + 0.5f;
    }
    uv.x = sqrt(saturate(-light_view_cos * 0.5f + 0.5f));
    return float2(to_sub_uv(uv.x, size.x), to_sub_uv(uv.y, size.y));
}

// -- sunlight -----------------------------------------------------------------

// Of the light arriving from direction `toward` (unit), the fraction left at
// `position` (from the centre, `altitude` above the radius): through the air
// between, and none if the body is in the way. A point above the atmosphere
// is lit through it only where its ray dips in.
public float3 transmittance_toward(GpuFrame frame, GpuAtmosphere a, float3 position, float altitude,
                                   float3 toward)
{
    float b = dot(position, toward);

    float2 ground = sphere_hits(b, squared_excess(altitude + kGroundDepth, ground_radius(a)));
    if (ground.x > 0.0f)
    {
        return 0.0f;
    }

    float r = bottom_radius(a) + altitude;
    if (altitude <= thickness(a))
    {
        return transmittance_lut(frame, a, altitude, b / r);
    }

    // Outside: from where the ray enters, if it does.
    float2 shell = sphere_hits(b, squared_excess(altitude - thickness(a), top_radius(a)));
    if (shell.x <= 0.0f)
    {
        return 1.0f;
    }
    float3 entry = position + shell.x * toward;
    return transmittance_lut(frame, a, thickness(a), dot(entry, toward) / top_radius(a));
}

// The sun's transmittance at the view-space point p.
public float3 sun_transmittance(GpuFrame frame, GpuAtmosphere a, float3 p)
{
    return transmittance_toward(frame, a, camera_position(a) + p, altitude_at(a, p),
                                frame.sun_direction.xyz);
}

// -- the view ray ---------------------------------------------------------------

public struct Segment
{
    public float start;
    public float end;
    public bool  ground;   // ends on the body, not in space
};

// Where the view-space ray from the camera along `direction` (unit) is in
// the air, cut at `limit`: start >= end when it never is.
public Segment view_segment(GpuAtmosphere a, float3 direction, float limit)
{
    float3 o = camera_position(a);
    float  b = dot(o, direction);

    Segment segment;
    segment.start  = 0.0f;
    segment.end    = -1.0f;
    segment.ground = false;

    float2 shell = sphere_hits(b, squared_excess(camera_altitude(a) - thickness(a), top_radius(a)));
    if (shell.y <= 0.0f)
    {
        return segment;
    }
    segment.start = max(shell.x, 0.0f);
    segment.end   = shell.y;

    float2 ground = sphere_hits(b, squared_excess(camera_altitude(a) + kGroundDepth, ground_radius(a)));
    if (ground.x > 0.0f && ground.x < segment.end)
    {
        segment.end    = ground.x;
        segment.ground = true;
    }
    if (limit < segment.end)
    {
        segment.end    = limit;
        segment.ground = false;
    }
    return segment;
}

public struct Scattered
{
    public float3 luminance;       // in-scattered toward the camera
    public float3 transmittance;   // of whatever lies behind
};

// Marches the view-space ray along `direction` from t = start to end in
// `steps`, in-scattering sunlight (single scattering by its phase, higher
// orders from the LUT) and attenuating. Each step integrates its own
// segment analytically, as Hillaire does, so few steps do not lose energy.
public Scattered march(GpuFrame frame, GpuAtmosphere a, float3 direction, float start, float end, uint steps)
{
    float3 sun       = frame.sun_direction.xyz;
    float3 light     = frame.sun_radiance.rgb;
    float  cos_theta = dot(direction, sun);
    float  rayleigh  = rayleigh_phase(cos_theta);
    float  mie       = mie_phase(a.mie_absorption.w, cos_theta);
    float3 o         = camera_position(a);

    Scattered result;
    result.luminance     = 0.0f;
    result.transmittance = 1.0f;

    float dt = (end - start) / float(steps);
    for (uint step = 0; step < steps; ++step)
    {
        float  t        = start + (float(step) + 0.3f) * dt;
        float3 p        = t * direction;
        float  altitude = altitude_at(a, p);
        float3 position = o + p;
        float  mu_sun   = dot(position, sun) / (bottom_radius(a) + altitude);

        Medium medium  = medium_at(a, altitude);
        float3 sunlit  = transmittance_toward(frame, a, position, altitude, sun);
        float3 higher  = multiscatter_lut(frame, a, altitude, mu_sun);
        float3 emitted = light * (sunlit * (medium.rayleigh * rayleigh + medium.mie * mie) +
                                  higher * medium.scattering);

        float3 extinction = max(medium.extinction, 1e-7f);
        float3 through    = exp(-extinction * dt);
        result.luminance     += result.transmittance * (emitted - emitted * through) / extinction;
        result.transmittance *= through;
    }
    return result;
}

// Steps for a segment `length` km long: few for the metres to nearby
// geometry, more for a ray across the sky.
public uint steps_for(float length)
{
    return uint(clamp(4.0f + 28.0f * saturate(length / 100.0f), 4.0f, 32.0f));
}
