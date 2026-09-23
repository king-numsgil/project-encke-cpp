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

    // One triangle list with one material, in its mesh's own space: no node
    // transform is applied, so every node using the mesh shares it. UVs are
    // glTF's texture coordinates, 0..1 over the atlas, not metres; the
    // renderer knows not to stretch them.
    struct GltfPrimitive
    {
        vector<Vertex> vertices;
        vector<u32>    indices;
        u32            material = 0;   // into GltfModel::materials
        f32vec3        min{0.0f};
        f32vec3        max{0.0f};
    };

    // A glTF mesh: its triangle primitives, loaded once however many nodes
    // instance it. May be empty if every primitive was unsupported.
    struct GltfMesh
    {
        string                name;
        vector<GltfPrimitive> primitives;
    };

    // A node of the default scene, parents before children, converted to
    // Transform's convention: scale is the node's own and not inherited. glTF
    // inherits it, so each node's model-space transform is composed the glTF
    // way, split into position, rotation and scale, and the position and
    // rotation made relative to the parent's again. Exact unless a parent
    // scales unevenly under a rotated child, which is shear; that is logged.
    struct GltfNode
    {
        string        name;
        optional<u32> parent;   // into GltfModel::nodes; none for a scene root
        f32vec3       position{0.0f};   // unscaled, in the parent's rotated frame
        f32quat       rotation{1.0f, 0.0f, 0.0f, 0.0f};
        f32vec3       scale{1.0f};      // own size only
        optional<u32> mesh;     // into GltfModel::meshes
    };

    struct GltfModel
    {
        vector<GltfMesh>     meshes;
        vector<GltfNode>     nodes;
        vector<GltfMaterial> materials;
        f32vec3              min{0.0f};   // model space, over every instanced primitive
        f32vec3              max{0.0f};
    };

    // Reads a .gltf or .glb: the default scene's node hierarchy and the
    // triangle primitives of the meshes it instances. Tangents are generated
    // where the file has none. Images may be embedded or external JPG/PNG
    // files beside it; they are only located here, and decoded later by each
    // material's `decode`. Unsupported features (texture coordinate sets
    // other than 0, alpha blending and masking, double-sided materials,
    // mirroring node transforms, other primitive modes) are logged and
    // ignored, not fatal. So is shear, which is drawn approximately.
    bool load_gltf(string const& path, GltfModel& model);
}
