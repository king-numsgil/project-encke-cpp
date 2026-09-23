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

        // glTF factors, copied onto each SceneObject made from the part.
        f32vec3 albedo{1.0f};
        f32     roughness = 1.0f;
        f32     metallic  = 1.0f;
        f32vec3 emissive{0.0f};   // relative; the scene scales it to luminance

        // Model space, for shadow caster culling.
        f64vec3 bounds_centre{0.0};
        f64     bounds_radius = 0.0;
    };

    struct Model
    {
        vector<ModelPart> parts;
        f64vec3           min{0.0};   // model space, over every part
        f64vec3           max{0.0};
    };
}
