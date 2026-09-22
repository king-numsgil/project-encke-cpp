// Metallic-roughness BRDF: GGX distribution, height-correlated Smith
// visibility, Schlick Fresnel, Lambert diffuse. The standard real-time model;
// nothing clever yet.

module pbr;

static const float kPi = 3.14159265358979f;

public struct Surface
{
    public float3 albedo;
    public float3 normal;      // view space, unit
    public float3 to_eye;      // view space, unit
    public float  roughness;   // perceptual
    public float  metallic;
    public float  ao;
};

float d_ggx(float n_dot_h, float alpha)
{
    float a2 = alpha * alpha;
    float d  = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
    return a2 / max(kPi * d * d, 1e-7f);
}

// Height-correlated Smith, already divided by 4 * NdotL * NdotV.
float v_smith(float n_dot_v, float n_dot_l, float alpha)
{
    float a2 = alpha * alpha;
    float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0f - a2) + a2);
    float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0f - a2) + a2);
    return 0.5f / max(gv + gl, 1e-7f);
}

float3 f_schlick(float v_dot_h, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - v_dot_h, 5.0f);
}

// Outgoing radiance from one light arriving from `to_light` with `radiance`
// already attenuated.
public float3 shade(Surface s, float3 to_light, float3 radiance)
{
    float3 h       = normalize(to_light + s.to_eye);
    float  n_dot_l = saturate(dot(s.normal, to_light));
    float  n_dot_v = max(dot(s.normal, s.to_eye), 1e-4f);
    float  n_dot_h = saturate(dot(s.normal, h));
    float  v_dot_h = saturate(dot(s.to_eye, h));

    if (n_dot_l <= 0.0f)
    {
        return 0.0f;
    }

    // Perceptual roughness squared; clamped so a perfect mirror does not
    // collapse the specular lobe to a single texel of NaN.
    float alpha = max(s.roughness * s.roughness, 0.002f);

    float3 f0       = lerp(float3(0.04f), s.albedo, s.metallic);
    float3 fresnel  = f_schlick(v_dot_h, f0);
    float3 specular = d_ggx(n_dot_h, alpha) * v_smith(n_dot_v, n_dot_l, alpha) * fresnel;
    float3 diffuse  = (1.0f - fresnel) * (1.0f - s.metallic) * s.albedo / kPi;

    return (diffuse + specular) * radiance * n_dot_l;
}

// Inverse-square with a smooth window to zero at `radius`, so a light's
// influence is finite and can be bounded by a cluster sphere test. Karis 2013.
public float point_attenuation(float distance, float radius)
{
    float ratio  = distance / radius;
    float window = saturate(1.0f - ratio * ratio * ratio * ratio);
    return window * window / max(distance * distance, 1e-4f);
}

// Spot cone falloff: 1 inside the inner half-angle, 0 outside the outer, a
// squared ramp between. `from_light` is the unit direction from the light to
// the surface. A point light passes cos_outer < -1, which is never reached.
public float spot_cone(float3 from_light, float3 axis, float cos_outer, float cos_inner)
{
    float ramp = saturate((dot(from_light, axis) - cos_outer) / max(cos_inner - cos_outer, 1e-4f));
    return ramp * ramp;
}
