#pragma once

// Every renderer capacity and tuning knob, in one place so they can be edited
// and rebuilt while testing. Shaders read the counts they need from the Frame
// buffer at runtime, so nothing here is duplicated on the Slang side.

namespace encke::config
{
    // -- clusters ---------------------------------------------------------------
    // 16x9 tiles match a 16:9 screen. Logarithmic slices between the near and
    // far distances spend depth resolution close to the camera, where local
    // lights are. Past kClusterFar everything shares the last slice.
    inline constexpr u32 kClustersX           = 16;
    inline constexpr u32 kClustersY           = 9;
    inline constexpr u32 kClustersZ           = 24;
    inline constexpr u32 kClusterCount        = kClustersX * kClustersY * kClustersZ;
    inline constexpr u32 kMaxLightsPerCluster = 64;
    inline constexpr f32 kClusterNear         = 0.1f;
    inline constexpr f32 kClusterFar          = 400.0f;

    // -- scene capacity -----------------------------------------------------------
    // Per-frame upload buffers are sized from these; anything past them is
    // dropped from the frame, not an error.
    inline constexpr u32 kMaxObjects = 256;
    inline constexpr u32 kMaxLights  = 1024;

    // -- sun: cascaded shadow maps ------------------------------------------------
    // Splits blend logarithmic and uniform by kCascadeSplitLambda (1 is fully
    // logarithmic). Past kShadowDistance the sun is unshadowed; the last
    // kShadowFadeFraction of that range fades out so the edge does not show.
    inline constexpr u32 kCascadeCount       = 4;
    inline constexpr u32 kCascadeResolution  = 2048;
    inline constexpr f32 kShadowDistance     = 150.0f;
    inline constexpr f32 kCascadeSplitLambda = 0.75f;
    inline constexpr f32 kShadowFadeFraction = 0.1f;

    // How far past a cascade's bounding sphere, toward the sun, casters are
    // still rendered at their true depth. Anything nearer than that is clamped
    // to the near plane by depth clamp, which keeps it casting.
    inline constexpr f64 kCascadeCasterMargin = 200.0;

    // -- local shadows: spot lights -------------------------------------------------
    // At most this many shadow-casting spots are shadowed per frame, chosen
    // nearest the camera first among those in view and within their own
    // shadow range. The rest still light, unshadowed.
    inline constexpr u32 kMaxShadowedSpots     = 4;
    inline constexpr u32 kSpotShadowResolution = 1024;
    inline constexpr f64 kSpotShadowNear       = 0.05;

    // -- shadow bias ------------------------------------------------------------------
    // Rasterisation bias is negative because depth is reversed: pushing an
    // occluder away from the light makes its depth smaller. The normal offset
    // is in shadow-map texels, applied to the receiver along its normal.
    inline constexpr f32 kShadowBiasConstant = -2.0f;
    inline constexpr f32 kShadowBiasSlope    = -2.5f;
    inline constexpr f32 kShadowNormalOffset = 1.5f;

    inline constexpr u32 kShadowViewCount = kCascadeCount + kMaxShadowedSpots;
}
