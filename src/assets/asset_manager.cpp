#include "core/pch.hpp"

#include "assets/asset_manager.hpp"

#include "core/log.hpp"
#include "render/gltf.hpp"
#include "render/material.hpp"

#include <filesystem>
#include <memory>
#include <utility>

namespace encke
{
    namespace
    {
        template<class Asset, class Tag>
        Asset const* resolve(vector<Asset> const& assets, Handle<Tag> handle)
        {
            if (!handle || handle.index >= assets.size() ||
                assets[handle.index].generation != handle.generation)
            {
                return nullptr;
            }
            return &assets[handle.index];
        }
    }

    AssetManager::~AssetManager()
    {
        stop();
    }

    void AssetManager::start()
    {
        worker_.start();
    }

    void AssetManager::stop()
    {
        worker_.stop();
    }

    MeshHandle AssetManager::add_mesh(string name, MeshData data)
    {
        MeshAsset asset{.name = std::move(name), .generation = 0, .state = AssetState::Ready};
        if (!data.vertices.empty())
        {
            asset.min = data.vertices.front().position;
            asset.max = asset.min;
            for (Vertex const& vertex : data.vertices)
            {
                asset.min = glm::min(asset.min, vertex.position);
                asset.max = glm::max(asset.max, vertex.position);
            }
        }

        MeshHandle const handle{.index = static_cast<u32>(meshes_.size()), .generation = 0};
        meshes_.push_back(std::move(asset));
        ready_meshes_.push_back(ReadyMesh{.handle = handle, .data = std::move(data)});
        return handle;
    }

    MaterialHandle AssetManager::add_material(MaterialAsset material)
    {
        MaterialHandle const handle{.index = static_cast<u32>(materials_.size()), .generation = 0};
        materials_.push_back(std::move(material));
        return handle;
    }

    TextureHandle AssetManager::load_texture(string const& key, string name,
                                             TextureEncoding encoding,
                                             function<bool(Pixels& pixels)> decode)
    {
        if (auto const found = texture_keys_.find(key); found != texture_keys_.end())
        {
            return found->second;
        }

        TextureHandle const handle{.index = static_cast<u32>(textures_.size()), .generation = 0};
        textures_.push_back(TextureAsset{
            .name       = std::move(name),
            .generation = 0,
            .state      = AssetState::Loading,
            .encoding   = encoding,
        });
        texture_keys_.emplace(key, handle);

        auto finish = [this, handle](bool ok, Pixels pixels) {
            TextureAsset& asset = textures_[handle.index];
            if (!ok)
            {
                // The decoder logged why; materials leave the texture out.
                asset.state = AssetState::Failed;
                log::error("texture %s failed; left out", asset.name.c_str());
                return;
            }
            asset.state = AssetState::Ready;
            ready_textures_.push_back(ReadyTexture{.handle = handle, .pixels = std::move(pixels)});
        };

        worker_.submit(AssetWorker::Job{
            .name = textures_[handle.index].name,
            .work = [decode = std::move(decode), finish]() -> AssetWorker::Finish {
                Pixels     pixels;
                bool const ok = decode(pixels);
                // Copyable, as a std::function must be; it only runs once.
                return [finish, ok, pixels = std::move(pixels)]() mutable {
                    finish(ok, std::move(pixels));
                };
            },
            .failed = [finish] { finish(false, Pixels{}); },
        });
        return handle;
    }

    MaterialHandle AssetManager::load_ambientcg(string const& set, f32vec2 tile)
    {
        if (auto const found = ambientcg_.find(set); found != ambientcg_.end())
        {
            return found->second;
        }

        auto map = [&](AmbientCgMap which, char const* suffix, TextureEncoding encoding) {
            string const name = set + " " + suffix;
            return load_texture("ambientcg:" + name, name, encoding, [set, which](Pixels& pixels) {
                return encke::load_ambientcg(set, which, pixels);
            });
        };

        MaterialHandle const handle = add_material(MaterialAsset{
            .name     = set,
            .albedo   = map(AmbientCgMap::Colour, "colour", TextureEncoding::Srgb),
            .normal   = map(AmbientCgMap::Normal, "normal", TextureEncoding::Linear),
            .orm      = map(AmbientCgMap::Orm, "orm", TextureEncoding::Linear),
            .emission = TextureHandle{},
            .tiling   = true,
            .tile     = tile,
        });
        ambientcg_.emplace(set, handle);
        return handle;
    }

    ModelHandle AssetManager::load_model(string const& path)
    {
        if (auto const found = model_paths_.find(path); found != model_paths_.end())
        {
            return found->second;
        }

        ModelHandle const handle{.index = static_cast<u32>(models_.size()), .generation = 0};
        models_.push_back(ModelAsset{
            .name       = std::filesystem::path{path}.filename().string(),
            .generation = 0,
            .state      = AssetState::Loading,
            .model      = nullopt,
        });
        model_paths_.emplace(path, handle);

        auto failed = [this, handle] {
            models_[handle.index].state = AssetState::Failed;
            log::error("model %s did not load", models_[handle.index].name.c_str());
        };

        worker_.submit(AssetWorker::Job{
            .name = models_[handle.index].name,
            .work = [this, handle, path, failed]() -> AssetWorker::Finish {
                // Shared, so the Finish stays copyable as std::function needs.
                auto source = std::make_shared<GltfModel>();
                if (!load_gltf(path, *source))
                {
                    return failed;
                }
                return [this, handle, path, source] {
                    ModelAsset& asset = models_[handle.index];
                    asset.model       = register_gltf(path, *source);
                    asset.state       = AssetState::Ready;
                };
            },
            .failed = failed,
        });
        return handle;
    }

