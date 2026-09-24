#pragma once

#include <memory>

namespace encke::terrain
{
    // The macro layer: continents, range placement, biome masks, large warps.
    // FastNoise2 node graphs authored in its Node Editor, loaded from their
    // encoded strings. A graph has one scalar output, so each channel is its
    // own graph.
    enum class MacroChannel : u32
    {
        Height,            // metres above the body's radius
        DetailAmplitude,   // metres: the detail layer's first-octave amplitude
        Ridge,             // 0 plain fBm, 1 fully ridged
        Persistence,       // amplitude ratio from one detail octave to the next
    };
    inline constexpr size_t kMacroChannelCount = 4;

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

        // Every channel at origin + (x[i], y[i], z[i]), metres.
        void sample(f64vec3 const& origin, f64 voxel_size,
                    span<f32 const> x, span<f32 const> y, span<f32 const> z,
                    MacroValues& out);

    private:
        struct Nodes;

        MacroSpec              spec_;
        i32                    seed_ = 0;
        std::unique_ptr<Nodes> nodes_;
        vector<f32>            node_x_;
        vector<f32>            node_y_;
        vector<f32>            node_z_;
        vector<f32>            node_values_;
        vector<i64vec3>        cells_;
        vector<f64vec3>        weights_;
    };

    // An FBm-over-Simplex graph, encoded as the Node Editor would: for
    // defaults and tests until bodies carry authored graphs.
    string encode_fbm_graph(f32 feature_scale, i32 octaves, f32 gain, f32 lacunarity);
}
