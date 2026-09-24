#include "core/pch.hpp"

#include "terrain/terrain_field.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace encke::terrain
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        f64 seconds(Clock::time_point from, Clock::time_point to)
        {
            return std::chrono::duration<f64>(to - from).count();
        }
    }

    BodyTerrain example_planet(i32 seed)
    {
        BodyTerrain terrain{
            .radius          = 6'371'000.0,
            .seed            = seed,
            .base_voxel_size = 0.25,
        };

        auto const channel = [&](MacroChannel which) -> MacroChannelSpec& {
            return terrain.macro.channels[static_cast<size_t>(which)];
        };

        // Continents and ranges: a few kilometres of relief over hundreds.
        channel(MacroChannel::Height) = MacroChannelSpec{
            .graph = encode_fbm_graph(400'000.0f, 6, 0.5f, 2.0f), .scale = 2'500.0f, .bias = 0.0f};
        // Rough country and smooth plains, 20 to 100 m of detail.
        channel(MacroChannel::DetailAmplitude) = MacroChannelSpec{
            .graph = encode_fbm_graph(50'000.0f, 3, 0.5f, 2.0f), .scale = 40.0f, .bias = 60.0f};
        channel(MacroChannel::Ridge) = MacroChannelSpec{
            .graph = encode_fbm_graph(80'000.0f, 3, 0.5f, 2.0f), .scale = 0.5f, .bias = 0.5f};
        channel(MacroChannel::Persistence) = MacroChannelSpec{
            .graph = encode_fbm_graph(30'000.0f, 2, 0.5f, 2.0f), .scale = 0.1f, .bias = 0.5f};

        return terrain;
    }

    TerrainSampler::TerrainSampler(BodyTerrain const& terrain)
        : terrain_(terrain)
        , detail_(terrain.detail, terrain.seed)
        , macro_(terrain.macro, terrain.seed)
    {
    }

    ChunkRequest TerrainSampler::chunk(f64vec3 const& origin, u32 lod) const
    {
        f64 const voxel = terrain_.voxel_size(lod);
        return ChunkRequest{
            .origin         = origin,
            .voxel_size     = voxel,
            .cells          = 32,
            .detail_octaves = octaves_for_voxel(octaves(), voxel),
        };
    }

    void TerrainSampler::sample_chunk(ChunkRequest const& request, span<f32> out)
    {
        u32 const    axis  = request.samples_per_axis();
        size_t const count = request.sample_count();

        offset_x_.resize(count);
        offset_y_.resize(count);
        offset_z_.resize(count);

        // Whole multiples of a power-of-two voxel: exact in f32, and exact
        // again when added to the f64 origin, so a point query at the same
        // spot computes the very same f64 position.
        f32 const voxel = static_cast<f32>(request.voxel_size);
        size_t    n     = 0;
        for (u32 k = 0; k < axis; ++k)
        {
            for (u32 j = 0; j < axis; ++j)
            {
                for (u32 i = 0; i < axis; ++i)
                {
                    offset_x_[n] = (static_cast<f32>(i) - 1.0f) * voxel;
                    offset_y_[n] = (static_cast<f32>(j) - 1.0f) * voxel;
                    offset_z_[n] = (static_cast<f32>(k) - 1.0f) * voxel;
                    ++n;
                }
            }
        }

        evaluate(request.origin, request.voxel_size, request.detail_octaves,
                 offset_x_, offset_y_, offset_z_, out.first(count));
    }

    PointSample TerrainSampler::sample_point(f64vec3 const& p, f64 voxel_size, u32 detail_octaves)
    {
        f32 const h = static_cast<f32>(voxel_size);

        // The point, then a voxel either side along each axis.
        array<f32, 7> const x{{0.0f, h, -h, 0.0f, 0.0f, 0.0f, 0.0f}};
        array<f32, 7> const y{{0.0f, 0.0f, 0.0f, h, -h, 0.0f, 0.0f}};
        array<f32, 7> const z{{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, h, -h}};
        array<f32, 7>       values{};

        evaluate(p, voxel_size, detail_octaves, x, y, z, values);

        f32 const inverse = 1.0f / (2.0f * h);
        return PointSample{
            .value    = values[0],
            .gradient = f32vec3{values[1] - values[2], values[3] - values[4], values[5] - values[6]} * inverse,
        };
    }

    void TerrainSampler::evaluate(f64vec3 const& origin, f64 voxel_size, u32 detail_octaves,
                                  span<f32 const> x, span<f32 const> y, span<f32 const> z,
                                  span<f32> out)
    {
        size_t const count = out.size();

        Clock::time_point const start = Clock::now();
        macro_.sample(origin, voxel_size, x, y, z, macro_values_);
        Clock::time_point const macro_done = Clock::now();

        span<f32 const> const height      = macro_values_[MacroChannel::Height];
        span<f32 const> const amplitude   = macro_values_[MacroChannel::DetailAmplitude];
        span<f32 const> const ridge       = macro_values_[MacroChannel::Ridge];
        span<f32 const> const persistence = macro_values_[MacroChannel::Persistence];

        detail_sum_.assign(count, 0.0f);
        weight_.assign(count, 1.0f);
        octave_values_.resize(count);

        u32 const octaves = std::min(detail_octaves, static_cast<u32>(detail_.octaves().size()));
        for (u32 octave = 0; octave < octaves; ++octave)
        {
            detail_.octave(octave, origin, x, y, z, octave_values_);

            for (size_t i = 0; i < count; ++i)
            {
                // Ridged is 1 - 2|n|, folded into the same [-1, 1] range, so
                // blending toward it keeps the amplitude meaning the same.
                f32 const n      = octave_values_[i];
                f32 const r      = std::clamp(ridge[i], 0.0f, 1.0f);
                f32 const shaped = n + r * ((1.0f - 2.0f * std::abs(n)) - n);

                detail_sum_[i] += weight_[i] * shaped;
                weight_[i]     *= std::clamp(persistence[i], 0.0f, 1.0f);
            }
        }

        for (size_t i = 0; i < count; ++i)
        {
            // |p| - radius in f64: both are millions of metres and only their
            // difference is small enough for f32.
            f64vec3 const point  = origin + f64vec3{x[i], y[i], z[i]};
            f64 const     above  = glm::length(point) - terrain_.radius;
            f64 const     ground = static_cast<f64>(height[i]) + static_cast<f64>(amplitude[i]) * static_cast<f64>(detail_sum_[i]);
            out[i] = static_cast<f32>(above - ground);
        }
        Clock::time_point const detail_done = Clock::now();

        timing_ = LayerTiming{
            .macro  = seconds(start, macro_done),
            .detail = seconds(macro_done, detail_done),
        };
    }
}
