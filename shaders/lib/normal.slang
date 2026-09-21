// Octahedral normal encoding, for the RG16_UNORM G-buffer channel.
//
// Projects the unit sphere onto an octahedron and unfolds it into a square,
// so two channels carry a full normal with near-uniform precision. Output is
// remapped to [0,1] for a UNORM target.

module normal;

float2 sign_not_zero(float2 v)
{
    return float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}

public float2 encode_normal(float3 n)
{
    float2 p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0f)
    {
        p = (1.0f - abs(p.yx)) * sign_not_zero(p);
    }
    return p * 0.5f + 0.5f;
}

public float3 decode_normal(float2 encoded)
{
    float2 f = encoded * 2.0f - 1.0f;
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    if (n.z < 0.0f)
    {
        n.xy = (1.0f - abs(n.yx)) * sign_not_zero(n.xy);
    }
    return normalize(n);
}
