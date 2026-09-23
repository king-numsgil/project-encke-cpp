#include "core/pch.hpp"

#include "render/gltf.hpp"

#include "core/log.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>

namespace encke
{
    namespace
    {
        namespace fs = std::filesystem;

        // Where one glTF image's encoded data is: the bytes, copied out of
        // the asset, or a file path. Neither when it cannot be loaded.
        struct ImageSource
        {
            vector<byte> bytes;
            string       path;
        };

        // By image index. Shared by every material's decode job, which runs
        // on the loader's worker after the asset itself is gone.
        using ImageSources = vector<ImageSource>;

        ImageSources locate_images(fastgltf::Asset const& asset, fs::path const& directory)
        {
            ImageSources sources(asset.images.size());

            for (size_t index = 0; index < asset.images.size(); ++index)
            {
                ImageSource& source = sources[index];

                auto copy = [&](span<byte const> bytes) {
                    source.bytes.assign(bytes.begin(), bytes.end());
                };

                std::visit(
                    fastgltf::visitor{
                        [&](fastgltf::sources::BufferView const& view) {
                            auto const bytes =
                                fastgltf::DefaultBufferDataAdapter{}(asset, view.bufferViewIndex);
                            copy(span<byte const>{bytes.data(), bytes.size()});
                        },
                        [&](fastgltf::sources::Array const& array) {
                            copy(span<byte const>{array.bytes.data(), array.bytes.size()});
                        },
                        [&](fastgltf::sources::URI const& uri) {
                            if (!uri.uri.isLocalPath() || uri.fileByteOffset != 0)
                            {
                                log::error("gltf image %zu: only whole local image files are "
                                           "supported", index);
                                return;
                            }
                            source.path = (directory / uri.uri.fspath()).string();
                        },
                        [&](auto const&) {
                            log::error("gltf image %zu: unsupported image source", index);
                        },
                    },
                    asset.images[index].data);
            }

            return sources;
        }

        // Decoded images for one job, by image index, so an image shared by
        // two of a material's textures -- occlusion and metallic-roughness,
        // often -- is decoded once.
        class ImageDecoder
        {
        public:
            explicit ImageDecoder(ImageSources const& sources)
                : sources_(sources), images_(sources.size())
            {
            }

            // Null when there is no image or it failed to decode; logged.
            Pixels const* image(optional<size_t> index)
            {
                if (!index.has_value())
                {
                    return nullptr;
                }

                if (!images_[*index].has_value())
                {
                    ImageSource const& source = sources_[*index];
                    string const       what   = "gltf image " + std::to_string(*index);

                    Pixels pixels;
                    bool   decoded = false;
                    if (!source.bytes.empty())
                    {
                        decoded = decode_pixels(source.bytes, what.c_str(), pixels);
                    }
                    else if (!source.path.empty())
                    {
                        decoded = load_pixels(source.path, pixels);
                    }
                    if (!decoded)
                    {
                        return nullptr;
                    }
                    images_[*index] = std::move(pixels);
                }

                return &*images_[*index];
            }

        private:
            ImageSources const&      sources_;
            vector<optional<Pixels>> images_;
        };

        // Which image each of a material's textures reads, resolved from the
        // asset up front so the decode job needs nothing but the sources.
        struct MaterialImages
        {
            optional<size_t> albedo;
            optional<size_t> normal;
            optional<size_t> emission;
            optional<size_t> occlusion;
            optional<size_t> metal_rough;
            f32              occlusion_strength = 1.0f;

            bool any() const
            {
                return albedo || normal || emission || occlusion || metal_rough;
            }
        };

        // The image a texture info points at. Nullopt, logged, when it has
        // no core image; absent infos are nullopt silently.
        template<class OptionalInfo>
        optional<size_t> image_of(fastgltf::Asset const& asset, OptionalInfo const& info)
        {
            if (!info.has_value())
            {
                return nullopt;
            }

            if (info->texCoordIndex != 0)
            {
                log::warn("gltf: texture %zu uses TEXCOORD_%zu; only TEXCOORD_0 is loaded",
                          info->textureIndex, info->texCoordIndex);
            }

            fastgltf::Texture const& texture = asset.textures[info->textureIndex];
            if (!texture.imageIndex.has_value())
            {
                log::warn("gltf: texture %zu has no core image", info->textureIndex);
                return nullopt;
            }
            return *texture.imageIndex;
        }

