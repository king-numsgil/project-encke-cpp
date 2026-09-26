// A body's surface map: its albedo, normal and roughness from far off, over a
// cube around its centre. Mirrors terrain/surface_map.hpp, where the layout is
// described; change one, change the other.
//
// Face f looks along axis a = f / 2, positive for even f, with s and t along
// axes (a + 1) % 3 and (a + 2) % 3. Each face is a tile of `face` texels
// inside a border of `border`, three tiles across and two down.

module surface_map;

import lib.bindless;

public struct MapSample
{
    public float3 albedo;      // linear
    public float3 normal;      // body space, unit
    public float  roughness;
    public float  height;      // 0 to 1 across the map's height range
};

float3 axis_vector(uint a)
{
    return float3(a == 0u ? 1.0f : 0.0f, a == 1u ? 1.0f : 0.0f, a == 2u ? 1.0f : 0.0f);
}

// The map at body-space unit direction `d`, whose screen derivatives are
// `d_dx` and `d_dy`: its level of detail follows them, capped where the
// border runs out, so no mip reaches into another face's tile. The
// derivatives go through the chosen face's projection, which keeps them
// continuous across face edges, where the atlas coordinates jump.
public MapSample sample_surface_map(uint albedo, uint surface, SamplerState filter, uint face_size, uint border,
                                    float3 d, float3 d_dx, float3 d_dy)
{
    float3 size  = abs(d);
    uint   axis  = size.x >= size.y && size.x >= size.z ? 0u : (size.y >= size.z ? 1u : 2u);
    float  major = d[axis];
    uint   face  = 2u * axis + (major < 0.0f ? 1u : 0u);
    uint   u     = (axis + 1u) % 3u;
    uint   v     = (axis + 2u) % 3u;

    // st = (d[u], d[v]) / |d[axis]|, and its derivative by the quotient rule.
    float  inverse = 1.0f / abs(major);
    float  sign    = major < 0.0f ? -1.0f : 1.0f;
    float2 st      = float2(d[u], d[v]) * inverse;
    float2 st_dx   = (float2(d_dx[u], d_dx[v]) - st * sign * d_dx[axis]) * inverse;
    float2 st_dy   = (float2(d_dy[u], d_dy[v]) - st * sign * d_dy[axis]) * inverse;

    float  tile  = float(face_size + 2u * border);
    float2 atlas = float2(3.0f, 2.0f) * tile;
    float2 texel = float2(float(face % 3u), float(face / 3u)) * tile + float(border) +
                   (st * 0.5f + 0.5f) * float(face_size);
    float2 uv = texel / atlas;

    float half_face = 0.5f * float(face_size);
    float footprint = max(length(st_dx), length(st_dy)) * half_face;
    float lod       = clamp(log2(max(footprint, 1e-6f)), 0.0f, log2(float(max(border, 1u))));

    float3 colour = g_textures[NonUniformResourceIndex(albedo)].SampleLevel(filter, uv, lod).rgb;
    float4 packed = g_textures[NonUniformResourceIndex(surface)].SampleLevel(filter, uv, lod);

    // The normal in the face's tangent frame at d: along s laid into the
    // surface, then cross(d, that).
    float3 along_u = axis_vector(u);
    float3 along_s = normalize(along_u - d * dot(along_u, d));
    float3 along_t = cross(d, along_s);
    float2 slope   = packed.xy - 0.5f;

    MapSample result;
    result.albedo    = colour;
    result.normal    = normalize(slope.x * along_s + slope.y * along_t + sqrt(saturate(1.0f - dot(slope, slope))) * d);
    result.roughness = packed.z;
    result.height    = packed.w;
    return result;
}
