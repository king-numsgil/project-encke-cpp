#include "core/pch.hpp"

#include "terrain/macro_field.hpp"

#include "core/log.hpp"
#include "terrain/fastnoise.hpp"

#include <FastNoise/Metadata.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>

namespace encke::terrain
{
    namespace
    {
        FastNoise::Metadata::MemberVariable::ValueUnion& variable(FastNoise::NodeData& node, string_view name)
        {
            auto const& members = node.metadata->memberVariables;
            for (size_t i = 0; i < members.size(); ++i)
            {
                if (name == members[i].name)
                {
                    return node.variables[i];
                }
            }
            throw std::logic_error("FastNoise2 node has no variable " + string{name});
        }

        f32& hybrid(FastNoise::NodeData& node, string_view name)
        {
            auto const& members = node.metadata->memberHybrids;
            for (size_t i = 0; i < members.size(); ++i)
            {
                if (name == members[i].name)
                {
                    return node.hybrids[i].second;
                }
            }
            throw std::logic_error("FastNoise2 node has no hybrid " + string{name});
        }
    }

    struct MacroField::Nodes
    {
        array<FastNoise::SmartNode<>, kMacroChannelCount> graphs;
    };

    MacroField::MacroField(MacroSpec const& spec, i32 seed)
        : spec_(spec)
        , seed_(seed)
        , nodes_(std::make_unique<Nodes>())
    {
        for (size_t channel = 0; channel < kMacroChannelCount; ++channel)
        {
            string const& graph = spec_.channels[channel].graph;
            if (graph.empty())
            {
                continue;
            }

            nodes_->graphs[channel] = FastNoise::NewFromEncodedNodeTree(graph.c_str(), kFeatureSet);
            if (!nodes_->graphs[channel])
            {
                // A bad string is an authoring error, and a terrain silently
                // missing a channel is worse than no terrain.
                throw std::runtime_error("macro channel " + std::to_string(channel) +
                                         ": FastNoise2 could not decode its node tree");
            }
        }
    }

    MacroField::~MacroField() = default;

    bool MacroField::fill_constants(size_t count, MacroValues& out, size_t first, size_t last)
    {
        for (size_t channel = first; channel < last; ++channel)
        {
            out.channels[channel].resize(count);
        }

        bool any_graph = false;
        for (size_t channel = first; channel < last; ++channel)
        {
            if (nodes_->graphs[channel])
            {
                any_graph = true;
            }
            else
            {
                std::fill(out.channels[channel].begin(), out.channels[channel].end(),
                          spec_.channels[channel].bias);
            }
        }
        return any_graph && count != 0;
    }

    void MacroField::lay_nodes(i64vec3 const& low, i64vec3 const& dims, f64 spacing)
    {
        size_t const nodes = static_cast<size_t>(dims.x * dims.y * dims.z);
        node_x_.resize(nodes);
        node_y_.resize(nodes);
        node_z_.resize(nodes);
        node_values_.resize(nodes);

        size_t n = 0;
        for (i64 k = 0; k < dims.z; ++k)
        {
            for (i64 j = 0; j < dims.y; ++j)
            {
                for (i64 i = 0; i < dims.x; ++i)
                {
                    // The same node gets the same f32 position whichever chunk
                    // or query asks for it, so the same value.
                    node_x_[n] = static_cast<f32>(static_cast<f64>(low.x + i) * spacing);
                    node_y_[n] = static_cast<f32>(static_cast<f64>(low.y + j) * spacing);
                    node_z_[n] = static_cast<f32>(static_cast<f64>(low.z + k) * spacing);
                    ++n;
                }
            }
        }
    }

