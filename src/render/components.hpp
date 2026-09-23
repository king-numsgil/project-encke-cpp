#pragma once

#include "render/material.hpp"
#include "render/mesh.hpp"

namespace encke
{
    // What the renderer reads off an entity, beside its WorldTransform. The
    // renderer copies these out once a frame (render/extract) and never
    // touches the registry otherwise.

    // Draws one mesh with one material at the entity's world transform,
    // sized by its scale.
    struct Renderable
    {
        // Renderer ids. A MeshKind or MaterialKind converts to its own id;
        // loaded models get ids past them. Material 0 is untextured.
        u32 mesh     = static_cast<u32>(MeshKind::Cube);
        u32 material = static_cast<u32>(MaterialKind::None);

        // With a material these are glTF-style factors on its maps; without,
        // they are the whole material.
        f32vec3 albedo{1.0f};          // linear
        f32     roughness = 0.5f;
        f32vec3 emissive{0.0f};        // linear, HDR
        f32     metallic  = 0.0f;
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
