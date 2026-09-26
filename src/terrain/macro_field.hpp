#pragma once

#include <memory>

namespace encke::terrain
{
    // The macro layer: continents, range placement, biome masks, large warps.
    // FastNoise2 node graphs authored in its Node Editor, loaded from their
    // encoded strings. A graph has one scalar output, so each channel is its
    // own graph.
    //
    // The first four shape the field. The climate channels do not: the
    // field never samples them, and they are read at mesh vertices to choose
    // the ground's materials.
    enum class MacroChannel : u32
    {
        Height,            // metres above the body's radius
        DetailAmplitude,   // metres: the detail layer's first-octave amplitude
        Ridge,             // 0 plain fBm, 1 fully ridged
        Persistence,       // amplitude ratio from one detail octave to the next
        Temperature,       // degrees Celsius against the latitude's own, at the body's radius
        Moisture,          // 0 arid to 1 wet, before the dry subtropics
    };
    inline constexpr size_t kMacroChannelCount = 6;
    inline constexpr size_t kFieldChannelCount = 4;

    // bias + scale * graph(p). An empty graph is the constant bias.
    struct MacroChannelSpec
    {
        string graph;
        f32    scale = 1.0f;
        f32    bias  = 0.0f;
    };

    // Graphs are fed f32 body-relative positions in metres, which near an
    // Earth-sized body resolve to about half a metre. That is why this layer
    // is kept to wavelengths of a kilometre and up.
    //
    // They are evaluated on a lattice fixed to the body, `lattice_spacing`
    // apart, or one voxel apart where voxels are larger, and interpolated
    // trilinearly. The lattice is the body's, not the chunk's, so a point gets
    // the same value from every chunk that samples it, and from a point query.
    // Voxel sizes and the spacing must be powers of two for that to hold
    // across LODs: then a coarse chunk's samples all sit on nodes the finer
    // chunk's lattice also has.
    struct MacroSpec
    {
        array<MacroChannelSpec, kMacroChannelCount> channels{};
        f64 lattice_spacing = 64.0;
    };

    // One value per point per channel.
    struct MacroValues
    {
        array<vector<f32>, kMacroChannelCount> channels;

        span<f32 const> operator[](MacroChannel channel) const
        {
            return channels[static_cast<size_t>(channel)];
        }
    };

    // One instance per thread: FastNoise2 nodes are not shared.
    class MacroField
    {
    public:
        MacroField(MacroSpec const& spec, i32 seed);
        ~MacroField();

        MacroField(MacroField const&)            = delete;
        MacroField& operator=(MacroField const&) = delete;

        // Channels `first` to `last` - 1 at origin + (x[i], y[i], z[i]),
        // metres; the others are left as they were. The field's channels by
        // default.
        void sample(f64vec3 const& origin, f64 voxel_size,
                    span<f32 const> x, span<f32 const> y, span<f32 const> z,
                    MacroValues& out, size_t first = 0, size_t last = kFieldChannelCount);

        // The same over the grid origin + (x[i], y[j], z[k]), x fastest:
        // bit for bit what sample() gives at those points. Cells and weights
        // are found once per axis and the interpolation is separable, so it
        // costs a few lerps a point, and vectorises.
        void sample_grid(f64vec3 const& origin, f64 voxel_size,
                         span<f32 const> x, span<f32 const> y, span<f32 const> z,
                         MacroValues& out, size_t first = 0, size_t last = kFieldChannelCount);

    private:
        struct Nodes;

        // One axis of a grid: each coordinate's lattice cell, relative to
        // the lowest, and its weight within it.
        struct Axis
        {
            vector<i64> cell;
            vector<f64> weight;
            i64         low  = 0;
            i64         high = 0;
        };

        // Sizes the channels and fills those without a graph with their
        // bias. False when there is nothing left to evaluate.
        bool fill_constants(size_t count, MacroValues& out, size_t first, size_t last);

        // The positions of the lattice nodes from `low`, `dims` a side.
        void lay_nodes(i64vec3 const& low, i64vec3 const& dims, f64 spacing);

        MacroSpec              spec_;
        i32                    seed_ = 0;
        std::unique_ptr<Nodes> nodes_;
        vector<f32>            node_x_;
        vector<f32>            node_y_;
        vector<f32>            node_z_;
        vector<f32>            node_values_;
        vector<i64vec3>        cells_;
        vector<f64vec3>        weights_;
        array<Axis, 3>         axes_;
        vector<f64>            along_x_;
        vector<f64>            along_y_;
    };

    // An FBm-over-Simplex graph, encoded as the Node Editor would: for
    // defaults and tests until bodies carry authored graphs.
    string encode_fbm_graph(f32 feature_scale, i32 octaves, f32 gain, f32 lacunarity);

    // Builds a FastNoise2 node graph in code and encodes it as the Node
    // Editor would, for bodies without authored graphs. Nodes are numbered
    // in order of creation and live as long as the builder. Positions are
    // metres, so feature scales and warp amplitudes are too. Every noise
    // takes a seed offset, so two noises of the same scale do not share
    // their features.
    class GraphBuilder
    {
    public:
        using Node = u32;

        GraphBuilder();
        ~GraphBuilder();

        GraphBuilder(GraphBuilder const&)            = delete;
        GraphBuilder& operator=(GraphBuilder const&) = delete;

        Node simplex(f32 feature_scale, i32 seed_offset);
        Node fbm(Node source, i32 octaves, f32 gain = 0.5f, f32 lacunarity = 2.0f);
        // Sharp crests where the source crosses zero: mountain ranges.
        Node ridged(Node source, i32 octaves, f32 gain = 0.5f, f32 lacunarity = 2.0f);
        // Displaces the source's input by up to `amplitude` metres, along a
        // gradient noise of `feature_scale`.
        Node warp(Node source, f32 amplitude, f32 feature_scale, i32 seed_offset);
        Node add(Node a, Node b);
        Node multiply(Node a, Node b);
        Node scale(Node a, f32 factor);
        // Linear from [from_min, from_max] to [to_min, to_max], clamped to
        // the latter when `clamp`.
        Node remap(Node source, f32 from_min, f32 from_max, f32 to_min, f32 to_max, bool clamp);
        // source ^ exponent; the source must not go negative.
        Node power(Node source, f32 exponent);

        string encode(Node root) const;

    private:
        struct Nodes;
        std::unique_ptr<Nodes> nodes_;
    };
}
