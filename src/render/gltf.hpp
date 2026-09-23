#pragma once

#include "render/mesh.hpp"
#include "render/pixels.hpp"

#include <memory>

namespace encke
{
    // Where a loaded glTF's encoded images are: bytes copied out of the
    // file, or paths beside it. Held by the model so images decode later,
    // on any thread, after load_gltf's parsed asset is gone.
    struct GltfImages;

    // glTF keeps occlusion apart from metallic-roughness, though exporters
    // often point both at one image. The renderer wants them as one ORM
    // texture, so this names what is packed into it.
    struct GltfOrm
    {
        optional<u32> occlusion;     // image index; R
        optional<u32> metal_rough;   // image index; G roughness, B metalness
        f32           occlusion_strength = 1.0f;

        bool any() const { return occlusion.has_value() || metal_rough.has_value(); }
    };

    // A glTF material: factors, which multiply the maps, and which images
    // the maps are. Nothing is decoded here; see decode_gltf_image.
    struct GltfMaterial
    {
        f32vec3 base_colour{1.0f};   // linear
        f32     roughness = 1.0f;
        f32     metallic  = 1.0f;
        f32vec3 emissive{0.0f};      // linear, emissiveFactor times KHR_materials_emissive_strength

        optional<u32> albedo;        // image indices
        optional<u32> normal;
        optional<u32> emission;
        GltfOrm       orm;
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

        std::shared_ptr<GltfImages const> images;
    };

    // Reads a .gltf or .glb: the default scene's node hierarchy and the
    // triangle primitives of the meshes it instances. Tangents are generated
    // where the file has none. Images may be embedded or external JPG/PNG
    // files beside it; they are only located here, and decoded later with
    // decode_gltf_image and decode_gltf_orm. Unsupported features (texture coordinate sets
    // other than 0, alpha blending and masking, double-sided materials,
    // mirroring node transforms, other primitive modes) are logged and
    // ignored, not fatal. So is shear, which is drawn approximately.
    bool load_gltf(string const& path, GltfModel& model);

    // Decodes image `index` of a loaded model. Safe on any thread. Logs and
    // returns false on failure.
    bool decode_gltf_image(GltfImages const& images, u32 index, Pixels& pixels);

    // Decodes and packs an ORM texture: R occlusion, at `occlusion_strength`
    // as glTF defines it, G and B the metallic-roughness image's. A missing
    // part reads 1, leaving its factor alone; an image that fails to decode
    // is logged and treated as missing. False if nothing is left to pack.
    // Safe on any thread.
    bool decode_gltf_orm(GltfImages const& images, GltfOrm const& orm, Pixels& pixels);
}
