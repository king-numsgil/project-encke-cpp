// Contrast Adaptive Sharpening, ported to Slang from AMD FidelityFX CAS
// (ffx_cas.h), its non-scaling filter, per channel.
//
// Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// A cross-shaped sharpening kernel whose strength follows local contrast:
// full where the neighbourhood is flat, backed off where it already spans
// most of the range, so hard edges do not ring. The result stays within
// the neighbourhood's range. Input must be in [0, 1]: display values, after
// the tonemap curve.

module cas;

// The 3x3 neighbourhood, row by row from the top left; e is the centre.
//
//   a b c
//   d e f
//   g h i
//
// `sharpness` from 0, least, to 1, most.
public float3 cas(float3 a, float3 b, float3 c, float3 d, float3 e, float3 f, float3 g, float3 h, float3 i,
                  float sharpness)
{
    // Soft minimum and maximum: the cross's plus the whole square's, which
    // doubles the range to [0, 2] and rounds off single-texel outliers.
    float3 cross_min = min(min(min(d, e), min(f, b)), h);
    float3 cross_max = max(max(max(d, e), max(f, b)), h);
    float3 low       = cross_min + min(cross_min, min(min(a, c), min(g, i)));
    float3 high      = cross_max + max(cross_max, max(max(a, c), max(g, i)));

    // Headroom to whichever limit is nearer, over the maximum: small where
    // the neighbourhood already has contrast. Shaped by a square root.
    float3 amount = sqrt(saturate(min(low, 2.0f - high) / max(high, 1e-5f)));

    // The negative lobe's weight: -1/8 at sharpness 0, -1/5 at 1.
    float  peak   = -1.0f / lerp(8.0f, 5.0f, saturate(sharpness));
    float3 weight = amount * peak;

    return saturate(((b + d + f + h) * weight + e) / (1.0f + 4.0f * weight));
}