        // glTF keeps occlusion (R) apart from metallic-roughness (G, B),
        // though exporters often point both at one image. Packs them into one
        // ORM image; a missing part reads 1, leaving its factor alone.
        optional<Pixels> pack_orm(Pixels const* occlusion, f32 strength, Pixels const* metal_rough)
        {
            if (occlusion == nullptr && metal_rough == nullptr)
            {
                return nullopt;
            }

            if (occlusion != nullptr && metal_rough != nullptr &&
                (occlusion->width != metal_rough->width || occlusion->height != metal_rough->height))
            {
                log::warn("gltf: occlusion %ux%u and metallic-roughness %ux%u differ; "
                          "occlusion dropped", occlusion->width, occlusion->height,
                          metal_rough->width, metal_rough->height);
                occlusion = nullptr;
            }

            Pixels const& shape = metal_rough != nullptr ? *metal_rough : *occlusion;

            Pixels orm;
            orm.width  = shape.width;
            orm.height = shape.height;
            orm.rgba.resize(shape.rgba.size());

            for (size_t at = 0; at < orm.rgba.size(); at += 4)
            {
                u8 ao = 255;
                if (occlusion != nullptr)
                {
                    // glTF: occlusion = 1 + strength * (sampled - 1).
                    f32 const sampled = static_cast<f32>(occlusion->rgba[at]) / 255.0f;
                    f32 const value   = std::clamp(1.0f + strength * (sampled - 1.0f), 0.0f, 1.0f);
                    ao = static_cast<u8>(std::lround(value * 255.0f));
                }

                orm.rgba[at + 0] = ao;
                orm.rgba[at + 1] = metal_rough != nullptr ? metal_rough->rgba[at + 1] : u8{255};
                orm.rgba[at + 2] = metal_rough != nullptr ? metal_rough->rgba[at + 2] : u8{255};
                orm.rgba[at + 3] = 255;
            }

            return orm;
        }

        GltfMaterial convert_material(fastgltf::Material const& source, fastgltf::Asset const& asset,
                                      std::shared_ptr<ImageSources const> const& sources)
        {
            GltfMaterial material;

            fastgltf::PBRData const& pbr = source.pbrData;
            material.base_colour = f32vec3{pbr.baseColorFactor[0], pbr.baseColorFactor[1],
                                           pbr.baseColorFactor[2]};
            material.roughness   = pbr.roughnessFactor;
            material.metallic    = pbr.metallicFactor;
            material.emissive    = f32vec3{source.emissiveFactor[0], source.emissiveFactor[1],
                                           source.emissiveFactor[2]} *
                                   source.emissiveStrength;

            if (source.alphaMode != fastgltf::AlphaMode::Opaque)
            {
                log::warn("gltf: material \"%s\" is not opaque; drawn opaque",
                          source.name.c_str());
            }
            if (source.doubleSided)
            {
                log::warn("gltf: material \"%s\" is double-sided; drawn back-face culled",
                          source.name.c_str());
            }

            MaterialImages images;
            images.albedo      = image_of(asset, pbr.baseColorTexture);
            images.normal      = image_of(asset, source.normalTexture);
            images.emission    = image_of(asset, source.emissiveTexture);
            images.occlusion   = image_of(asset, source.occlusionTexture);
            images.metal_rough = image_of(asset, pbr.metallicRoughnessTexture);
            if (source.occlusionTexture.has_value())
            {
                images.occlusion_strength = source.occlusionTexture->strength;
            }

            material.has_emission_map = images.emission.has_value();
            if (!images.any())
            {
                return material;
            }

            // Everything the job reads is captured by value or shared, so it
            // runs safely after load_gltf and its asset are gone. A map that
            // fails to decode is logged and left out, as if absent.
            material.decode = [sources, images](MaterialMaps& maps) {
                ImageDecoder decoder{*sources};

                auto copy = [](Pixels const* pixels) -> optional<Pixels> {
                    return pixels != nullptr ? optional<Pixels>{*pixels} : nullopt;
                };

                maps          = MaterialMaps{};
                maps.albedo   = copy(decoder.image(images.albedo));
                maps.normal   = copy(decoder.image(images.normal));
                maps.emission = copy(decoder.image(images.emission));
                maps.orm      = pack_orm(decoder.image(images.occlusion), images.occlusion_strength,
                                         decoder.image(images.metal_rough));
                return true;
            };

            return material;
        }

