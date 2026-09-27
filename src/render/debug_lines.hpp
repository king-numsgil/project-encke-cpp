#pragma once

namespace encke
{
    // Line segments to draw over the frame, in f64 world space: collision
    // shapes, the octree's chunks, anything that wants showing. Refilled each
    // frame by the app and handed to the renderer, which takes every end to
    // view space in f64 before narrowing, like everything else it draws.
    //
    // Colours are display sRGB bytes, red lowest (rgb()); they are not lit or
    // exposed.
    class DebugLines
    {
    public:
        struct Line
        {
            f64vec3 a{0.0};
            f64vec3 b{0.0};
            u32     colour = 0;
        };

        void clear() { lines_.clear(); }
        bool empty() const { return lines_.empty(); }
        span<Line const> lines() const { return lines_; }

        void line(f64vec3 const& a, f64vec3 const& b, u32 colour);

        // Its three edges; a shared edge is drawn once per triangle.
        void triangle(f64vec3 const& a, f64vec3 const& b, f64vec3 const& c, u32 colour);

        // An oriented box's twelve edges: its centre, rotation and half
        // extents along its own axes.
        void box(f64vec3 const& centre, f64quat const& rotation, f64vec3 const& half, u32 colour);

    private:
        vector<Line> lines_;
    };

    // sRGB colour, 0 to 1 per channel, packed as DebugLines takes it.
    u32 rgb(f32vec3 const& srgb);

    // The colour of a terrain LOD whose voxels are `voxel_size` metres, a
    // power of two: 2^n takes entry n mod 8 of a palette the wireframe
    // shares (shaders/debug_draw.slang), so boxes and wires agree.
    u32 lod_colour(f64 voxel_size);
}
