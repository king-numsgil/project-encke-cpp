#pragma once

namespace encke::terrain
{
    // Edits to a body's field, applied where the field is composed, after the
    // macro and detail layers, so every consumer sees them: chunks at every
    // LOD, point queries, collision and the ground probe. Each is a pure
    // function of a sample's f64 position and its unedited value, which keeps
    // chunk samples bit-exact across chunks.
    //
    // An edit steepens the field where it blends, and the octree's cull and
    // the probe's march trust the field's gradient to stay under 2; see
    // Flatten for what that asks of it.

    // A level floor cut and filled into the ground: within `radius` of the
    // axis through `centre` along `up`, the field is the height above the
    // plane through `centre`, so the surface is that plane whatever the
    // terrain did there; over the next `blend` metres out it eases back to
    // the terrain. It holds `reach` metres above and below the plane and
    // eases out over `blend` past that, so the column stops short of the far
    // side of the body.
    //
    // Blending mixes two heights, so it adds up to 1.5 times the largest cut
    // or fill over `blend` to the field's slope, once radially and once
    // vertically: keep `blend` several times the height difference between
    // the plane and the ground around it.
    struct Flatten
    {
        f64vec3 centre{0.0};           // body-relative, on the floor
        f64vec3 up{0.0, 1.0, 0.0};     // unit
        f64     radius = 0.0;
        f64     blend  = 1.0;
        f64     reach  = 0.0;

        // Past this from `centre` nothing changes.
        f64 bound() const;

        f64 apply(f64vec3 const& point, f64 value) const;
    };

    struct Modifiers
    {
        vector<Flatten> flattens;

        bool empty() const { return flattens.empty(); }
    };
}