    void MacroField::sample_grid(f64vec3 const& origin, f64 voxel_size,
                                 span<f32 const> x, span<f32 const> y, span<f32 const> z,
                                 MacroValues& out, size_t first, size_t last)
    {
        size_t const nx    = x.size();
        size_t const ny    = y.size();
        size_t const nz    = z.size();
        size_t const count = nx * ny * nz;
        if (!fill_constants(count, out, first, last))
        {
            return;
        }

        f64 const spacing = std::max(spec_.lattice_spacing, voxel_size);

        // Per axis, exactly the arithmetic sample() does per component.
        auto const find_cells = [spacing](f64 corner, span<f32 const> offsets, Axis& axis) {
            size_t const n = offsets.size();
            axis.cell.resize(n);
            axis.weight.resize(n);
            axis.low  = std::numeric_limits<i64>::max();
            axis.high = std::numeric_limits<i64>::min();
            for (size_t i = 0; i < n; ++i)
            {
                f64 const q     = (corner + static_cast<f64>(offsets[i])) / spacing;
                f64 const floor = std::floor(q);
                axis.cell[i]   = static_cast<i64>(floor);
                axis.weight[i] = q - floor;
                axis.low  = std::min(axis.low, axis.cell[i]);
                axis.high = std::max(axis.high, axis.cell[i] + 1);
            }
            // From here, relative to the lowest.
            for (i64& cell : axis.cell)
            {
                cell -= axis.low;
            }
        };
        find_cells(origin.x, x, axes_[0]);
        find_cells(origin.y, y, axes_[1]);
        find_cells(origin.z, z, axes_[2]);
        Axis const& ax = axes_[0];
        Axis const& ay = axes_[1];
        Axis const& az = axes_[2];

        i64vec3 const low{ax.low, ay.low, az.low};
        i64vec3 const dims = i64vec3{ax.high, ay.high, az.high} - low + i64vec3{1};
        lay_nodes(low, dims, spacing);

        size_t const dx    = static_cast<size_t>(dims.x);
        size_t const dy    = static_cast<size_t>(dims.y);
        size_t const dz    = static_cast<size_t>(dims.z);
        size_t const nodes = dx * dy * dz;
        along_x_.resize(dz * dy * nx);
        along_y_.resize(dz * ny * nx);

        for (size_t channel = first; channel < last; ++channel)
        {
            FastNoise::SmartNode<> const& graph = nodes_->graphs[channel];
            if (!graph)
            {
                continue;
            }

            graph->GenPositionArray3D(node_values_.data(), static_cast<int>(nodes),
                                      node_x_.data(), node_y_.data(), node_z_.data(),
                                      0.0f, 0.0f, 0.0f, seed_);

            // sample() lerps along x on the four node rows around a point,
            // then y, then z. Separably: along x on every node row, then
            // along y between those, then along z. Each value is the same
            // lerp of the same operands, so the same bits.
            for (size_t row = 0; row < dz * dy; ++row)
            {
                f32 const* const nodes_row = node_values_.data() + row * dx;
                f64* const       into      = along_x_.data() + row * nx;
                for (size_t i = 0; i < nx; ++i)
                {
                    size_t const c = static_cast<size_t>(ax.cell[i]);
                    into[i] = glm::mix(static_cast<f64>(nodes_row[c]), static_cast<f64>(nodes_row[c + 1]),
                                       ax.weight[i]);
                }
            }

            for (size_t k = 0; k < dz; ++k)
            {
                for (size_t j = 0; j < ny; ++j)
                {
                    f64 const* const below = along_x_.data() + (k * dy + static_cast<size_t>(ay.cell[j])) * nx;
                    f64 const* const above = below + nx;
                    f64* const       into  = along_y_.data() + (k * ny + j) * nx;
                    f64 const        t     = ay.weight[j];
                    for (size_t i = 0; i < nx; ++i)
                    {
                        into[i] = glm::mix(below[i], above[i], t);
                    }
                }
            }

            MacroChannelSpec const& mapping = spec_.channels[channel];
            f64 const               bias    = static_cast<f64>(mapping.bias);
            f64 const               scale   = static_cast<f64>(mapping.scale);
            f32* const              values  = out.channels[channel].data();
            for (size_t k = 0; k < nz; ++k)
            {
                for (size_t j = 0; j < ny; ++j)
                {
                    f64 const* const below = along_y_.data() + (static_cast<size_t>(az.cell[k]) * ny + j) * nx;
                    f64 const* const above = below + ny * nx;
                    f32* const       into  = values + (k * ny + j) * nx;
                    f64 const        t     = az.weight[k];
                    for (size_t i = 0; i < nx; ++i)
                    {
                        into[i] = static_cast<f32>(bias + scale * glm::mix(below[i], above[i], t));
                    }
                }
            }
        }
    }

    void MacroField::sample_points(span<f32 const> x, span<f32 const> y, span<f32 const> z,
                                   MacroValues& out, size_t first, size_t last)
    {
        size_t const count = x.size();
        if (!fill_constants(count, out, first, last))
        {
            return;
        }

        node_values_.resize(count);
        for (size_t channel = first; channel < last; ++channel)
        {
            FastNoise::SmartNode<> const& graph = nodes_->graphs[channel];
            if (!graph)
            {
                continue;
            }

            graph->GenPositionArray3D(node_values_.data(), static_cast<int>(count), x.data(), y.data(), z.data(),
                                      0.0f, 0.0f, 0.0f, seed_);

            MacroChannelSpec const& mapping = spec_.channels[channel];
            span<f32> const         values  = out.channels[channel];
            for (size_t i = 0; i < count; ++i)
            {
                values[i] = static_cast<f32>(static_cast<f64>(mapping.bias) +
                                             static_cast<f64>(mapping.scale) * static_cast<f64>(node_values_[i]));
            }
        }
    }

