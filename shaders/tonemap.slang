// HDR -> display. A full-screen triangle reads the accumulation buffer,
// applies exposure and a display curve, and writes linear display values into
// the _SRGB swapchain, which encodes on write.
//
// push.tonemap picks the curve, numbered as Tonemap in render/renderer.hpp.
// Every curve takes linear Rec.709 and returns linear Rec.709.

import lib.bindless;
import lib.gpu_types;

struct VertexOut
{
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOut vertex_main(uint index : SV_VertexID)
{
    // One triangle covering the screen; the overhang is clipped.
    float2 corner = float2(float((index << 1u) & 2u), float(index & 2u));

    VertexOut output;
    output.position = float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

// Narkowicz's fit of the ACES RRT+ODT. Per channel, so a bright saturated
// colour runs each channel up the same curve and lands near white.
float3 aces(float3 x)
{
    return saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
}

// AgX, Sobotka's, in the minimal form with a polynomial fit of the default
// contrast curve (Wrensch). The inset matrix pulls colours toward the
// achromatic axis before the log curve, so they desaturate gradually with
// brightness instead of clipping per channel. The matrices are written row by
// row, so mul(v, M) is the reference's column-major M * v.
float3 agx(float3 colour)
{
    static const float3x3 kInset = float3x3(
        0.842479062253094f,  0.0423282422610123f, 0.0423756549057051f,
        0.0784335999999992f, 0.878468636469772f,  0.0784336f,
        0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
    static const float3x3 kOutset = float3x3(
        1.19687900512017f,   -0.0528968517574562f, -0.0529716355144438f,
        -0.0980208811401368f, 1.15190312990417f,   -0.0980434501171241f,
        -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f);

    // log2 of scene linear, 0.18 at about 0.6 of the range.
    const float kMinEv = -12.47393f;
    const float kMaxEv = 4.026069f;

    float3 x = mul(max(colour, 0.0f), kInset);
    x = clamp(log2(max(x, 1.0e-10f)), kMinEv, kMaxEv);
    x = (x - kMinEv) / (kMaxEv - kMinEv);

    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    x = 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 +
        0.1191f * x - 0.00232f;

    // The curve's output is display-encoded for a 2.2 power display.
    x = mul(x, kOutset);
    return pow(saturate(x), 2.2f);
}

// Khronos PBR Neutral: linear below 0.76, so base colours survive to the
// screen unchanged, then compresses the peak channel and blends toward white
// only as much as the compression demands, which keeps hue.
float3 pbr_neutral(float3 colour)
{
    const float kStartCompression = 0.8f - 0.04f;
    const float kDesaturation     = 0.15f;

    float x      = min(colour.r, min(colour.g, colour.b));
    float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    colour -= offset;

    float peak = max(colour.r, max(colour.g, colour.b));
    if (peak < kStartCompression)
    {
        return colour;
    }

    const float d        = 1.0f - kStartCompression;
    float       new_peak = 1.0f - d * d / (peak + d - kStartCompression);
    colour *= new_peak / peak;

    float g = 1.0f - 1.0f / (kDesaturation * (peak - new_peak) + 1.0f);
    return lerp(colour, float3(new_peak), g);
}

[shader("fragment")]
float4 fragment_main(VertexOut input) : SV_Target
{
    // SV_Position.xy is the pixel centre in framebuffer coordinates, which
    // are top-left like the HDR target's -- no conversion.
    int3   texel = int3(int2(input.position.xy), 0);
    float3 hdr   = g_textures[NonUniformResourceIndex(push.hdr_sampled)].Load(texel).rgb;

    // Auto-exposure leaves an adapted EV100 in a 1x1 image; otherwise the
    // CPU pushed the scene's fixed exposure.
    float exposure = push.exposure;
    if (push.exposure_image != 0xFFFFFFFFu)
    {
        float ev100 = g_textures[NonUniformResourceIndex(push.exposure_image)].Load(int3(0, 0, 0)).r;
        exposure    = 1.0f / (1.2f * exp2(ev100));
    }

    float3 exposed = hdr * exposure;
    float3 display;
    switch (push.tonemap)
    {
    case 1:  display = agx(exposed); break;
    case 2:  display = pbr_neutral(exposed); break;
    default: display = aces(exposed); break;
    }
    return float4(display, 1.0f);
}
