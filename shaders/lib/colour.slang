// Colour space helpers.
//
// Shader output is LINEAR. The swapchain is an _SRGB format, so the hardware
// encodes on write and a value written straight out appears considerably
// lighter than the number suggests. Author in sRGB and convert here, rather
// than hand-computing linear constants and leaving the intent unreadable.

module colour;

// sRGB [0,1] -> linear. The piecewise curve, not the 2.2 approximation.
public float3 srgb_to_linear(float3 srgb)
{
    float3 low  = srgb / 12.92f;
    float3 high = pow((srgb + 0.055f) / 1.055f, 2.4f);
    return lerp(low, high, step(0.04045f, srgb));
}

// linear -> sRGB [0,1]. For writing to a UNORM target, or for debug readback.
public float3 linear_to_srgb(float3 linear)
{
    float3 low  = linear * 12.92f;
    float3 high = 1.055f * pow(linear, 1.0f / 2.4f) - 0.055f;
    return lerp(low, high, step(0.0031308f, linear));
}

// Rec. 709 luma, for tonemapping and debug views.
public float luminance(float3 linear)
{
    return dot(linear, float3(0.2126f, 0.7152f, 0.0722f));
}