    void MacroField::sample(f64vec3 const& origin, f64 voxel_size,
                            span<f32 const> x, span<f32 const> y, span<f32 const> z,
                            MacroValues& out, size_t first, size_t last)
    {
        size_t const count = x.size();
        if (!fill_constants(count, out, first, last))
        {
            return;
        }

        f64 const spacing = std::max(spec_.lattice_spacing, voxel_size);

        // Each point's lattice cell and its weights within it. They come from
        // the f64 point, never from the chunk, which is what makes every
        // sampler agree on them.
        cells_.resize(count);
        weights_.resize(count);
        i64vec3 low{std::numeric_limits<i64>::max()};
        i64vec3 high{std::numeric_limits<i64>::min()};
        for (size_t i = 0; i < count; ++i)
        {
            f64vec3 const point = origin + f64vec3{x[i], y[i], z[i]};
            f64vec3 const q     = point / spacing;
            f64vec3 const floor = glm::floor(q);
            cells_[i]   = i64vec3{floor};
            weights_[i] = q - floor;
            low  = glm::min(low, cells_[i]);
            high = glm::max(high, cells_[i] + i64vec3{1});
        }

        i64vec3 const dims  = high - low + i64vec3{1};
        size_t const  nodes = static_cast<size_t>(dims.x * dims.y * dims.z);
        lay_nodes(low, dims, spacing);

        size_t const stride_y = static_cast<size_t>(dims.x);
        size_t const stride_z = static_cast<size_t>(dims.x * dims.y);

        for (size_t channel = first; channel < last; ++channel)
        {
            FastNoise::SmartNode<> const& graph = nodes_->graphs[channel];
            if (!graph)
            {
                continue;
            }

            graph->GenPositionArray3D(node_values_.data(), static_cast<int>(nodes),
                                      node_x_.data(), node_y_.data(), node_z_.data(),
                                      0.0f, 0.0f, 0.0f, seed_);

            MacroChannelSpec const& mapping = spec_.channels[channel];
            span<f32> const         values  = out.channels[channel];

            for (size_t i = 0; i < count; ++i)
            {
                i64vec3 const index = cells_[i] - low;
                f32 const*    base  = node_values_.data() + static_cast<size_t>((index.z * dims.y + index.y) * dims.x + index.x);
                f64vec3 const t     = weights_[i];

                auto const c = [base](size_t offset) { return static_cast<f64>(base[offset]); };

                f64 const x00 = glm::mix(c(0), c(1), t.x);
                f64 const x10 = glm::mix(c(stride_y), c(stride_y + 1), t.x);
                f64 const x01 = glm::mix(c(stride_z), c(stride_z + 1), t.x);
                f64 const x11 = glm::mix(c(stride_z + stride_y), c(stride_z + stride_y + 1), t.x);
                f64 const v   = glm::mix(glm::mix(x00, x10, t.y), glm::mix(x01, x11, t.y), t.z);

                values[i] = static_cast<f32>(static_cast<f64>(mapping.bias) + static_cast<f64>(mapping.scale) * v);
            }
        }
    }

    string encode_fbm_graph(f32 feature_scale, i32 octaves, f32 gain, f32 lacunarity)
    {
        FastNoise::NodeData simplex{&FastNoise::Metadata::Get<FastNoise::Simplex>()};
        variable(simplex, "Feature Scale") = feature_scale;

        FastNoise::NodeData fbm{&FastNoise::Metadata::Get<FastNoise::FractalFBm>()};
        fbm.nodeLookups.at(0)        = &simplex;
        variable(fbm, "Octaves")     = octaves;
        variable(fbm, "Lacunarity")  = lacunarity;
        hybrid(fbm, "Gain")          = gain;

        string encoded = FastNoise::Metadata::SerialiseNodeData(&fbm);
        if (encoded.empty())
        {
            throw std::logic_error("FastNoise2 could not encode an FBm graph");
        }
        return encoded;
    }

    // A deque, so nodes keep their addresses as more are added: they point
    // at one another.
    struct GraphBuilder::Nodes
    {
        std::deque<FastNoise::NodeData> data;

        template <typename T>
        std::pair<Node, FastNoise::NodeData&> make()
        {
            data.emplace_back(&FastNoise::Metadata::Get<T>());
            return {static_cast<Node>(data.size() - 1), data.back()};
        }