    Model AssetManager::register_gltf(string const& path, GltfModel& source)
    {
        string const stem = std::filesystem::path{path}.stem().string();

        // Keyed by the file and what is decoded from it, so an image used by
        // several materials, or an ORM several materials pack alike, decodes
        // once. `images` is shared: the jobs outlive `source`.
        std::shared_ptr<GltfImages const> const images = source.images;

        auto image = [&](optional<u32> index, TextureEncoding encoding) -> TextureHandle {
            if (!index.has_value())
            {
                return TextureHandle{};
            }
            string const name = stem + " image " + std::to_string(*index);
            char const*  tag  = encoding == TextureEncoding::Srgb ? ":srgb" : ":linear";
            return load_texture(path + "#" + std::to_string(*index) + tag, name, encoding,
                                [images, image = *index](Pixels& pixels) {
                                    return decode_gltf_image(*images, image, pixels);
                                });
        };

        auto orm = [&](GltfOrm const& parts) -> TextureHandle {
            if (!parts.any())
            {
                return TextureHandle{};
            }
            auto part = [](optional<u32> index) {
                return index.has_value() ? std::to_string(*index) : string{"-"};
            };
            string const which = part(parts.occlusion) + "+" + part(parts.metal_rough);
            return load_texture(path + "#orm:" + which + ":" + std::to_string(parts.occlusion_strength),
                                stem + " orm " + which, TextureEncoding::Linear,
                                [images, parts](Pixels& pixels) {
                                    return decode_gltf_orm(*images, parts, pixels);
                                });
        };

        vector<MaterialHandle> materials;
        for (size_t index = 0; index < source.materials.size(); ++index)
        {
            GltfMaterial const& material = source.materials[index];
            materials.push_back(add_material(MaterialAsset{
                .name     = stem + " material " + std::to_string(index),
                .albedo   = image(material.albedo, TextureEncoding::Srgb),
                .normal   = image(material.normal, TextureEncoding::Linear),
                .orm      = orm(material.orm),
                .emission = image(material.emission, TextureEncoding::Srgb),
                .tiling   = false,
                .tile     = f32vec2{1.0f},
            }));
        }

        Model model;
        model.min = f64vec3{source.min};
        model.max = f64vec3{source.max};

        // A mesh asset per primitive, however many nodes instance it.
        for (size_t mesh_index = 0; mesh_index < source.meshes.size(); ++mesh_index)
        {
            GltfMesh&  source_mesh = source.meshes[mesh_index];
            ModelMesh& mesh        = model.meshes.emplace_back();
            string const name = source_mesh.name.empty() ? stem + " mesh " + std::to_string(mesh_index)
                                                         : stem + " " + source_mesh.name;

            for (size_t primitive_index = 0; primitive_index < source_mesh.primitives.size();
                 ++primitive_index)
            {
                GltfPrimitive&      primitive = source_mesh.primitives[primitive_index];
                GltfMaterial const& material  = source.materials[primitive.material];

                MeshHandle const handle =
                    add_mesh(name + " " + std::to_string(primitive_index),
                             MeshData{.vertices = std::move(primitive.vertices),
                                      .indices  = std::move(primitive.indices)});

                mesh.parts.push_back(ModelPart{
                    .mesh      = handle,
                    .material  = materials[primitive.material],
                    .albedo    = material.base_colour,
                    .roughness = material.roughness,
                    .metallic  = material.metallic,
                    .emissive  = material.emissive,
                });
            }
        }

        for (GltfNode const& node : source.nodes)
        {
            model.nodes.push_back(ModelNode{
                .name     = node.name,
                .parent   = node.parent,
                .position = f64vec3{node.position},
                .rotation = f64quat{node.rotation},
                .scale    = f64vec3{node.scale},
                .mesh     = node.mesh,
            });
        }

        return model;
    }

    void AssetManager::update()
    {
        for (AssetWorker::Finish& finish : worker_.take())
        {
            finish();
        }
    }

    MeshAsset const* AssetManager::mesh(MeshHandle handle) const
    {
        return resolve(meshes_, handle);
    }

    TextureAsset const* AssetManager::texture(TextureHandle handle) const
    {
        return resolve(textures_, handle);
    }

    MaterialAsset const* AssetManager::material(MaterialHandle handle) const
    {
        return resolve(materials_, handle);
    }

    ModelAsset const* AssetManager::model(ModelHandle handle) const
    {
        return resolve(models_, handle);
    }

    vector<AssetManager::ReadyMesh> AssetManager::take_ready_meshes()
    {
        return std::exchange(ready_meshes_, {});
    }

    vector<AssetManager::ReadyTexture> AssetManager::take_ready_textures()
    {
        return std::exchange(ready_textures_, {});
    }

    bool AssetManager::idle() const
    {
        return worker_.idle() && ready_meshes_.empty() && ready_textures_.empty();
    }
}
