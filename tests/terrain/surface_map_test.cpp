#include "core/pch.hpp"

#include "terrain/surface_map.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

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

    namespace
    {
        // The example planet shrunk to a map that bakes in a moment.
        SurfaceMapLayout const kSmall{.face = 24, .border = 2};

        GroundLook small_look()
        {
            GroundLook look;
            for (size_t set = 0; set < kGroundSetCount; ++set)
            {
                look.albedo[set]    = f32vec3{0.1f * static_cast<f32>(set + 1)};
                look.roughness[set] = 0.5f + 0.1f * static_cast<f32>(set);
            }
            return look;
        }

        void bake_atlas(BodyTerrain const& terrain, GroundLook const& look, Pixels& albedo, Pixels& surface)
        {
            array<SurfaceTile, 6> tiles;
            for (u32 face = 0; face < 6; ++face)
            {
                bake_face(terrain, look, kSmall, face, tiles[face]);
            }
            assemble_atlas(tiles, kSmall, albedo, surface);
        }
    }

    TEST_CASE("surface map rows baked alone are the face's own rows", "[terrain][surface_map]")
    {
        BodyTerrain const terrain = example_planet(7);
        GroundLook const  look    = small_look();
        size_t const      row     = size_t{kSmall.tile()} * 4;

        for (u32 face = 0; face < 6; ++face)
        {
            SurfaceTile whole;
            bake_face(terrain, look, kSmall, face, whole);

            SurfaceTile part;
            bake_rows(terrain, look, kSmall, face, 5, 3, part);
            REQUIRE(part.albedo.size() == 3 * row);
            CAPTURE(face);
            CHECK(std::equal(part.albedo.begin(), part.albedo.end(), whole.albedo.begin() + 5 * static_cast<std::ptrdiff_t>(row)));
            CHECK(std::equal(part.surface.begin(), part.surface.end(), whole.surface.begin() + 5 * static_cast<std::ptrdiff_t>(row)));
        }
    }

    TEST_CASE("a surface map's key and canaries follow what the bake is given", "[terrain][surface_map]")
    {
        BodyTerrain const terrain = example_planet(7);
        GroundLook const  look    = small_look();
        u64 const         key     = surface_map_key(terrain, look, kSmall);
        vector<u8> const  canaries = bake_canaries(terrain, look, kSmall);

        CHECK(key == surface_map_key(example_planet(7), small_look(), kSmall));
        CHECK(canaries == bake_canaries(example_planet(7), small_look(), kSmall));

        CHECK(key != surface_map_key(example_planet(8), look, kSmall));
        CHECK(key != surface_map_key(terrain, look, SurfaceMapLayout{.face = 24, .border = 4}));

        GroundLook other = look;
        other.albedo[2].g += 0.01f;
        CHECK(key != surface_map_key(terrain, other, kSmall));

        BodyTerrain higher = terrain;
        higher.macro.channels[static_cast<size_t>(MacroChannel::Height)].scale = 5'001.0f;
        CHECK(key != surface_map_key(higher, look, kSmall));

        // The detail layer's channels are not in the map, so not in its key.
        BodyTerrain rougher = terrain;
        rougher.macro.channels[static_cast<size_t>(MacroChannel::DetailAmplitude)].scale = 50.0f;
        CHECK(key == surface_map_key(rougher, look, kSmall));

        // The canaries hold values no byte of the map shows a metre's change
        // in: a change to the arithmetic, which is what they are for.
        BodyTerrain shifted = terrain;
        shifted.macro.channels[static_cast<size_t>(MacroChannel::Height)].bias += 1.0f;
        CHECK(canaries != bake_canaries(shifted, look, kSmall));
    }

    TEST_CASE("a cached surface map loads only as the map it was saved as", "[terrain][surface_map]")
    {
        BodyTerrain const terrain  = example_planet(7);
        GroundLook const  look     = small_look();
        u64 const         key      = surface_map_key(terrain, look, kSmall);
        vector<u8> const  canaries = bake_canaries(terrain, look, kSmall);

        Pixels albedo;
        Pixels surface;
        bake_atlas(terrain, look, albedo, surface);

        std::filesystem::path const directory = std::filesystem::temp_directory_path() / "encke_surface_map_test";
        std::filesystem::remove_all(directory);
        std::filesystem::path const file = surface_map_file(directory, key);

        Pixels loaded_albedo;
        Pixels loaded_surface;
        CHECK_FALSE(load_surface_map(file, key, canaries, kSmall, loaded_albedo, loaded_surface));

        REQUIRE(save_surface_map(file, key, canaries, kSmall, albedo, surface));
        REQUIRE(load_surface_map(file, key, canaries, kSmall, loaded_albedo, loaded_surface));
        CHECK(loaded_albedo.width == albedo.width);
        CHECK(loaded_albedo.height == albedo.height);
        CHECK(loaded_albedo.rgba == albedo.rgba);
        CHECK(loaded_surface.rgba == surface.rgba);

        // Another key, another layout, and canaries one value off, as after
        // a change to the terrain code.
        Pixels none;
        CHECK_FALSE(load_surface_map(file, key + 1, canaries, kSmall, none, none));
        CHECK_FALSE(load_surface_map(file, key, canaries, SurfaceMapLayout{.face = 24, .border = 4}, none, none));
        vector<u8> changed = canaries;
        changed[changed.size() / 2] ^= 1;
        CHECK_FALSE(load_surface_map(file, key, changed, kSmall, none, none));
        CHECK(none.rgba.empty());

        // A byte of the atlas flipped on disk, and a file cut short.
        auto const size = std::filesystem::file_size(file);
        {
            std::fstream damage{file, std::ios::binary | std::ios::in | std::ios::out};
            damage.seekg(static_cast<std::streamoff>(size) - 9);
            char const byte = static_cast<char>(damage.get());
            damage.seekp(static_cast<std::streamoff>(size) - 9);
            damage.put(static_cast<char>(byte ^ 1));
        }
        CHECK_FALSE(load_surface_map(file, key, canaries, kSmall, none, none));

        REQUIRE(save_surface_map(file, key, canaries, kSmall, albedo, surface));
        REQUIRE(load_surface_map(file, key, canaries, kSmall, loaded_albedo, loaded_surface));
        std::filesystem::resize_file(file, size - 16);
        CHECK_FALSE(load_surface_map(file, key, canaries, kSmall, none, none));

        std::filesystem::remove_all(directory);
    }
}