        FastNoise::NodeData* at(Node node) { return &data.at(node); }
    };

    GraphBuilder::GraphBuilder()
        : nodes_(std::make_unique<Nodes>())
    {
    }

    GraphBuilder::~GraphBuilder() = default;

    GraphBuilder::Node GraphBuilder::simplex(f32 feature_scale, i32 seed_offset)
    {
        auto [node, data]              = nodes_->make<FastNoise::Simplex>();
        variable(data, "Feature Scale") = feature_scale;
        variable(data, "Seed Offset")   = seed_offset;
        return node;
    }

    GraphBuilder::Node GraphBuilder::fbm(Node source, i32 octaves, f32 gain, f32 lacunarity)
    {
        FastNoise::NodeData* const from = nodes_->at(source);
        auto [node, data]               = nodes_->make<FastNoise::FractalFBm>();
        data.nodeLookups.at(0)          = from;
        variable(data, "Octaves")       = octaves;
        variable(data, "Lacunarity")    = lacunarity;
        hybrid(data, "Gain")            = gain;
        return node;
    }

    GraphBuilder::Node GraphBuilder::ridged(Node source, i32 octaves, f32 gain, f32 lacunarity)
    {
        FastNoise::NodeData* const from = nodes_->at(source);
        auto [node, data]               = nodes_->make<FastNoise::FractalRidged>();
        data.nodeLookups.at(0)          = from;
        variable(data, "Octaves")       = octaves;
        variable(data, "Lacunarity")    = lacunarity;
        hybrid(data, "Gain")            = gain;
        return node;
    }

    GraphBuilder::Node GraphBuilder::warp(Node source, f32 amplitude, f32 feature_scale, i32 seed_offset)
    {
        FastNoise::NodeData* const from = nodes_->at(source);
        auto [node, data]               = nodes_->make<FastNoise::DomainWarpGradient>();
        data.nodeLookups.at(0)          = from;
        hybrid(data, "Warp Amplitude")  = amplitude;
        variable(data, "Feature Scale") = feature_scale;
        variable(data, "Seed Offset")   = seed_offset;
        return node;
    }

    GraphBuilder::Node GraphBuilder::add(Node a, Node b)
    {
        FastNoise::NodeData* const lhs = nodes_->at(a);
        FastNoise::NodeData* const rhs = nodes_->at(b);
        auto [node, data]              = nodes_->make<FastNoise::Add>();
        data.nodeLookups.at(0)         = lhs;
        data.hybrids.at(0).first       = rhs;
        return node;
    }

    GraphBuilder::Node GraphBuilder::multiply(Node a, Node b)
    {
        FastNoise::NodeData* const lhs = nodes_->at(a);
        FastNoise::NodeData* const rhs = nodes_->at(b);
        auto [node, data]              = nodes_->make<FastNoise::Multiply>();
        data.nodeLookups.at(0)         = lhs;
        data.hybrids.at(0).first       = rhs;
        return node;
    }

    GraphBuilder::Node GraphBuilder::scale(Node a, f32 factor)
    {
        FastNoise::NodeData* const lhs = nodes_->at(a);
        auto [node, data]              = nodes_->make<FastNoise::Multiply>();
        data.nodeLookups.at(0)         = lhs;
        hybrid(data, "RHS")            = factor;
        return node;
    }

    GraphBuilder::Node GraphBuilder::remap(Node source, f32 from_min, f32 from_max, f32 to_min, f32 to_max, bool clamp)
    {
        FastNoise::NodeData* const from = nodes_->at(source);
        auto [node, data]               = nodes_->make<FastNoise::Remap>();
        data.nodeLookups.at(0)          = from;
        hybrid(data, "From Min")        = from_min;
        hybrid(data, "From Max")        = from_max;
        hybrid(data, "To Min")          = to_min;
        hybrid(data, "To Max")          = to_max;
        variable(data, "Clamp Output")  = clamp ? 1 : 0;
        return node;
    }

    GraphBuilder::Node GraphBuilder::power(Node source, f32 exponent)
    {
        FastNoise::NodeData* const from = nodes_->at(source);
        auto [node, data]               = nodes_->make<FastNoise::PowFloat>();
        data.hybrids.at(0).first        = from;
        hybrid(data, "Pow")             = exponent;
        return node;
    }

    string GraphBuilder::encode(Node root) const
    {
        string encoded = FastNoise::Metadata::SerialiseNodeData(nodes_->at(root));
        if (encoded.empty())
        {
            throw std::logic_error("FastNoise2 could not encode a built graph");
        }
        return encoded;
    }
}
