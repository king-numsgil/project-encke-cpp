// Screen-space coordinate conventions.
//
// The viewport has negative height (see flipped_viewport in vulkan/context),
// so NDC has +Y UP while SV_Position and any render target are addressed from
// the TOP-LEFT with +Y DOWN. Those two disagree, and every conversion between
// them belongs here rather than open-coded per shader -- an inverted screen
// space effect looks plausible enough to ship.
//
// Geometry is wound counter-clockwise in NDC (+Y up), matching glTF. The
// negative viewport cancels against Vulkan's y-down framebuffer, so the
// pipeline uses frontFace = COUNTER_CLOCKWISE.

module screen;

// Pixel coordinate from SV_Position.xy -> NDC in [-1, 1], +Y up.
public float2 fragment_to_ndc(float2 fragment, float2 viewport_size)
{
    float2 uv = fragment / viewport_size;
    return float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
}

// NDC (+Y up) -> UV for sampling a full-screen render target (top-left origin).
public float2 ndc_to_uv(float2 ndc)
{
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

// UV (top-left origin) -> NDC (+Y up).
public float2 uv_to_ndc(float2 uv)
{
    return float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
}

// Pixel coordinate -> UV. No Y flip: both are top-left origin.
public float2 fragment_to_uv(float2 fragment, float2 viewport_size)
{
    return fragment / viewport_size;
}