        f32vec3 any_perpendicular(f32vec3 const& n)
        {
            f32vec3 const axis = std::abs(n.x) < 0.9f ? f32vec3{1.0f, 0.0f, 0.0f}
                                                      : f32vec3{0.0f, 1.0f, 0.0f};
            return glm::normalize(glm::cross(n, axis));
        }

        // Per-vertex tangents from the triangles' UV gradients, averaged
        // where triangles share a vertex. Not MikkTSpace: a normal map baked
        // against MikkTSpace can show faint seams under it, which is the
        // trade for not carrying the library.
        void generate_tangents(vector<Vertex>& vertices, span<u32 const> indices)
        {
            vector<f32vec3> along_u(vertices.size(), f32vec3{0.0f});
            vector<f32vec3> along_v(vertices.size(), f32vec3{0.0f});

            for (size_t at = 0; at + 2 < indices.size(); at += 3)
            {
                u32 const a = indices[at];
                u32 const b = indices[at + 1];
                u32 const c = indices[at + 2];

                f32vec3 const e1 = vertices[b].position - vertices[a].position;
                f32vec3 const e2 = vertices[c].position - vertices[a].position;
                f32vec2 const d1 = vertices[b].uv - vertices[a].uv;
                f32vec2 const d2 = vertices[c].uv - vertices[a].uv;

                f32 const det = d1.x * d2.y - d2.x * d1.y;
                if (std::abs(det) < 1e-12f)
                {
                    continue;
                }

                f32 const     r  = 1.0f / det;
                f32vec3 const du = (e1 * d2.y - e2 * d1.y) * r;   // dP/du
                f32vec3 const dv = (e2 * d1.x - e1 * d2.x) * r;   // dP/dv

                for (u32 const vertex : {a, b, c})
                {
                    along_u[vertex] += du;
                    along_v[vertex] += dv;
                }
            }

            for (size_t index = 0; index < vertices.size(); ++index)
            {
                Vertex&       vertex = vertices[index];
                f32vec3 const n      = vertex.normal;

                f32vec3 t = along_u[index] - n * glm::dot(n, along_u[index]);
                t = glm::length(t) > 1e-8f ? glm::normalize(t) : any_perpendicular(n);

                // cross(n, t) * w must be the top of the image, which is -v.
                f32 const w = glm::dot(glm::cross(n, t), along_v[index]) > 0.0f ? -1.0f : 1.0f;
                vertex.tangent = f32vec4{t, w};
            }
        }

        // Smooth normals, area-weighted, for a primitive that has none.
        void generate_normals(vector<Vertex>& vertices, span<u32 const> indices)
        {
            for (Vertex& vertex : vertices)
            {
                vertex.normal = f32vec3{0.0f};
            }

            for (size_t at = 0; at + 2 < indices.size(); at += 3)
            {
                u32 const     a = indices[at];
                u32 const     b = indices[at + 1];
                u32 const     c = indices[at + 2];
                f32vec3 const face =
                    glm::cross(vertices[b].position - vertices[a].position,
                               vertices[c].position - vertices[a].position);
                vertices[a].normal += face;
                vertices[b].normal += face;
                vertices[c].normal += face;
            }

            for (Vertex& vertex : vertices)
            {
                f32 const length = glm::length(vertex.normal);
                vertex.normal = length > 0.0f ? vertex.normal / length : f32vec3{0.0f, 1.0f, 0.0f};
            }
        }

        f32mat4 to_glm(fastgltf::math::fmat4x4 const& source)
        {
            f32mat4 result{1.0f};
            for (size_t column = 0; column < 4; ++column)
            {
                for (size_t row = 0; row < 4; ++row)
                {
                    result[column][row] = source[column][row];
                }
            }
            return result;
        }

