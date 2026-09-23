#pragma once

#include "render/mesh.hpp"
#include "render/pixels.hpp"

namespace encke
{
    // A glTF material, decoded and packed for upload. Factors are glTF's and
    // multiply the maps. A missing map is nullopt; the renderer substitutes
    // white for albedo and ORM, which leaves the factor alone, and skips the
    // normal and emissive samples.
    struct GltfMaterial
    {
        f32vec3 base_colour{1.0f};   // linear
        f32     roughness = 1.0f;
        f32     metallic  = 1.0f;
        f32vec3 emissive{0.0f};      // linear, emissiveFactor times KHR_materials_emissive_strength

        optional<Pixels> albedo;     // sRGB
        optional<Pixels> normal;     // tangent space, glTF convention, as encke's
        optional<Pixels> orm;        // R occlusion, G roughness, B metalness
        optional<Pixels> emission;   // sRGB
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
    // or external JPG/PNG files beside it. Unsupported features (texture
    // coordinate sets other than 0, alpha blending and masking, double-sided
    // materials, other primitive modes) are logged and ignored, not fatal.
    bool load_gltf(string const& path, GltfModel& model);
}
