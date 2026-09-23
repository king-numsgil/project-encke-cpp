#pragma once

#include "render/material.hpp"
#include "render/mesh.hpp"

namespace encke
{
    // A glTF material. Factors are glTF's and multiply the maps.
    //
    // The maps are not decoded here: `decode` does that, reading the model's
    // images from bytes and paths it holds itself, so it can run on the
    // material loader's worker after load_gltf has returned. It packs
    // occlusion and metallic-roughness into one ORM map and leaves absent
    // maps nullopt. Null when the material has no maps at all.
    struct GltfMaterial
    {
        f32vec3 base_colour{1.0f};   // linear
        f32     roughness = 1.0f;
        f32     metallic  = 1.0f;
        f32vec3 emissive{0.0f};      // linear, emissiveFactor times KHR_materials_emissive_strength

        // The emissive factor is meant to be multiplied by a map; drawn
        // without it before the map lands, the whole surface would glow.
        bool has_emission_map = false;

        function<bool(MaterialMaps& maps)> decode;
    };

    // One triangle list with one material, in the model's own space: node
    // transforms are baked in. UVs are glTF's texture coordinates, 0..1 over
    // the atlas, not metres; the renderer knows not to stretch them.
    struct GltfPrimitive
    {
        vector<Vertex> vertices;
        vector<u32>    indices;
        u32            material = 0;   // into GltfModel::materials
        f32vec3        min{0.0f};
        f32vec3        max{0.0f};
    };

    struct GltfModel
    {
        vector<GltfPrimitive> primitives;
        vector<GltfMaterial>  materials;
        f32vec3               min{0.0f};   // over every primitive
        f32vec3               max{0.0f};
    };

    // Reads a .gltf or .glb, the default scene's triangle primitives only.
    // Tangents are generated where the file has none. Images may be embedded
    // or external JPG/PNG files beside it; they are only located here, and
    // decoded later by each material's `decode`. Unsupported features (texture
    // coordinate sets other than 0, alpha blending and masking, double-sided
    // materials, other primitive modes) are logged and ignored, not fatal.
    bool load_gltf(string const& path, GltfModel& model);
}