        // A model-space matrix as position, rotation and scale. A mirroring
        // one comes back with a negative x scale, so the rotation is proper;
        // a shearing one is split as if it were not.
        struct Pose
        {
            f64vec3 position{0.0};
            f64quat rotation{1.0, 0.0, 0.0, 0.0};
            f64vec3 scale{1.0};
            bool    mirrored = false;
            bool    sheared  = false;
        };

        Pose decompose(f32mat4 const& matrix)
        {
            // Axes further off square than this are shear, not rounding.
            constexpr f64 kShear = 1.0e-4;

            f64mat3 const linear{f64mat4{matrix}};

            Pose pose;
            pose.position = f64vec3{f64mat4{matrix}[3]};
            pose.scale    = f64vec3{glm::length(linear[0]), glm::length(linear[1]),
                                    glm::length(linear[2])};

            // glTF files hide nodes with a zero scale; such a node has no
            // rotation to speak of.
            if (pose.scale.x <= 0.0 || pose.scale.y <= 0.0 || pose.scale.z <= 0.0)
            {
                return pose;
            }

            f64mat3 axes{linear[0] / pose.scale.x, linear[1] / pose.scale.y,
                         linear[2] / pose.scale.z};
            if (glm::determinant(axes) < 0.0)
            {
                axes[0]       = -axes[0];
                pose.scale.x  = -pose.scale.x;
                pose.mirrored = true;
            }

            pose.sheared  = std::abs(glm::dot(axes[0], axes[1])) > kShear ||
                            std::abs(glm::dot(axes[1], axes[2])) > kShear ||
                            std::abs(glm::dot(axes[2], axes[0])) > kShear;
            pose.rotation = glm::normalize(glm::quat_cast(axes));
            return pose;
        }

        // False only for a malformed primitive; unsupported ones are skipped
        // with a warning and leave `out` empty.
        bool convert_primitive(fastgltf::Asset const& asset, fastgltf::Primitive const& primitive,
                               GltfPrimitive& out)
        {
            if (primitive.type != fastgltf::PrimitiveType::Triangles)
            {
                log::warn("gltf: skipping a primitive of mode %d, not triangles",
                          static_cast<int>(primitive.type));
                return true;
            }

            auto const position = primitive.findAttribute("POSITION");
            if (position == primitive.attributes.cend())
            {
                log::error("gltf: primitive without POSITION");
                return false;
            }

            fastgltf::Accessor const& positions = asset.accessors[position->accessorIndex];
            if (!positions.bufferViewIndex.has_value())
            {
                log::warn("gltf: skipping a primitive whose positions have no buffer view");
                return true;
            }

            out.vertices.assign(positions.count,
                                Vertex{f32vec3{0.0f}, f32vec3{0.0f}, f32vec4{0.0f}, f32vec2{0.0f}});

            fastgltf::iterateAccessorWithIndex<f32vec3>(
                asset, positions, [&](f32vec3 value, size_t index) { out.vertices[index].position = value; });

            auto const normal = primitive.findAttribute("NORMAL");
            bool const has_normals = normal != primitive.attributes.cend();
            if (has_normals)
            {
                fastgltf::iterateAccessorWithIndex<f32vec3>(
                    asset, asset.accessors[normal->accessorIndex],
                    [&](f32vec3 value, size_t index) { out.vertices[index].normal = value; });
            }

            auto const uv = primitive.findAttribute("TEXCOORD_0");
            if (uv != primitive.attributes.cend())
            {
                fastgltf::iterateAccessorWithIndex<f32vec2>(
                    asset, asset.accessors[uv->accessorIndex],
                    [&](f32vec2 value, size_t index) { out.vertices[index].uv = value; });
            }

            auto const tangent = primitive.findAttribute("TANGENT");
            bool const has_tangents = tangent != primitive.attributes.cend() && has_normals;
            if (has_tangents)
            {
                fastgltf::iterateAccessorWithIndex<f32vec4>(
                    asset, asset.accessors[tangent->accessorIndex],
                    [&](f32vec4 value, size_t index) { out.vertices[index].tangent = value; });
            }

            if (primitive.indicesAccessor.has_value())
            {
                fastgltf::Accessor const& indices = asset.accessors[*primitive.indicesAccessor];
                out.indices.resize(indices.count);
                fastgltf::iterateAccessorWithIndex<u32>(
                    asset, indices, [&](u32 value, size_t index) { out.indices[index] = value; });
            }
            else
            {
                out.indices.resize(out.vertices.size());
                for (size_t index = 0; index < out.indices.size(); ++index)
                {
                    out.indices[index] = static_cast<u32>(index);
                }
            }

            if (!has_normals)
            {
                generate_normals(out.vertices, out.indices);
            }
            if (!has_tangents)
            {
                generate_tangents(out.vertices, out.indices);
            }

            out.min = f32vec3{std::numeric_limits<f32>::max()};
            out.max = f32vec3{std::numeric_limits<f32>::lowest()};
            for (Vertex const& vertex : out.vertices)
            {
                out.min = glm::min(out.min, vertex.position);
                out.max = glm::max(out.max, vertex.position);
            }

            return true;
        }
    }

