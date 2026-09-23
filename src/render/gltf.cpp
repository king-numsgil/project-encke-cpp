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
#include <utility>

namespace encke
{
    namespace
    {
        namespace fs = std::filesystem;

        // Decoded images, by glTF image index, so one shared by several
        // textures is decoded once.
        class ImageCache
        {
        public:
            ImageCache(fastgltf::Asset const& asset, fs::path directory)
                : asset_(asset), directory_(std::move(directory)), images_(asset.images.size())
            {
            }

            // Null when the texture has no loadable image; the reason is logged.
            Pixels const* texture(fastgltf::TextureInfo const* info)
            {
                if (info == nullptr)
                {
                    return nullptr;
                }

                if (info->texCoordIndex != 0)
                {
                    log::warn("gltf: texture %zu uses TEXCOORD_%zu; only TEXCOORD_0 is loaded",
                              info->textureIndex, info->texCoordIndex);
                }

                fastgltf::Texture const& texture = asset_.textures[info->textureIndex];
                if (!texture.imageIndex.has_value())
                {
                    log::warn("gltf: texture %zu has no core image", info->textureIndex);
                    return nullptr;
                }

                size_t const index = *texture.imageIndex;
                if (!images_[index].has_value())
                {
                    Pixels pixels;
                    if (!decode(asset_.images[index], index, pixels))
                    {
                        return nullptr;
                    }
                    images_[index] = std::move(pixels);
                }

                return &*images_[index];
            }

        private:
            bool decode(fastgltf::Image const& image, size_t index, Pixels& pixels) const
            {
                string const what = "gltf image " + std::to_string(index);

                auto from_bytes = [&](span<byte const> bytes) {
                    return decode_pixels(bytes, what.c_str(), pixels);
                };

                return std::visit(
                    fastgltf::visitor{
                        [&](fastgltf::sources::BufferView const& view) {
                            auto const bytes =
                                fastgltf::DefaultBufferDataAdapter{}(asset_, view.bufferViewIndex);
                            return from_bytes(span<byte const>{bytes.data(), bytes.size()});
                        },
                        [&](fastgltf::sources::Array const& array) {
                            return from_bytes(span<byte const>{array.bytes.data(), array.bytes.size()});
                        },
                        [&](fastgltf::sources::URI const& uri) {
                            if (!uri.uri.isLocalPath() || uri.fileByteOffset != 0)
                            {
                                log::error("%s: only whole local image files are supported",
                                           what.c_str());
                                return false;
                            }
                            return load_pixels((directory_ / uri.uri.fspath()).string(), pixels);
                        },
                        [&](auto const&) {
                            log::error("%s: unsupported image source", what.c_str());
                            return false;
                        },
                    },
                    image.data);
            }

            fastgltf::Asset const&   asset_;
            fs::path                 directory_;
            vector<optional<Pixels>> images_;
        };

        // Any of fastgltf's optional texture infos, as a nullable pointer.
        template<class OptionalInfo>
        fastgltf::TextureInfo const* info_of(OptionalInfo const& info)
        {
            return info.has_value() ? &*info : nullptr;
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

        GltfMaterial convert_material(fastgltf::Material const& source, ImageCache& images)
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

            auto copy = [](Pixels const* pixels) -> optional<Pixels> {
                return pixels != nullptr ? optional<Pixels>{*pixels} : nullopt;
            };

            material.albedo   = copy(images.texture(info_of(pbr.baseColorTexture)));
            material.normal   = copy(images.texture(info_of(source.normalTexture)));
            material.emission = copy(images.texture(info_of(source.emissiveTexture)));

            f32 const strength = source.occlusionTexture.has_value()
                                     ? source.occlusionTexture->strength
                                     : 1.0f;
            material.orm = pack_orm(images.texture(info_of(source.occlusionTexture)), strength,
                                    images.texture(info_of(pbr.metallicRoughnessTexture)));

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

        // False only for a malformed primitive; unsupported ones are skipped
        // with a warning and leave `out` empty.
        bool convert_primitive(fastgltf::Asset const& asset, fastgltf::Primitive const& primitive,
                               f32mat4 const& world, GltfPrimitive& out)
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

            // Bake the node transform in. Normals go through the inverse
            // transpose; a mirroring transform reverses the winding, and
            // the tangent frame's handedness with it.
            f32mat3 const linear{world};
            f32mat3 const normal_matrix = glm::transpose(glm::inverse(linear));
            bool const    mirrored      = glm::determinant(linear) < 0.0f;

            for (Vertex& vertex : out.vertices)
            {
                vertex.position = f32vec3{world * f32vec4{vertex.position, 1.0f}};
                if (has_normals)
                {
                    vertex.normal = glm::normalize(normal_matrix * vertex.normal);
                }
                if (has_tangents)
                {
                    vertex.tangent = f32vec4{glm::normalize(linear * f32vec3{vertex.tangent}),
                                             mirrored ? -vertex.tangent.w : vertex.tangent.w};
                }
            }

            if (mirrored)
            {
                for (size_t at = 0; at + 2 < out.indices.size(); at += 3)
                {
                    std::swap(out.indices[at + 1], out.indices[at + 2]);
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

        ImageCache images{asset, fs::path{path}.parent_path()};
        for (fastgltf::Material const& material : asset.materials)
        {
            model.materials.push_back(convert_material(material, images));
        }

        // Primitives without a material get glTF's default one: white,
        // fully metallic and rough, as the spec defines it.
        optional<u32> default_material;

        bool ok = true;
        fastgltf::iterateSceneNodes(
            asset, asset.defaultScene.value_or(0), fastgltf::math::fmat4x4(1.0f),
            [&](fastgltf::Node& node, fastgltf::math::fmat4x4 const& matrix) {
                if (!ok || !node.meshIndex.has_value())
                {
                    return;
                }

                f32mat4 const world = to_glm(matrix);
                for (fastgltf::Primitive const& primitive : asset.meshes[*node.meshIndex].primitives)
                {
                    GltfPrimitive converted;
                    if (!convert_primitive(asset, primitive, world, converted))
                    {
                        ok = false;
                        return;
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

                    model.primitives.push_back(std::move(converted));
                }
            });

        if (!ok)
        {
            return false;
        }
        if (model.primitives.empty())
        {
            log::error("%s: no triangle primitives in the default scene", path.c_str());
            return false;
        }

        model.min = model.primitives.front().min;
        model.max = model.primitives.front().max;
        size_t vertex_count = 0;
        for (GltfPrimitive const& primitive : model.primitives)
        {
            model.min = glm::min(model.min, primitive.min);
            model.max = glm::max(model.max, primitive.max);
            vertex_count += primitive.vertices.size();
        }

        log::info("gltf %s: %zu primitives, %zu vertices, %zu materials", path.c_str(),
                  model.primitives.size(), vertex_count, model.materials.size());
        return true;
    }
}
