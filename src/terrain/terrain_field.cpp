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

        // Rounds toward negative infinity; `divisor` is positive.
        i64 floor_div(i64 value, i64 divisor)
        {
            i64 const quotient = value / divisor;
            return value % divisor < 0 ? quotient - 1 : quotient;
        }

        size_t flat(u32vec3 const& count, u32 i, u32 j, u32 k)
        {
            return (static_cast<size_t>(k) * count.y + j) * count.x + i;
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
        for (Octave const& octave : detail_.octaves())
        {
            anchor_blocks_.push_back(anchor_block(octave, terrain_.base_voxel_size));
        }
    }

    ChunkRequest TerrainSampler::chunk(i64vec3 const& origin, u32 lod) const
    {
        return ChunkRequest{
            .origin         = origin,
            .lod            = lod,
            .cells          = 32,
            .detail_octaves = octaves_for_voxel(octaves(), terrain_.voxel_size(lod)),
            .coarse_octaves = octaves_for_voxel(octaves(), terrain_.voxel_size(lod + 1)),
        };
    }

    void TerrainSampler::sample_chunk(ChunkRequest const& request, span<f32> out, CoarseSamples const* coarse)
    {
        i64 const stride = i64{1} << request.lod;
        u32 const axis   = request.samples_per_axis();

        GridBox const box{
            .first  = request.origin - i64vec3{2 * stride},
            .stride = stride,
            .count  = u32vec3{axis},
        };

        if (coarse != nullptr)
        {
            partial_.resize(box.size());
        }
        span<f32> const snapshot = coarse != nullptr ? span<f32>{partial_} : span<f32>{};

        LayerTiming const own = evaluate_box(box, request.detail_octaves, request.coarse_octaves,
                                             snapshot, out.first(box.size()));
        timing_ = LayerTiming{.macro = own.macro, .detail = own.detail};

        if (coarse == nullptr)
        {
            return;
        }

        Clock::time_point const start = Clock::now();

        // The parent's grid points from one parent voxel before the origin
        // to one past the far side, `m` a side. All but the last layer on
        // each axis are chunk samples; that last layer, past the chunk's far
        // faces, only the gradients need.
        u32 const     m     = request.cells / 2 + 3;
        u32vec3 const grid{m};
        i64 const     coarse_stride = 2 * stride;
        i64vec3 const grid_first    = request.origin - i64vec3{coarse_stride};
        coarse_grid_.resize(static_cast<size_t>(m) * m * m);

        // The snapshot taken on the way to the full sum. Parent point a is
        // at offset 2a - 2 from the origin: sample 2a.
        for (u32 c = 0; c + 1 < m; ++c)
        {
            for (u32 b = 0; b + 1 < m; ++b)
            {
                for (u32 a = 0; a + 1 < m; ++a)
                {
                    coarse_grid_[flat(grid, a, b, c)] = partial_[flat(box.count, 2 * a, 2 * b, 2 * c)];
                }
            }
        }

        // The far layer, face by face, evaluated as the parent would.
        auto const ring = [&](u32vec3 const& low, u32vec3 const& high) {
            GridBox const face{
                .first  = grid_first + i64vec3{low} * coarse_stride,
                .stride = coarse_stride,
                .count  = high - low + u32vec3{1},
            };
            ring_.resize(face.size());
            evaluate_box(face, request.coarse_octaves, 0, {}, ring_);

            for (u32 k = 0; k < face.count.z; ++k)
            {
                for (u32 j = 0; j < face.count.y; ++j)
                {
                    for (u32 i = 0; i < face.count.x; ++i)
                    {
                        coarse_grid_[flat(grid, low.x + i, low.y + j, low.z + k)] = ring_[flat(face.count, i, j, k)];
                    }
                }
            }
        };
        u32 const last = m - 1;
        u32 const deep = m - 2;
        ring(u32vec3{last, 0, 0}, u32vec3{last, last, last});
        ring(u32vec3{0, last, 0}, u32vec3{deep, last, last});
        ring(u32vec3{0, 0, last}, u32vec3{deep, deep, last});

        f64 const     coarse_voxel = terrain_.voxel_size(request.lod + 1);
        u32vec3 const outputs{request.coarse_per_axis()};
        for (u32 c = 1; c + 1 < m; ++c)
        {
            for (u32 b = 1; b + 1 < m; ++b)
            {
                for (u32 a = 1; a + 1 < m; ++a)
                {
                    auto const at = [&](u32 x, u32 y, u32 z) { return coarse_grid_[flat(grid, x, y, z)]; };

                    size_t const index = flat(outputs, a - 1, b - 1, c - 1);
                    coarse->values[index]    = at(a, b, c);
                    coarse->gradients[index] = f32vec3{
                        central_difference(at(a - 1, b, c), at(a + 1, b, c), coarse_voxel),
                        central_difference(at(a, b - 1, c), at(a, b + 1, c), coarse_voxel),
                        central_difference(at(a, b, c - 1), at(a, b, c + 1), coarse_voxel),
                    };
                }
            }
        }

        timing_.coarse = seconds(start, Clock::now());
    }

    PointSample TerrainSampler::sample_point(f64vec3 const& p, f64 voxel_size, u32 detail_octaves)
    {
        f32 const h = static_cast<f32>(voxel_size);

        // The point, then a voxel either side along each axis.
        array<f32, 7> const x{{0.0f, h, -h, 0.0f, 0.0f, 0.0f, 0.0f}};
        array<f32, 7> const y{{0.0f, 0.0f, 0.0f, h, -h, 0.0f, 0.0f}};
        array<f32, 7> const z{{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, h, -h}};
        array<f32, 7>       values{};

        // Split about the point itself: it need not be a grid point.
        auto const fill_octave = [&](u32 octave) {
            detail_.octave(octave, p, x, y, z, octave_values_);
        };
        timing_ = combine(p, voxel_size, x, y, z, detail_octaves, fill_octave, 0, {}, values);

        return PointSample{
            .value    = values[0],
            .gradient = f32vec3{
                central_difference(values[2], values[1], voxel_size),
                central_difference(values[4], values[3], voxel_size),
                central_difference(values[6], values[5], voxel_size),
            },
        };
    }

    f32 TerrainSampler::sample_value(f64vec3 const& p, f64 voxel_size, u32 detail_octaves)
    {
        array<f32, 1> const zero{{0.0f}};
        array<f32, 1>       value{};

        auto const fill_octave = [&](u32 octave) {
            detail_.octave(octave, p, zero, zero, zero, octave_values_);
        };
        timing_ = combine(p, voxel_size, zero, zero, zero, detail_octaves, fill_octave, 0, {}, value);
        return value[0];
    }

    LayerTiming TerrainSampler::evaluate_box(GridBox const& box, u32 detail_octaves,
                                             u32 snapshot_octaves, span<f32> snapshot,
                                             span<f32> out)
    {
        size_t const count = box.size();
        offset_x_.resize(count);
        offset_y_.resize(count);
        offset_z_.resize(count);

        // Grid offsets in metres from the box's first point: whole numbers of
        // a power-of-two voxel, exact in f32, and exact again when added to
        // the f64 corner. So a grid point's f64 position is the same bits
        // whichever box it is reached from.
        f32 const base = static_cast<f32>(terrain_.base_voxel_size);
        size_t    n    = 0;
        for (u32 k = 0; k < box.count.z; ++k)
        {
            for (u32 j = 0; j < box.count.y; ++j)
            {
                for (u32 i = 0; i < box.count.x; ++i)
                {
                    offset_x_[n] = static_cast<f32>(static_cast<i64>(i) * box.stride) * base;
                    offset_y_[n] = static_cast<f32>(static_cast<i64>(j) * box.stride) * base;
                    offset_z_[n] = static_cast<f32>(static_cast<i64>(k) * box.stride) * base;
                    ++n;
                }
            }
        }

        f64vec3 const origin = f64vec3{box.first} * terrain_.base_voxel_size;
        f64 const     voxel  = static_cast<f64>(box.stride) * terrain_.base_voxel_size;

        auto const fill_octave = [&](u32 octave) { box_octave(box, octave); };
        return combine(origin, voxel, offset_x_, offset_y_, offset_z_,
                       detail_octaves, fill_octave, snapshot_octaves, snapshot, out);
    }

    void TerrainSampler::axis_runs(i64 first, i64 stride, u32 count, i64 block, vector<Run>& runs) const
    {
        runs.clear();
        for (u32 i = 0; i < count; ++i)
        {
            i64 const b = floor_div(first + static_cast<i64>(i) * stride, block);
            if (runs.empty() || runs.back().block != b)
            {
                runs.push_back(Run{.block = b, .first = i, .last = i});
            }
            else
            {
                runs.back().last = i;
            }
        }
    }

    void TerrainSampler::box_octave(GridBox const& box, u32 octave)
    {
        i64 const block = anchor_blocks_[octave];
        axis_runs(box.first.x, box.stride, box.count.x, block, runs_x_);
        axis_runs(box.first.y, box.stride, box.count.y, block, runs_y_);
        axis_runs(box.first.z, box.stride, box.count.z, block, runs_z_);

        size_t const count = box.size();
        block_x_.resize(count);
        block_y_.resize(count);
        block_z_.resize(count);
        block_values_.resize(count);
        block_index_.resize(count);
        octave_values_.resize(count);

        f32 const base = static_cast<f32>(terrain_.base_voxel_size);

        for (Run const& rz : runs_z_)
        {
            for (Run const& ry : runs_y_)
            {
                for (Run const& rx : runs_x_)
                {
                    i64vec3 const anchor = i64vec3{rx.block, ry.block, rz.block} * block;

                    // Each sample's offset from its block's corner: an integer
                    // under the block edge, times a power of two, so exact.
                    size_t n = 0;
                    for (u32 k = rz.first; k <= rz.last; ++k)
                    {
                        for (u32 j = ry.first; j <= ry.last; ++j)
                        {
                            for (u32 i = rx.first; i <= rx.last; ++i)
                            {
                                i64vec3 const point = box.first + i64vec3{i, j, k} * box.stride;
                                i64vec3 const local = point - anchor;
                                block_x_[n]     = static_cast<f32>(local.x) * base;
                                block_y_[n]     = static_cast<f32>(local.y) * base;
                                block_z_[n]     = static_cast<f32>(local.z) * base;
                                block_index_[n] = flat(box.count, i, j, k);
                                ++n;
                            }
                        }
                    }

                    span<f32> const values = span<f32>{block_values_}.first(n);
                    detail_.octave(octave, f64vec3{anchor} * terrain_.base_voxel_size,
                                   span<f32 const>{block_x_}.first(n),
                                   span<f32 const>{block_y_}.first(n),
                                   span<f32 const>{block_z_}.first(n),
                                   values);

                    for (size_t s = 0; s < n; ++s)
                    {
                        octave_values_[block_index_[s]] = values[s];
                    }
                }
            }
        }
    }

    LayerTiming TerrainSampler::combine(f64vec3 const& origin, f64 voxel_size,
                                        span<f32 const> x, span<f32 const> y, span<f32 const> z,
                                        u32 detail_octaves, function<void(u32)> const& fill_octave,
                                        u32 snapshot_octaves, span<f32> snapshot,
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

        auto const compose = [&](span<f32> into) {
            for (size_t i = 0; i < count; ++i)
            {
                // |p| - radius in f64: both are millions of metres and only
                // their difference is small enough for f32.
                f64vec3 const point  = origin + f64vec3{x[i], y[i], z[i]};
                f64 const     above  = glm::length(point) - terrain_.radius;
                f64 const     ground = static_cast<f64>(height[i]) + static_cast<f64>(amplitude[i]) * static_cast<f64>(detail_sum_[i]);
                into[i] = static_cast<f32>(above - ground);
            }
        };

        u32 const octaves     = std::min(detail_octaves, static_cast<u32>(detail_.octaves().size()));
        u32 const snapshot_at = std::min(snapshot_octaves, octaves);
        bool const snapshotting = !snapshot.empty();

        if (snapshotting && snapshot_at == 0)
        {
            compose(snapshot);
        }

        for (u32 octave = 0; octave < octaves; ++octave)
        {
            fill_octave(octave);

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

            // The sum so far is exactly what a coarser chunk, stopping here,
            // would compose.
            if (snapshotting && octave + 1 == snapshot_at)
            {
                compose(snapshot);
            }
        }

        compose(out);
        Clock::time_point const detail_done = Clock::now();

        return LayerTiming{
            .macro  = seconds(start, macro_done),
            .detail = seconds(macro_done, detail_done),
        };
    }
}
