#pragma once

#include "assets/handle.hpp"

namespace encke
{
    // What the renderer reads off an entity, beside its WorldTransform. The
    // renderer copies these out once a frame (render/extract) and never
    // touches the registry otherwise.

    // Draws one mesh with one material at the entity's world transform,
    // sized by its scale.
    struct Renderable
    {
        // Drawn once the mesh is resident. No material, or one whose maps
        // have not landed, draws with the flat factors below.
        MeshHandle     mesh;
        MaterialHandle material;

        // With a material these are glTF-style factors on its maps; without,
        // they are the whole material.
        f32vec3 albedo{1.0f};          // linear
        f32     roughness = 0.5f;
        f32vec3 emissive{0.0f};        // linear, HDR
        f32     metallic  = 0.0f;
    };

    // Beside a Renderable whose mesh carries morph targets: how far toward
    // them each vertex is drawn. A vertex moves from its own position at
    // `start` metres from the camera to its target at `end`, so a terrain
    // chunk has become its parent by the distance the parent replaces it.
    //
    // The masks say which of the 26 chunks around this one are drawn one LOD
    // coarser or finer, bit (dx + 1) + 3 (dy + 1) + 9 (dz + 1) for the chunk
    // at offset (dx, dy, dz). Vertices near a coarser neighbour are pulled
    // fully to their targets, the neighbour's own vertices, and near a finer
    // one held at their own, which the neighbour's targets are; within a
    // voxel of the face, where chunks share vertices, exactly, so the seam
    // closes whatever the distances say. Positions near a face are measured
    // in mesh space, where the chunk spans 0 to `extent` on each axis.
    struct Geomorph
    {
        f32 start  = 0.0f;
        f32 end    = 0.0f;
        f32 extent = 0.0f;   // metres
        f32 voxel  = 0.0f;   // metres
        u32 coarser = 0;
        u32 finer   = 0;

        // The chunk's corner, body-relative, less whole texture periods:
        // mesh space plus this is continuous across chunks and repeats
        // every material's tile, for projecting textures along the mesh
        // axes. Small, since it is taken in f64.
        f32vec3 period_offset{0.0f};

        // The cube face the chunk's centre points through, 0 to 2 for x, y
        // and z: which two coordinates its UVs are, when the vertex shader
        // takes them from the morphed position.
        u32 face = 1;
    };

    // What terrain chunks are drawn with: gravel, rock, grass, snow and sand,
    // blended by each vertex's weights (TerrainVertex::materials), each with
    // a flat linear colour drawn until its maps have landed. Every tile must
    // divide the period terrain UVs are offset by, or chunks' textures would
    // not meet.
    //
    // `roughness` is each material's floor: its map's roughness is remapped
    // into [floor, 1], which keeps the map's variation and loses the gloss
    // photo-scanned ground reads as when it lies under a low sun.
    struct TerrainPalette
    {
        array<MaterialHandle, 5> materials;
        array<f32vec3, 5>        albedo;
        array<f32, 5>            roughness;
    };

    // A point light at the entity's world position, or a spot facing the
    // entity's -Z when cos_outer is above -1. Only spots cast shadows;
    // casts_shadow on a point light is ignored. The entity's scale is not
    // read.
    struct Light
    {
        f32vec3 colour{1.0f};          // linear
        f32     intensity = 1.0f;      // candela
        f32     radius    = 5.0f;      // metres; influence cut to zero here

        // Cone half-angles as cosines. The defaults make a point light.
        f32 cos_inner = -1.0f;
        f32 cos_outer = -2.0f;

        // Shadowed only while the camera is within shadow_range of the light,
        // and only if it wins one of the config::kMaxShadowedSpots slots.
        bool casts_shadow = false;
        f64  shadow_range = 40.0;
    };
}
