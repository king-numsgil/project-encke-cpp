#pragma once

#include "assets/handle.hpp"
#include "assets/model.hpp"
#include "assets/worker.hpp"
#include "render/mesh.hpp"
#include "render/pixels.hpp"

#include <unordered_map>

namespace encke
{
    struct GltfModel;

    // Loading and Failed are the manager's. Once Ready the CPU data waits in
    // a queue for the renderer, which takes it, uploads it and frees it; on
    // the GPU side, residency is the renderer's to track.
    enum class AssetState : u8
    {
        Loading,
        Ready,
        Failed,
    };

    struct MeshData
    {
        vector<Vertex> vertices;
        vector<u32>    indices;

        // Empty, or one per vertex: a geomorphing mesh's targets.
        vector<MorphTarget> morphs;
    };

    // What stays known of a mesh after its data has gone to the GPU.
    struct MeshAsset
    {
        string     name;
        u32        generation = 0;
        AssetState state      = AssetState::Loading;

        // Mesh space, once Ready: for culling now, and whatever else needs
        // a size without the vertices.
        f32vec3 min{0.0f};
        f32vec3 max{0.0f};
    };

    // How a texture's texels are read: colour is sRGB-encoded, and the
    // sampler decodes it; data (normals, ORM) is used as it is.
    enum class TextureEncoding : u8
    {
        Srgb,
        Linear,
    };

    // What stays known of a texture after its texels have gone to the GPU.
    struct TextureAsset
    {
        string          name;
        u32             generation = 0;
        AssetState      state      = AssetState::Loading;
        TextureEncoding encoding   = TextureEncoding::Srgb;
    };

    // A material is what it samples and how; it has no data of its own, so
    // it is Ready as soon as it exists. The factors are not here: they are
    // per object, on its Renderable.
    //
    // A texture that is absent or Failed is left out: white for albedo and
    // ORM, skipped for normal and emission. The renderer draws the material
    // untextured until every texture it names has either landed or failed.
    struct MaterialAsset
    {
        string     name;
        u32        generation = 0;
        AssetState state      = AssetState::Ready;

        TextureHandle albedo;
        TextureHandle normal;
        TextureHandle orm;
        TextureHandle emission;

        // A tiling material repeats every `tile` metres, across and down, at
        // the object's scale; a non-tiling one is a glTF atlas whose UVs are
        // used as they are.
        bool    tiling = true;
        f32vec2 tile{1.0f};
    };

    // A model file. Its meshes, textures and materials are assets of their
    // own, registered when it is Ready; `model` holds the node tree that
    // refers to them.
    struct ModelAsset
    {
        string          name;
        u32             generation = 0;
        AssetState      state      = AssetState::Loading;
        optional<Model> model;   // once Ready
    };

    // Owns every asset's handle and what the CPU knows of it. Loads decode
    // on the one AssetWorker; their results are applied on the main thread
    // in update(), which is the only thread that may call anything here.
    //
    // Everything that reads a file is deduplicated: asking again for the
    // same ambientCG set, model, or glTF image (by file and index) returns
    // the first handle, and decodes nothing.
    //
    // Nothing here touches Vulkan: the renderer takes Ready data with
    // take_ready_*() and keeps its own GPU state by handle. Meshes can be
    // released; textures, materials and models are never freed yet.
    class AssetManager
    {
    public:
        struct ReadyMesh
        {
            MeshHandle handle;
            MeshData   data;
        };

        struct ReadyTexture
        {
            TextureHandle handle;
            Pixels        pixels;
        };

        AssetManager() = default;
        ~AssetManager();

        AssetManager(AssetManager const&)            = delete;
        AssetManager& operator=(AssetManager const&) = delete;
        AssetManager(AssetManager&&)                 = delete;
        AssetManager& operator=(AssetManager&&)      = delete;

        // Starts and stops the worker. Loads may be requested before start.
        void start();
        void stop();

        // A mesh built in code; Ready at once. Reuses released slots.
        MeshHandle add_mesh(string name, MeshData data);

        // Frees a mesh: its handle stops resolving at once, and the renderer
        // frees its GPU copy once no frame in flight can draw it. Whatever
        // draws it must stop in the same frame. Stale handles are ignored.
        void release_mesh(MeshHandle handle);

        // An ambientCG set from assets/textures, tiling every `tile` metres:
        // its colour, normal and packed ORM textures decode on the worker.
        // Asking for a set twice returns the first handle, whatever tile the
        // second asks for.
        MaterialHandle load_ambientcg(string const& set, f32vec2 tile);

        // A glTF, parsed and converted on the worker. When it is Ready its
        // meshes are Ready too and its textures are decoding. Failed, logged,
        // if the file does not load.
        ModelHandle load_model(string const& path);

        // Applies whatever the worker has finished. Once a frame.
        void update();

        // Null for no asset, or a handle whose slot has moved on.
        MeshAsset const*     mesh(MeshHandle handle) const;
        TextureAsset const*  texture(TextureHandle handle) const;
        MaterialAsset const* material(MaterialHandle handle) const;
        ModelAsset const*    model(ModelHandle handle) const;

        // The renderer's: everything that became Ready since the last call,
        // with its data moved out. What is taken is gone from here.
        vector<ReadyMesh>    take_ready_meshes();
        vector<ReadyTexture> take_ready_textures();

        // The renderer's: meshes released since the last call whose data it
        // had already taken, so it holds their GPU copies.
        vector<MeshHandle> take_released_meshes();

        // Nothing loading, and nothing Ready that has not been taken.
        bool idle() const;

        // For the stats window.
        AssetWorker const& worker() const { return worker_; }

    private:
        // The texture known by `key`, or a new one decoded on the worker by
        // `decode`, which logs and returns false on failure. It runs on the
        // worker, so it must capture only what it owns or shares.
        TextureHandle load_texture(string const& key, string name, TextureEncoding encoding,
                                   function<bool(Pixels& pixels)> decode);

        MaterialHandle add_material(MaterialAsset material);

        // On the main thread, once the worker has parsed the file: registers
        // its meshes, textures and materials and builds the node tree over
        // them.
        Model register_gltf(string const& path, GltfModel& source);

        vector<MeshAsset>     meshes_;
        vector<TextureAsset>  textures_;
        vector<MaterialAsset> materials_;
        vector<ModelAsset>    models_;

        std::unordered_map<string, TextureHandle>  texture_keys_;
        std::unordered_map<string, MaterialHandle> ambientcg_;
        std::unordered_map<string, ModelHandle>    model_paths_;

        vector<ReadyMesh>    ready_meshes_;
        vector<ReadyTexture> ready_textures_;
        vector<MeshHandle>   released_meshes_;
        vector<u32>          free_meshes_;

        // Last, so it stops before anything its jobs point into goes.
        AssetWorker worker_;
    };
}
