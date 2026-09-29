#pragma once

#include "assets/handle.hpp"

namespace encke
{
    // A part's triangles kept on the CPU after its mesh has gone to the GPU,
    // in the mesh's own space, for a collider built from it.
    struct CollisionMesh
    {
        vector<f32vec3> positions;
        vector<u32>     indices;
    };

    // A loaded model: its node tree, and each node's mesh as parts that are
    // one mesh asset and one material asset each. The AssetManager makes it
    // from a glTF; Scene::spawn instances it.
    struct ModelPart
    {
        MeshHandle     mesh;
        MaterialHandle material;

        // glTF factors, copied onto each Renderable made from the part.
        f32vec3 albedo{1.0f};
        f32     roughness = 1.0f;
        f32     metallic  = 1.0f;
        f32vec3 emissive{0.0f};   // relative; the scene scales it to luminance

        std::shared_ptr<CollisionMesh const> collision;
    };

    // One glTF mesh: a part per primitive, each drawn by its own entity.
    struct ModelMesh
    {
        vector<ModelPart> parts;
    };

    // The file's node hierarchy, parents before children, already in
    // Transform's terms: scale is the node's own and not inherited, so a
    // position is an unscaled offset in the parent's rotated frame. glTF's
    // inherited scale is folded in by load_gltf.
    struct ModelNode
    {
        string        name;
        optional<u32> parent;   // into Model::nodes
        f64vec3       position{0.0};
        f64quat       rotation{1.0, 0.0, 0.0, 0.0};
        f64vec3       scale{1.0};
        optional<u32> mesh;     // into Model::meshes

        // A door, hatch or other part that turns or slides: the node has a
        // `joint` in its glTF extras. It is modelled in one pose, so a static
        // collider would hold it there.
        bool moving = false;
    };

    struct Model
    {
        vector<ModelMesh> meshes;
        vector<ModelNode> nodes;
        f64vec3           min{0.0};   // model space, over every instanced part
        f64vec3           max{0.0};
    };
}
