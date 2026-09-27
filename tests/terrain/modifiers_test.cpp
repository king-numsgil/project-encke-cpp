#include "core/pch.hpp"

#include "terrain/modifiers.hpp"
#include "terrain/planet.hpp"
#include "terrain/terrain_field.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace encke::terrain
{
    namespace
    {
        constexpr i32 kSeed = 1337;

        // Off every axis, on gentle ground.
        f64vec3 const kDirection = glm::normalize(f64vec3{0.1184, 0.9894, 0.0836});

        struct Site
        {
            BodyTerrain plain;
            BodyTerrain flattened;
            Flatten     flatten;
            f64vec3     across{0.0};   // unit, in the floor's plane
        };

        // A floor 3 m above the ground in kDirection, so it cuts and fills
        // over the detail's bumps.
        Site make_site()
        {
            Site site{.plain = example_planet(kSeed), .flattened = {}, .flatten = {}, .across = {}};

            GroundProbe const probe{site.plain, 0};
            f64vec3 const     above = kDirection * (site.plain.radius + height_bound(site.plain) + 100.0);
            optional<f64vec3> ground = probe.below(above);
            REQUIRE(ground.has_value());

            site.flatten = Flatten{
                .centre = *ground + kDirection * 3.0,
                .up     = kDirection,
                .radius = 20.0,
                .blend  = 30.0,
                .reach  = 50.0,
            };
            site.flattened = site.plain;
            site.flattened.modifiers.flattens.push_back(site.flatten);
            site.across = glm::normalize(glm::cross(kDirection, f64vec3{1.0, 0.0, 0.0}));
            return site;
        }

        i64vec3 chunk_holding(BodyTerrain const& terrain, f64vec3 const& p, u32 lod)
        {
            f64 const extent = static_cast<f64>(i64{32} << lod);
            return i64vec3{glm::floor(p / terrain.base_voxel_size / extent) * extent};
        }

        bool same_bits(f32 a, f32 b)
        {
            return bit_cast<u32>(a) == bit_cast<u32>(b);
        }
    }

    TEST_CASE("a flatten's floor is its plane across its radius", "[terrain][modifiers]")
    {
        Site const        site = make_site();
        GroundProbe const probe{site.flattened, 0};

        f64vec3 const along = glm::cross(site.flatten.up, site.across);
        for (f64 const x : {-19.0, -8.0, 0.0, 11.0, 19.0})
        {
            for (f64 const y : {-19.0, 0.0, 6.0})
            {
                if (x * x + y * y > 19.0 * 19.0)
                {
                    continue;
                }
                f64vec3 const on   = site.flatten.centre + site.across * x + along * y;
                optional<f64vec3> const hit = probe.hit(on + site.flatten.up * 30.0, -site.flatten.up);
                REQUIRE(hit.has_value());
                f64 const height = glm::dot(*hit - site.flatten.centre, site.flatten.up);
                CAPTURE(x, y, height);
                CHECK(std::abs(height) < 1e-3);
            }
        }
    }

    TEST_CASE("a flatten changes nothing past its bound, to the bit", "[terrain][modifiers]")
    {
        Site const     site = make_site();
        TerrainSampler plain{site.plain};
        TerrainSampler flattened{site.flattened};

        // A LOD 0 chunk straddling the blend's outer edge.
        f64vec3 const edge   = site.flatten.centre + site.across * (site.flatten.radius + site.flatten.blend);
        ChunkRequest  request = plain.chunk(chunk_holding(site.plain, edge, 0), 0);

        vector<f32> a(request.sample_count());
        vector<f32> b(request.sample_count());
        plain.sample_chunk(request, a);
        flattened.sample_chunk(request, b);

        u32 const axis    = request.samples_per_axis();
        u32       outside = 0;
        u32       changed = 0;
        for (u32 k = 0; k < axis; ++k)
        {
            for (u32 j = 0; j < axis; ++j)
            {
                for (u32 i = 0; i < axis; ++i)
                {
                    i64vec3 const grid = request.origin + i64vec3{i, j, k} - i64vec3{2};
                    f64vec3 const p    = f64vec3{grid} * site.plain.base_voxel_size;
                    size_t const  n    = (static_cast<size_t>(k) * axis + j) * axis + i;
                    if (glm::length(p - site.flatten.centre) >= site.flatten.bound())
                    {
                        ++outside;
                        CHECK(same_bits(a[n], b[n]));
                    }
                    changed += same_bits(a[n], b[n]) ? 0u : 1u;
                }
            }
        }
        CAPTURE(outside, changed);
        CHECK(changed > 0);
    }

    TEST_CASE("a flattened chunk's samples do not depend on the box they are taken in", "[terrain][modifiers]")
    {
        Site const     site = make_site();
        TerrainSampler sampler{site.flattened};

        // Straddling the radius, where the weight is between 0 and 1, and at
        // LOD 2, whose boxes each sample alone would be too far apart to
        // share a bound test.
        for (u32 const lod : {0u, 2u})
        {
            f64vec3 const edge    = site.flatten.centre + site.across * (site.flatten.radius + 0.5 * site.flatten.blend);
            ChunkRequest  request = sampler.chunk(chunk_holding(site.flattened, edge, lod), lod);
            vector<f32>   whole(request.sample_count());
            sampler.sample_chunk(request, whole);

            u32 const axis   = request.samples_per_axis();
            i64 const stride = i64{1} << lod;
            u32       compared = 0;
            for (u32 k = 0; k < axis; k += 5)
            {
                for (u32 j = 0; j < axis; j += 5)
                {
                    for (u32 i = 0; i < axis; i += 5)
                    {
                        i64vec3 const grid = request.origin + (i64vec3{i, j, k} - i64vec3{2}) * stride;
                        array<f32, 1> one{};
                        sampler.sample_grid(grid, stride, u32vec3{1}, request.detail_octaves, one);
                        size_t const n = (static_cast<size_t>(k) * axis + j) * axis + i;
                        CAPTURE(lod, i, j, k);
                        CHECK(same_bits(whole[n], one[0]));
                        ++compared;
                    }
                }
            }
            CHECK(compared > 0);
        }
    }
}
