#pragma once

namespace encke
{
    // A loaded model as the renderer holds it: GPU meshes and materials by
    // id, plus what the scene needs to place it. Renderer::add_model makes
    // one; Scene::add_model instances it. Ids index the renderer's mesh and
    // material lists, which start with MeshKind and MaterialKind in order.
    struct ModelPart
    {
        u32 mesh     = 0;
        u32 material = 0;

        // glTF factors, copied onto each Renderable made from the part.
        f32vec3 albedo{1.0f};
        f32     roughness = 1.0f;
        f32     metallic  = 1.0f;
        f32vec3 emissive{0.0f};   // relative; the scene scales it to luminance
    };

    // One mesh, uploaded once and drawn once per node that uses it.
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
    };

    struct Model
    {
        vector<ModelMesh> meshes;
        vector<ModelNode> nodes;
        f64vec3           min{0.0};   // model space, over every instanced part
        f64vec3           max{0.0};
    };
}
