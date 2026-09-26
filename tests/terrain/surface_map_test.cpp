#include "core/pch.hpp"

#include "terrain/surface_map.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace encke::terrain
{
    TEST_CASE("a surface map face's (s, t) and direction are inverses", "[terrain][surface_map]")
    {
        // Short of the edges, where a direction belongs to two faces.
        for (u32 face = 0; face < 6; ++face)
        {
            for (f64 s = -0.95; s <= 0.95; s += 0.1)
            {
                for (f64 t = -0.95; t <= 0.95; t += 0.1)
                {
                    f64vec3 const   direction = face_direction(face, s, t);
                    FacePoint const point     = face_point(direction);
                    CAPTURE(face, s, t);
                    CHECK(glm::length(direction) == Catch::Approx(1.0));
                    CHECK(point.face == face);
                    CHECK(point.st.x == Catch::Approx(s).margin(1e-12));
                    CHECK(point.st.y == Catch::Approx(t).margin(1e-12));
                }
            }
        }
    }

    TEST_CASE("a surface map face's tangent frame follows s and is right-handed", "[terrain][surface_map]")
    {
        // The frame normals are stored in; shaders/lib/surface_map.slang
        // decodes them in the same one.
        for (u32 face = 0; face < 6; ++face)
        {
            f64vec3 const direction = face_direction(face, 0.3, -0.6);
            f64vec3 const along_s   = face_tangent(face, direction);
            f64vec3 const along_t   = glm::cross(direction, along_s);

            // Where s grows, on the face.
            f64vec3 const toward_s = face_direction(face, 0.301, -0.6) - direction;

            CAPTURE(face);
            CHECK(glm::length(along_s) == Catch::Approx(1.0));
            CHECK(glm::dot(along_s, direction) == Catch::Approx(0.0).margin(1e-12));
            CHECK(glm::dot(along_s, toward_s) > 0.0);
            CHECK(glm::length(along_t) == Catch::Approx(1.0));
            CHECK(glm::dot(glm::cross(along_s, along_t), direction) == Catch::Approx(1.0));
        }
    }

    TEST_CASE("a flat body's surface map is radial and level", "[terrain][surface_map]")
    {
        // No graphs: the height is its bias everywhere, and so every normal
        // points straight out, which stores as a half in R and G.
        BodyTerrain terrain{.radius = 40'000.0, .seed = 3, .base_voxel_size = 0.25};
        terrain.macro.channels[static_cast<size_t>(MacroChannel::Height)].bias  = 150.0f;
        terrain.macro.channels[static_cast<size_t>(MacroChannel::Height)].scale = 500.0f;

        SurfaceMapLayout const layout{.face = 16, .border = 2};
        GroundLook             look;
        look.albedo.fill(f32vec3{0.2f});
        look.roughness.fill(0.5f);

        array<SurfaceTile, 6> tiles;
        for (u32 face = 0; face < 6; ++face)
        {
            bake_face(terrain, look, layout, face, tiles[face]);
            size_t const texels = size_t{layout.tile()} * layout.tile();
            REQUIRE(tiles[face].albedo.size() == texels * 4);
            REQUIRE(tiles[face].surface.size() == texels * 4);

            // The bias, with no graph, is the middle of the range, 127.5 of
            // 255, a rounding edge. Roughness 0.5 by weights that sum to one
            // to within their bytes' rounding.
            for (size_t texel = 0; texel < texels; ++texel)
            {
                u8 const* const surface = tiles[face].surface.data() + texel * 4;
                CAPTURE(face, texel);
                CHECK(std::abs(static_cast<int>(surface[0]) - 128) <= 1);
                CHECK(std::abs(static_cast<int>(surface[1]) - 128) <= 1);
                CHECK(std::abs(static_cast<int>(surface[2]) - 128) <= 1);
                CHECK(std::abs(static_cast<int>(surface[3]) - 128) <= 1);
            }
        }

        Pixels albedo;
        Pixels surface;
        assemble_atlas(tiles, layout, albedo, surface);
        CHECK(albedo.width == layout.width());
        CHECK(albedo.height == layout.height());
        // Face 5's last texel is the atlas's last.
        CHECK(std::equal(surface.rgba.end() - 4, surface.rgba.end(), tiles[5].surface.end() - 4));
    }
}
