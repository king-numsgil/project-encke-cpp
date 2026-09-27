#pragma once

#include "render/renderer.hpp"

#include <filesystem>
#include <variant>

// A scenario is a JSON file the engine runs instead of waiting for input:
// settings, then a list of steps -- settle, move the camera, freeze the
// terrain, capture -- executed one after another, then quit. It is how
// captures are taken and compared, reproducibly and readably.
namespace encke::scenario
{
    // What the engine can be set to, at startup and by a `set` step. Every
    // field is optional: absent leaves the engine as it is.
    struct Settings
    {
        optional<bool>    ui;            // the overlay; captures want it off
        optional<bool>    taa;
        optional<bool>    geomorph;      // off draws every chunk at its own vertices
        optional<bool>    frame_limit;   // the 60 fps CPU limiter
        optional<Tonemap> tonemap;
        // Pins the exposure the frame is tonemapped at; metering still runs.
        optional<f32>     ev100;
        optional<DebugView> shading;
        // Replaces the set of open debug windows. They draw only with the UI.
        optional<vector<DebugWindow>> windows;
        // Animation's clock, pinned. A scenario always runs pinned, at 0
        // unless this says otherwise.
        optional<f64>     time;

        // Startup only; refused in a `set` step. The UI draws through the
        // sRGB swapchain view as a GPU without the mutable-format extension
        // would.
        optional<bool> srgb_ui;

        // Startup only: the terrain LOD rigid bodies collide with, in place
        // of config::kCollisionLod.
        optional<u32> collision_lod;
    };

    // Waits until nothing is streaming: the terrain shows the leaves the
    // camera wants (unless frozen), every asset has landed, and every rigid
    // body is in the simulation and asleep. At least
    // `min_frames` frames pass first, since a camera move reaches the octree
    // a frame late. Fails the scenario after `timeout_frames`.
    struct Settle
    {
        u32 min_frames     = 10;
        u32 timeout_frames = 36'000;
    };

    // Discards TAA's history, waits one jitter cycle so the image does not
    // depend on the frames before, and writes the frame as a PNG under the
    // run's output folder. It does not settle; put a `settle` before it.
    struct Capture
    {
        string file;
    };

    // Places the camera at `position` looking at `target`, its up toward
    // `up`, all in the test scene's frame: metres from its origin, +Y away
    // from the ground there.
    struct Camera
    {
        array<f64, 3> position{};
        array<f64, 3> target{};
        array<f64, 3> up{{0.0, 1.0, 0.0}};
    };

    // Moves the camera by `velocity`, metres per second in the test scene's
    // frame, at a fixed 60 steps
    // a second for `frames` frames. The octree stays live unless frozen, so
    // what has swapped by the end depends on timing.
    struct Fly
    {
        array<f64, 3> velocity{};
        u32           frames = 60;
    };

    struct Wait
    {
        u32 frames = 1;
    };

    enum class Shape
    {
        Box,
        Sphere,
    };

    // Dynamic bodies, a grid of `count` of them `spacing` metres apart,
    // centred on `position`, in the test scene's frame. A box is
    // `size` metres; a sphere's diameter is size's x. Each is turned by
    // `tilt` degrees about an axis that differs from body to body, the same
    // on every run, so they do not land flat. They wait in the air until the
    // ground under them is built, then fall; `settle` waits for them to
    // sleep.
    struct Spawn
    {
        Shape         shape = Shape::Box;
        array<f64, 3> position{};
        array<f64, 3> size{{0.5, 0.5, 0.5}};
        array<u32, 3> count{{1, 1, 1}};
        array<f64, 3> spacing{{1.0, 1.0, 1.0}};
        f64           tilt = 0.0;
    };

    // Frozen, the octree selects and swaps nothing: what is on screen stays
    // while the camera moves, for looking at seams from where it did not
    // choose its chunks.
    struct FreezeTerrain {};
    struct ThawTerrain {};

    // Ends the scenario and leaves the engine running under the user's
    // control, where the end would otherwise quit.
    struct Interactive {};

    using Step = std::variant<Settle, Capture, Camera, Fly, Wait, FreezeTerrain, ThawTerrain,
                              Interactive, Settings, Spawn>;

    struct Scenario
    {
        string       name;
        Settings     settings;
        vector<Step> steps;
    };

    // Reads and validates a scenario file; logs what is wrong and where.
    optional<Scenario> load(std::filesystem::path const& path);

    // The same, from text; `source` names it in errors.
    optional<Scenario> parse(string_view text, string_view source);

    // One step as a single line of JSON, as a scenario file holds it.
    string to_json(Step const& step);

    // A step's `op`, for the log.
    string_view op_name(Step const& step);
}