    bool load_gltf(string const& path, GltfModel& model)
    {
        model = GltfModel{};

        auto data = fastgltf::GltfDataBuffer::FromPath(fs::path{path});
        if (data.error() != fastgltf::Error::None)
        {
            log::error("%s: %s", path.c_str(),
                       string{fastgltf::getErrorMessage(data.error())}.c_str());
            return false;
        }

        fastgltf::Parser parser{fastgltf::Extensions::KHR_materials_emissive_strength};
        auto loaded = parser.loadGltf(data.get(), fs::path{path}.parent_path(),
                                      fastgltf::Options::LoadExternalBuffers);
        if (loaded.error() != fastgltf::Error::None)
        {
            log::error("%s: %s", path.c_str(),
                       string{fastgltf::getErrorMessage(loaded.error())}.c_str());
            return false;
        }

        fastgltf::Asset& asset = loaded.get();
        if (asset.scenes.empty())
        {
            log::error("%s: no scenes", path.c_str());
            return false;
        }

        // Only located here; the materials' decode jobs read them later.
        auto const sources = std::make_shared<ImageSources const>(
            locate_images(asset, fs::path{path}.parent_path()));
        for (fastgltf::Material const& material : asset.materials)
        {
            model.materials.push_back(convert_material(material, asset, sources));
        }

        // Primitives without a material get glTF's default one: white,
        // fully metallic and rough, as the spec defines it.
        optional<u32> default_material;

        // Each mesh is converted once, the first time a node uses it, so
        // meshes the default scene never instances are not loaded at all.
        vector<optional<u32>> mesh_ids(asset.meshes.size());
        auto load_mesh = [&](size_t source_index) -> optional<u32> {
            if (mesh_ids[source_index].has_value())
            {
                return mesh_ids[source_index];
            }

            fastgltf::Mesh const& source = asset.meshes[source_index];
            GltfMesh              mesh;
            mesh.name = string{source.name.c_str()};

            for (fastgltf::Primitive const& primitive : source.primitives)
            {
                GltfPrimitive converted;
                if (!convert_primitive(asset, primitive, converted))
                {
                    return nullopt;
                }
                if (converted.vertices.empty())
                {
                    continue;
                }

                if (primitive.materialIndex.has_value())
                {
                    converted.material = static_cast<u32>(*primitive.materialIndex);
                }
                else
                {
                    if (!default_material.has_value())
                    {
                        default_material = static_cast<u32>(model.materials.size());
                        model.materials.emplace_back();
                    }
                    converted.material = *default_material;
                }

                mesh.primitives.push_back(std::move(converted));
            }

            mesh_ids[source_index] = static_cast<u32>(model.meshes.size());
            model.meshes.push_back(std::move(mesh));
            return mesh_ids[source_index];
        };

        // Depth first, so a parent is always pushed before its children.
        // glTF forbids a node having two parents, so each is visited once.
        // Each node's glTF transform, relative to its parent and inheriting
        // its scale, until the nodes are converted below.
        vector<f32mat4> locals;

        bool ok = true;
        auto visit = [&](size_t source_index, optional<u32> parent, auto& self) -> void {
            fastgltf::Node const& source = asset.nodes[source_index];

            GltfNode node;
            node.name   = string{source.name.c_str()};
            node.parent = parent;
            locals.push_back(to_glm(fastgltf::getTransformMatrix(source)));
            if (source.meshIndex.has_value())
            {
                node.mesh = load_mesh(*source.meshIndex);
                if (!node.mesh.has_value())
                {
                    ok = false;
                    return;
                }
            }

            u32 const index = static_cast<u32>(model.nodes.size());
            model.nodes.push_back(std::move(node));

            for (size_t const child : source.children)
            {
                self(child, index, self);
                if (!ok)
                {
                    return;
                }
            }
        };

        for (size_t const root : asset.scenes[asset.defaultScene.value_or(0)].nodeIndices)
        {
            visit(root, nullopt, visit);
            if (!ok)
            {
                return false;
            }
        }

        // Model-space transforms the glTF way, scale inherited: for the
        // bounds, and split into poses to convert each node to Transform's
        // convention, where scale is not inherited. A mirrored instance
        // needs its winding reversed, which a mesh shared with unmirrored
        // instances cannot have baked in, so that is logged; so is shear,
        // which a position, rotation and scale cannot hold.
        vector<f32mat4> world(model.nodes.size());
        vector<Pose>    poses(model.nodes.size());
        model.min = f32vec3{std::numeric_limits<f32>::max()};
        model.max = f32vec3{std::numeric_limits<f32>::lowest()};
        size_t instances = 0;
        size_t vertex_count = 0;

        for (size_t index = 0; index < model.nodes.size(); ++index)
        {
            GltfNode& node = model.nodes[index];
            world[index] = node.parent.has_value() ? world[*node.parent] * locals[index] : locals[index];
            poses[index] = decompose(world[index]);

            // Relative to the parent's position and rotation, not its scale.
            Pose const& pose = poses[index];
            if (node.parent.has_value())
            {
                Pose const&   above  = poses[*node.parent];
                f64quat const undo   = glm::conjugate(above.rotation);
                node.position = f32vec3{undo * (pose.position - above.position)};
                node.rotation = f32quat{undo * pose.rotation};
            }
            else
            {
                node.position = f32vec3{pose.position};
                node.rotation = f32quat{pose.rotation};
            }
            node.scale = f32vec3{pose.scale};

            if (!node.mesh.has_value())
            {
                continue;
            }

            if (pose.mirrored)
            {
                log::warn("gltf: node \"%s\" mirrors its mesh; drawn with the wrong winding",
                          node.name.c_str());
            }
            if (pose.sheared)
            {
                log::warn("gltf: node \"%s\" is sheared by an uneven scale above it; drawn "
                          "without the shear",
                          node.name.c_str());
            }

            for (GltfPrimitive const& primitive : model.meshes[*node.mesh].primitives)
            {
                ++instances;
                for (u32 corner = 0; corner < 8; ++corner)
                {
                    f32vec3 const local{(corner & 1u) != 0 ? primitive.max.x : primitive.min.x,
                                        (corner & 2u) != 0 ? primitive.max.y : primitive.min.y,
                                        (corner & 4u) != 0 ? primitive.max.z : primitive.min.z};
                    f32vec3 const at{world[index] * f32vec4{local, 1.0f}};
                    model.min = glm::min(model.min, at);
                    model.max = glm::max(model.max, at);
                }
            }
        }

        if (instances == 0)
        {
            log::error("%s: no triangle primitives in the default scene", path.c_str());
            return false;
        }

        size_t primitive_count = 0;
        for (GltfMesh const& mesh : model.meshes)
        {
            primitive_count += mesh.primitives.size();
            for (GltfPrimitive const& primitive : mesh.primitives)
            {
                vertex_count += primitive.vertices.size();
            }
        }

        log::info("gltf %s: %zu nodes, %zu meshes, %zu primitives drawn as %zu instances, "
                  "%zu vertices, %zu materials", path.c_str(), model.nodes.size(),
                  model.meshes.size(), primitive_count, instances, vertex_count,
                  model.materials.size());
        return true;
    }
}
