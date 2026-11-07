#include "core/pch.hpp"

#include "app.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "input/sdl_input.hpp"
#include "physics/components.hpp"
#include "platform/cpu.hpp"
#include "render/config.hpp"
#include "render/pixels.hpp"
#include "ui/image_window.hpp"
#include "ui/input_window.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <imgui.h>

namespace encke
{
    namespace
    {
        constexpr char kTitle[] = "encke";
        constexpr i32  kWidth   = 1280;
        constexpr i32  kHeight  = 720;

        // Keys 1 and 2 pick how the frame is shaded; the keys after them
        // toggle one visualisation window each.
        constexpr u32 kShadingModeCount = 2;
        constexpr u32 kDebugKeyCount    = kShadingModeCount + kDebugWindowCount;
        static_assert(kDebugKeyCount == 6, "one debug_<n> action per key, in App::Actions and the bindings");

        // The motion gain slider's range. Motion is per-frame displacement:
        // the top end suits an uncapped couple of thousand frames a second,
        // the bottom a frame-limited 60.
        constexpr f32 kMotionGainMin = 10.0f;
        constexpr f32 kMotionGainMax = 100000.0f;

        // Longest frame the camera and exposure will integrate over at once.
        constexpr f64 kMaxFrameSeconds = 0.1;

        // The frame limiter's rate, when it is on.
        constexpr f64 kFrameLimitHz = 60.0;

        // Sub-frame event stamping: the most often the limiter's idle time
        // pumps events, so input is stamped finer than the frame. A ceiling, not a rate: a frame
        // with less idle time pumps less, down to the frame's own poll.
        constexpr f64 kIdlePumpHz = 120.0;

        // Where each debug window first opens: beside the stats window and
        // clear of one another at the default 1280x720, so opening them all
        // does not stack them.
        f32vec2 debug_window_position(u32 index)
        {
            constexpr f32 kLeft   = 470.0f;
            constexpr f32 kTop    = 10.0f;
            constexpr f32 kStepX  = 410.0f;
            constexpr f32 kStepY  = 270.0f;

            return f32vec2{kLeft + kStepX * static_cast<f32>(index % 2u),
                           kTop + kStepY * static_cast<f32>(index / 2u)};
        }

        char const* debug_view_name(DebugView view)
        {
            switch (view)
            {
            case DebugView::Lit:        return "lit (clustered)";
            case DebugView::BruteForce: return "lit (brute force)";
            }
            return "unknown";
        }

        char const* debug_window_title(DebugWindow window)
        {
            switch (window)
            {
            case DebugWindow::ClusterHeat: return "Lights per cluster";
            case DebugWindow::Normals:     return "Normals";
            case DebugWindow::Motion:      return "Motion vectors";
            case DebugWindow::Cascades:    return "Shadow cascades";
            }
            return "unknown";
        }

        char const* tonemap_name(Tonemap tonemap)
        {
            switch (tonemap)
            {
            case Tonemap::Aces:       return "ACES (Narkowicz fit)";
            case Tonemap::AgX:        return "AgX";
            case Tonemap::PbrNeutral: return "PBR Neutral";
            }
            return "unknown";
        }

        char const* wireframe_name(Wireframe wireframe)
        {
            switch (wireframe)
            {
            case Wireframe::Off:     return "off";
            case Wireframe::Overlay: return "over the image";
            case Wireframe::Only:    return "alone";
            }
            return "unknown";
        }

        // Collision shapes by what they belong to: ground chunks, the
        // scene's props, bodies waiting for their ground, awake bodies,
        // sleeping ones.
        u32 collision_colour(physics::PhysicsWorld::ShapeKind kind)
        {
            using Kind = physics::PhysicsWorld::ShapeKind;
            switch (kind)
            {
            case Kind::Ground: return rgb(f32vec3{1.0f, 0.55f, 0.1f});
            case Kind::Static: return rgb(f32vec3{0.9f, 0.9f, 0.9f});
            case Kind::Held:   return rgb(f32vec3{1.0f, 0.15f, 0.15f});
            case Kind::Awake:  return rgb(f32vec3{0.3f, 1.0f, 0.3f});
            case Kind::Asleep: return rgb(f32vec3{0.3f, 0.6f, 1.0f});
            }
            return rgb(f32vec3{1.0f});
        }

        f64vec3 to_vec(array<f64, 3> const& a)
        {
            return f64vec3{a[0], a[1], a[2]};
        }

        // To `places` decimals, so a pose copied with F3 reads as numbers a
        // person would write, and writes back out as the same digits.
        array<f64, 3> rounded(f64vec3 const& v, f64 places)
        {
            f64 const scale = std::pow(10.0, places);
            return {{std::round(v.x * scale) / scale, std::round(v.y * scale) / scale,
                     std::round(v.z * scale) / scale}};
        }

        // The frame rate a scenario's flying and fixed steps assume.
        constexpr f64 kScenarioStepHz = 60.0;
    }

    App::~App()
    {
        // Members are destroyed after this body runs, so this is the last
        // chance to be sure no command buffer is still executing.
        device_.wait_idle();
    }

    bool App::init(AppOptions const& options)
    {
        log::init();
        log::info("allocator: %s", memory::backend_name());

        // First, so a mistake in a file stops the run before a window opens.
        for (std::filesystem::path const& file : options.scenarios)
        {
            optional<scenario::Scenario> loaded = scenario::load(file);
            if (!loaded.has_value())
            {
                return false;
            }
            std::filesystem::path const output =
                !options.output.has_value()       ? file.parent_path() / "out" / loaded->name
                : options.scenarios.size() == 1 ? *options.output
                                                  : *options.output / loaded->name;
            log::info("scenario \"%s\": %zu steps, captures to %s", loaded->name.c_str(), loaded->steps.size(),
                      output.string().c_str());
            scenarios_.push_back(std::move(*loaded));
            scenario_outputs_.push_back(output);
        }

        // Several share one start, so what is chosen at startup must agree,
        // and only the last may hand over to the user.
        for (size_t index = 1; index < scenarios_.size(); ++index)
        {
            scenario::Settings const& first = scenarios_.front().settings;
            scenario::Settings const& other = scenarios_[index].settings;
            if (other.srgb_ui.value_or(false) != first.srgb_ui.value_or(false) ||
                other.collision_lod != first.collision_lod)
            {
                log::error("scenario \"%s\": srgb_ui and collision_lod differ from \"%s\"'s; run it on its own",
                           scenarios_[index].name.c_str(), scenarios_.front().name.c_str());
                return false;
            }
        }
        for (size_t index = 0; index + 1 < scenarios_.size(); ++index)
        {
            for (scenario::Step const& step : scenarios_[index].steps)
            {
                if (std::holds_alternative<scenario::Interactive>(step))
                {
                    log::error("scenario \"%s\" hands over to the user, so it must run last",
                               scenarios_[index].name.c_str());
                    return false;
                }
            }
        }
        bool const srgb_ui = !scenarios_.empty() && scenarios_.front().settings.srgb_ui.value_or(false);

        // Before the window too: a binding file with a mistake stops the run.
        if (!load_input())
        {
            return false;
        }

        CpuInfo const cpu = query_cpu();
        log::info("cpu: %u physical cores, %u logical processors", cpu.physical_cores,
                  cpu.logical_processors);

        // Must precede SDL_Init, which window_.init performs.
        if (!memory::install_sdl_allocator())
        {
            return false;
        }

        if (!window_.init(kTitle, kWidth, kHeight))
        {
            return false;
        }

        if (!vulkan_.init(window_) || !device_.init(vulkan_, srgb_ui))
        {
            return false;
        }

        if (!allocator_.init(vulkan_, device_))
        {
            return false;
        }

        if (!swapchain_.init(vulkan_, device_, window_.pixel_size()))
        {
            return false;
        }

        if (!renderer_.init(allocator_, device_, swapchain_))
        {
            return false;
        }

        if (!ui_.init(window_, allocator_, device_, renderer_.bindless(), swapchain_,
                      Renderer::kFramesInFlight))
        {
            return false;
        }
        // The input system first, so a joystick that has gone is let go of
        // by its identity before the device list forgets it. Typing into the
        // UI presses nothing.
        window_.set_event_hook([this](SDL_Event const& event) {
            ui_.process_event(event);
            input::translate(event, joysticks_, !ui_.wants_text(), input_);
            joysticks_.handle(event);
        });

        // Input logs land beside the executable, as imgui.ini does.
        if (char const* const base = SDL_GetBasePath())
        {
            input_log_directory_ = std::filesystem::path{base};
        }

        assets_.start();
        scene_.build_test_planet(assets_, config::kTerrainFinestLod);

        // One core left for this thread and the driver's.
        u32 const workers = std::max(cpu.physical_cores, 2u) - 1;
        pool_.start(workers, "terrain");
        log::info("worker pool: %u threads", pool_.size());

        // Physics takes the logical processors the physical cores leave: the
        // hyperthreads beside the terrain workers and this thread. Jolt's
        // step is mostly pointer chasing and branches, which shares a core
        // with the terrain's SIMD better than more SIMD would.
        u32 const collision_lod = !scenarios_.empty()
                                      ? scenarios_.front().settings.collision_lod.value_or(config::kCollisionLod)
                                      : config::kCollisionLod;
        physics_.init(cpu.logical_processors > cpu.physical_cores
                          ? cpu.logical_processors - cpu.physical_cores
                          : 0u,
                      collision_lod);
        // The props, from world transforms the first frame has not composed
        // yet; models' parts follow as they spawn.
        propagate_transforms(scene_.registry);
        physics_.add_static_colliders(scene_.registry);

        log::info("scene: %zu entities, %zu renderables, %zu lights",
                  scene_.registry.view<Transform>().size(),
                  scene_.registry.view<Renderable>().size(), scene_.registry.view<Light>().size());

        // A scenario's baseline, before its own settings: animation pinned so
        // runs match, and no overlay since its numbers change every frame.
        // The limiter stays on: physics and flying advance one 60 Hz step a
        // frame so runs match, which is real time only at 60 fps. A scenario
        // nobody watches can turn it off and run faster to the same images.
        renderer_.set_taa(taa_);
        if (!scenarios_.empty())
        {
            start_camera_ = scene_.registry.get<Transform>(scene_.camera);
            auto const ships = scene_.registry.view<physics::ShipSpace const, Transform const>();
            for (entt::entity const entity : ships)
            {
                start_poses_.emplace_back(entity, ships.get<Transform const>(entity));
            }
            scenario_defaults_ = scenario::Settings{
                .ui            = false,
                .taa           = taa_,
                .geomorph      = renderer_.geomorph(),
                .frame_limit   = limit_frames_,
                .tonemap       = renderer_.tonemap(),
                .ev100         = nullopt,
                .shading       = renderer_.debug_view(),
                .windows       = vector<DebugWindow>{},
                .time          = 0.0,
                .wireframe     = renderer_.wireframe(),
                .collision     = false,
                .octree        = false,
                .movement      = Movement::Jetpack,
                .srgb_ui       = nullopt,
                .collision_lod = nullopt,
            };
            start_scenario(0);
        }

        log::info("tonemap: %s (T cycles)", tonemap_name(renderer_.tonemap()));
        log::info("debug view: %s (keys 1-2 shade, 3-%u toggle windows, F1 toggles the UI)",
                  debug_view_name(renderer_.debug_view()), kDebugKeyCount);
        log::info("mouse: held for looking; hold Alt to free it for the UI");
        log::info("jetpack: WASD move, Space/Ctrl up/down, Q/E roll, Shift fast, C slow, wheel scales speed");

        // On foot from the start pose, unless a scenario chose already.
        if (!scenario_.has_value())
        {
            set_movement(Movement::Walk);
        }
        enable_contexts(scenario_.has_value());

        return true;
    }

    bool App::swapchain_matches_window() const
    {
        i32vec2 const    size    = window_.pixel_size();
        VkExtent2D const current = swapchain_.extent();

        return static_cast<u32>(size.x) == current.width &&
               static_cast<u32>(size.y) == current.height;
    }

    bool App::rebuild_swapchain()
    {
        i32vec2 const size = window_.pixel_size();

        // Zero on either axis is not a legal extent. Happens while minimised
        // and transiently mid-drag on some window managers.
        if (size.x == 0 || size.y == 0)
        {
            return false;
        }

        if (!swapchain_.recreate(size))
        {
            return false;
        }

        return renderer_.on_swapchain_changed(swapchain_);
    }

    void App::on_debug_key(u32 key)
    {
        if (key < kShadingModeCount)
        {
            renderer_.set_debug_view(static_cast<DebugView>(key));
            log::info("debug view: %s", debug_view_name(renderer_.debug_view()));
            return;
        }

        u32 const window = key - kShadingModeCount;
        debug_windows_[window] = !debug_windows_[window];

        // Asking for a window is asking to see it, so a hidden UI comes back.
        if (debug_windows_[window])
        {
            show_ui_ = true;
        }
    }

    void App::set_looking(bool looking)
    {
        if (looking == looking_)
        {
            return;
        }

        looking_ = looking;
        window_.set_relative_mouse(looking);
        ui_.set_input_blocked(looking);
    }

    void App::capture_mouse()
    {
        // Held in either movement, as in any first-person game; Alt lets go
        // of it for the UI, and so does losing focus.
        set_looking(focused_ && !input_.active(actions_.free_mouse));
    }

    void App::fly(f64 seconds)
    {
        // Not flown at all, rather than flown by nothing: an update with no
        // input still renormalises the rotation.
        if (!looking_)
        {
            return;
        }

        FlyInput const input{
            .look  = input_.axis2(actions_.look),
            .move  = f64vec3{input_.axis3(actions_.fly_move)},
            .roll  = static_cast<f64>(input_.axis1(actions_.fly_roll)),
            .wheel = input_.axis1(actions_.fly_speed),
            .fast  = input_.active(actions_.fly_fast),
            .slow  = input_.active(actions_.fly_slow),
        };

        if (Transform* const camera = scene_.registry.try_get<Transform>(scene_.camera))
        {
            fly_.update(*camera, input, seconds);
        }
    }

    void App::set_movement(Movement movement)
    {
        movement_ = movement;
        if (movement == Movement::Jetpack)
        {
            physics_.remove_character();
            log::info("movement: jetpack (X walks)");
            return;
        }

        // Standing where the camera is, eyes at its height: the character's
        // up, gravity's, needs the bodies' world transforms, which the first
        // frame has not composed yet.
        propagate_transforms(scene_.registry);
        Transform const& camera = scene_.registry.get<Transform>(scene_.camera);
        f64vec3 const    up     = physics_.up_at(scene_.registry, camera.position);
        physics_.add_character(scene_.registry, camera.position - up * config::kEyeHeight);
        walk_.begin(camera, up);
        log::info("movement: walking (X takes the jetpack); WASD, Shift sprints, Space jumps");
    }

    void App::read_walk_input()
    {
        walk_input_ = WalkInput{
            .look   = input_.axis2(actions_.look),
            .move   = f64vec2{input_.axis2(actions_.walk_move)},
            .sprint = input_.active(actions_.sprint),
            .jump   = input_.active(actions_.jump),
        };
    }

    bool App::load_input()
    {
        std::filesystem::path const defaults = std::filesystem::path{ENCKE_ASSET_DIR} / "input" / "default.json";
        optional<input::Layer> layer = input::load_layer(defaults);
        if (!layer.has_value())
        {
            return false;
        }
        string source = defaults.string();

        // The user's edits, laid over the defaults.
        if (char const* const base = SDL_GetBasePath())
        {
            std::filesystem::path const user = std::filesystem::path{base} / "input.json";
            std::error_code             error;
            if (std::filesystem::exists(user, error))
            {
                optional<input::Layer> const edits = input::load_layer(user);
                if (!edits.has_value())
                {
                    return false;
                }
                input::merge(*layer, *edits);
                source += " + " + user.string();
                log::info("input: %s over the defaults", user.string().c_str());
            }
        }

        optional<input::InputMap> map = input::compile(*layer, source);
        if (!map.has_value())
        {
            return false;
        }
        input_.set_map(std::move(*map));

        using input::ActionType;
        bool ok          = true;
        auto const need  = [&](input::ActionId& id, string_view name, ActionType type) {
            optional<input::ActionId> const found = input_.require(name, type);
            ok                                    = ok && found.has_value();
            id                                    = found.value_or(input::ActionId{});
        };
        auto const want_context = [&](input::ContextId& id, string_view name) {
            optional<input::ContextId> const found = input_.require_context(name);
            ok                                     = ok && found.has_value();
            id                                     = found.value_or(input::ContextId{});
        };

        need(actions_.quit, "quit", ActionType::Bool);
        need(actions_.toggle_ui, "toggle_ui", ActionType::Bool);
        need(actions_.freeze_terrain, "freeze_terrain", ActionType::Bool);
        need(actions_.copy_camera, "copy_camera", ActionType::Bool);
        need(actions_.cycle_wireframe, "cycle_wireframe", ActionType::Bool);
        need(actions_.toggle_collision, "toggle_collision", ActionType::Bool);
        need(actions_.toggle_octree, "toggle_octree", ActionType::Bool);
        need(actions_.toggle_input_window, "toggle_input_window", ActionType::Bool);
        need(actions_.cycle_tonemap, "cycle_tonemap", ActionType::Bool);
        need(actions_.free_mouse, "free_mouse", ActionType::Bool);
        for (size_t key = 0; key < actions_.debug.size(); ++key)
        {
            need(actions_.debug[key], "debug_" + std::to_string(key + 1), ActionType::Bool);
        }
        need(actions_.toggle_jetpack, "toggle_jetpack", ActionType::Bool);
        need(actions_.look, "look", ActionType::Axis2D);
        need(actions_.walk_move, "walk_move", ActionType::Axis2D);
        need(actions_.sprint, "sprint", ActionType::Bool);
        need(actions_.jump, "jump", ActionType::Bool);
        need(actions_.fly_move, "fly_move", ActionType::Axis3D);
        need(actions_.fly_roll, "fly_roll", ActionType::Axis1D);
        need(actions_.fly_speed, "fly_speed", ActionType::Axis1D);
        need(actions_.fly_fast, "fly_fast", ActionType::Bool);
        need(actions_.fly_slow, "fly_slow", ActionType::Bool);

        want_context(contexts_.engine, "engine");
        want_context(contexts_.debug, "debug");
        want_context(contexts_.on_foot, "on_foot");
        want_context(contexts_.jetpack, "jetpack");

        if (ok)
        {
            log::info("input: %zu actions in %zu contexts", input_.map().actions.size(),
                      input_.map().contexts.size());
        }
        return ok;
    }

    void App::enable_contexts(bool scripted)
    {
        input_.set_enabled(contexts_.engine, true);
        input_.set_enabled(contexts_.debug, !scripted);
        input_.set_enabled(contexts_.on_foot, looking_ && movement_ == Movement::Walk);
        input_.set_enabled(contexts_.jetpack, looking_ && movement_ == Movement::Jetpack);
    }

    void App::handle_engine_actions()
    {
        if (input_.fired(actions_.toggle_ui))
        {
            show_ui_ = !show_ui_;
        }

        // Asking for the window is asking to see it, as with the debug
        // windows.
        if (input_.fired(actions_.toggle_input_window))
        {
            show_input_window_ = !show_input_window_;
            if (show_input_window_)
            {
                show_ui_ = true;
            }
        }

        if (input_.fired(actions_.freeze_terrain))
        {
            terrain_.set_frozen(!terrain_.frozen());
            frozen_pose_ = terrain_.frozen() ? optional<scenario::Camera>{camera_step()} : nullopt;
            log::info("terrain: %s", terrain_.frozen() ? "frozen" : "thawed");
        }

        // The pose as scenario steps, on the clipboard and in the log.
        // Frozen, they settle where the terrain froze, freeze it and move
        // to the camera, so the octree shows what was on screen.
        if (input_.fired(actions_.copy_camera))
        {
            string const current = scenario::to_json(camera_step());
            string const text =
                frozen_pose_.has_value()
                    ? scenario::to_json(*frozen_pose_) + ",\n" + scenario::to_json(scenario::Settle{}) + ",\n" +
                          scenario::to_json(scenario::FreezeTerrain{}) + ",\n" + current + ","
                    : current + ",";
            SDL_SetClipboardText(text.c_str());
            log::info("camera:\n%s", text.c_str());
        }

        if (input_.fired(actions_.toggle_jetpack))
        {
            set_movement(movement_ == Movement::Walk ? Movement::Jetpack : Movement::Walk);
        }

        if (input_.fired(actions_.cycle_wireframe))
        {
            auto const next =
                static_cast<Wireframe>((static_cast<u32>(renderer_.wireframe()) + 1u) % kWireframeCount);
            renderer_.set_wireframe(next);
            log::info("wireframe: %s", wireframe_name(next));
        }
        if (input_.fired(actions_.toggle_collision))
        {
            show_collision_ = !show_collision_;
        }
        if (input_.fired(actions_.toggle_octree))
        {
            show_octree_ = !show_octree_;
        }

        if (input_.fired(actions_.cycle_tonemap))
        {
            auto const next = static_cast<Tonemap>((static_cast<u32>(renderer_.tonemap()) + 1u) % kTonemapCount);
            renderer_.set_tonemap(next);
            log::info("tonemap: %s", tonemap_name(next));
        }

        for (u32 key = 0; key < kDebugKeyCount; ++key)
        {
            if (input_.fired(actions_.debug[key]))
            {
                on_debug_key(key);
            }
        }
    }

    void App::draw_ui()
    {
        ui_.begin_frame();

        if (show_ui_)
        {
            AssetWorker const&               asset_worker = assets_.worker();
            array<StatsWindow::Threads, 2> const threads{{
                {.name = "terrain", .loads = pool_.loads(), .queued = pool_.outstanding()},
                {.name   = "assets",
                 .loads  = span<ThreadLoad const>{&asset_worker.load(), 1},
                 .queued = asset_worker.outstanding()},
            }};

            stats_.draw(StatsWindow::Info{
                .device       = device_.properties().deviceName,
                .present_mode = swapchain_.present_mode_name(),
                .extent       = swapchain_.extent(),
                .limit_frames = &limit_frames_,
                .limit_hz     = kFrameLimitHz,
                .taa          = &taa_,
                .threads      = threads,
            });
            renderer_.set_taa(taa_);

            ImGui::SetNextWindowPos(ImVec2{10.0f, 460.0f}, ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Debug draw", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                auto wireframe = static_cast<int>(renderer_.wireframe());
                ImGui::TextUnformatted("Wireframe (F4)");
                ImGui::RadioButton("off", &wireframe, static_cast<int>(Wireframe::Off));
                ImGui::SameLine();
                ImGui::RadioButton("overlay", &wireframe, static_cast<int>(Wireframe::Overlay));
                ImGui::SameLine();
                ImGui::RadioButton("only", &wireframe, static_cast<int>(Wireframe::Only));
                renderer_.set_wireframe(static_cast<Wireframe>(wireframe));

                ImGui::Checkbox("Collision shapes (F5)", &show_collision_);
                ImGui::Checkbox("Octree chunks (F6)", &show_octree_);
                ImGui::Text("%zu lines", debug_lines_.lines().size());
            }
            ImGui::End();

            for (u32 index = 0; index < kDebugWindowCount; ++index)
            {
                auto const window = static_cast<DebugWindow>(index);

                function<void()> controls;
                if (window == DebugWindow::Motion)
                {
                    controls = [this] {
                        ImGui::SliderFloat("gain", &motion_gain_, kMotionGainMin, kMotionGainMax,
                                           "%.0f", ImGuiSliderFlags_Logarithmic);
                    };
                }

                image_window(debug_window_title(window), debug_windows_[index],
                             renderer_.debug_window_image(window), swapchain_.extent(),
                             debug_window_position(index), controls);
            }

            joysticks_.refresh();
            input_window(joysticks_, show_input_window_, input_log_directory_);
        }

        ui_.end_frame();

        // After the UI, which may have closed a window this frame: the
        // renderer must draw exactly the images the draw data samples.
        for (u32 index = 0; index < kDebugWindowCount; ++index)
        {
            renderer_.set_debug_window_open(static_cast<DebugWindow>(index),
                                            show_ui_ && debug_windows_[index]);
        }
        renderer_.set_motion_gain(motion_gain_);
    }

    void App::place_camera(f64vec3 const& eye, f64vec3 const& target, f64vec3 const& up)
    {
        // The test scene's camera is a root, so its local transform is its
        // world one.
        Transform& camera = scene_.registry.get<Transform>(scene_.camera);
        camera.position   = scene_.to_world(eye);
        fly_.aim(camera, scene_.rotation() * (target - eye), scene_.rotation() * up);

        // Walking, the character is moved under the new eye, still, and
        // looks where the camera was aimed.
        if (movement_ == Movement::Walk)
        {
            f64vec3 const ground_up = physics_.up_at(scene_.registry, camera.position);
            physics_.teleport_character(camera.position - ground_up * config::kEyeHeight);
            walk_.begin(camera, ground_up);
        }
    }

    scenario::Camera App::camera_step() const
    {
        // A root, so its local transform is its world one, taken into the
        // scene's frame. The target is 10 m ahead.
        Transform const& camera = scene_.registry.get<Transform>(scene_.camera);
        f64quat const    into   = glm::inverse(scene_.rotation());
        f64vec3 const    eye    = into * (camera.position - scene_.origin());
        f64vec3 const    target = eye + into * forward(camera.rotation) * 10.0;
        f64vec3 const    up     = into * (camera.rotation * f64vec3{0.0, 1.0, 0.0});

        return scenario::Camera{.position = rounded(eye, 4.0), .target = rounded(target, 4.0),
                                .up = rounded(up, 6.0)};
    }

    void App::apply_settings(scenario::Settings const& settings)
    {
        if (settings.ui.has_value())
        {
            show_ui_ = *settings.ui;
        }
        if (settings.taa.has_value())
        {
            taa_ = *settings.taa;
            renderer_.set_taa(taa_);
            log::info("TAA %s", taa_ ? "on" : "off");
        }
        if (settings.geomorph.has_value())
        {
            renderer_.set_geomorph(*settings.geomorph);
            log::info("geomorph %s", *settings.geomorph ? "on" : "off");
        }
        if (settings.frame_limit.has_value())
        {
            limit_frames_ = *settings.frame_limit;
        }
        if (settings.tonemap.has_value())
        {
            renderer_.set_tonemap(*settings.tonemap);
            log::info("tonemap: %s", tonemap_name(*settings.tonemap));
        }
        if (settings.ev100.has_value())
        {
            renderer_.set_fixed_ev100(settings.ev100);
            log::info("exposure pinned to EV100 %.2f", static_cast<f64>(*settings.ev100));
        }
        if (settings.shading.has_value())
        {
            renderer_.set_debug_view(*settings.shading);
            log::info("debug view: %s", debug_view_name(*settings.shading));
        }
        if (settings.windows.has_value())
        {
            debug_windows_.fill(false);
            for (DebugWindow const window : *settings.windows)
            {
                debug_windows_[static_cast<u32>(window)] = true;
            }
        }
        if (settings.time.has_value())
        {
            fixed_time_ = settings.time;
            log::info("animation pinned to t=%.3f s", *fixed_time_);
        }
        if (settings.wireframe.has_value())
        {
            renderer_.set_wireframe(*settings.wireframe);
            log::info("wireframe: %s", wireframe_name(*settings.wireframe));
        }
        if (settings.collision.has_value())
        {
            show_collision_ = *settings.collision;
        }
        if (settings.octree.has_value())
        {
            show_octree_ = *settings.octree;
        }
        if (settings.movement.has_value() && *settings.movement != movement_)
        {
            set_movement(*settings.movement);
        }
    }

    void App::start_scenario(size_t index)
    {
        scenario_index_  = index;
        scenario_        = scenarios_[index];
        scenario_output_ = scenario_outputs_[index];
        step_index_      = 0;
        step_frames_     = 0;
        step_started_    = false;
        scenario_failed_ = false;
        log::info("scenario \"%s\" (%zu of %zu)", scenario_->name.c_str(), index + 1, scenarios_.size());

        fixed_time_ = 0.0;
        show_ui_    = false;
        apply_settings(scenario_->settings);
    }

    void App::reset_for_next_scenario()
    {
        if (movement_ != Movement::Jetpack)
        {
            set_movement(Movement::Jetpack);
        }

        // Physics from nothing, as at startup: the ships back where the
        // scene put them, the spawned bodies gone, then the scene taken in
        // again. The ground is built again around what needs it.
        physics_.rebuild(scene_.registry, pool_);
        for (entt::entity const entity : spawned_)
        {
            if (scene_.registry.valid(entity))
            {
                scene_.registry.destroy(entity);
            }
        }
        spawned_.clear();
        for (auto const& [entity, pose] : start_poses_)
        {
            scene_.registry.get<Transform>(entity) = pose;
        }
        propagate_transforms(scene_.registry);
        physics_.add_static_colliders(scene_.registry);
        terrain_.set_frozen(false);
        frozen_pose_.reset();
        scene_.registry.get<Transform>(scene_.camera) = start_camera_;

        renderer_.set_fixed_ev100(nullopt);
        apply_settings(scenario_defaults_);
    }

    bool App::finish_scenario()
    {
        if (scenario_failed_)
        {
            log::error("scenario \"%s\" failed", scenario_->name.c_str());
            failed_scenarios_.push_back(scenario_->name);
        }
        if (scenario_index_ + 1 >= scenarios_.size())
        {
            return false;
        }
        reset_for_next_scenario();
        start_scenario(scenario_index_ + 1);
        return true;
    }

    void App::collect_debug_lines()
    {
        debug_lines_.clear();

        if (show_collision_)
        {
            physics_.collision_triangles(
                [this](physics::PhysicsWorld::ShapeKind kind, span<f64vec3 const> triangles) {
                    u32 const colour = collision_colour(kind);
                    for (size_t index = 0; index + 2 < triangles.size(); index += 3)
                    {
                        debug_lines_.triangle(triangles[index], triangles[index + 1], triangles[index + 2], colour);
                    }
                });
        }

        // Each chunk's box in its body's frame, coloured by LOD like the
        // wireframe. Shrunk a little, so neighbours' edges stay apart.
        if (show_octree_)
        {
            terrain_.each_drawn([this](entt::entity body, terrain::BodyTerrain const& terrain,
                                       terrain::NodeKey const& key) {
                WorldTransform const* const world = scene_.registry.try_get<WorldTransform>(body);
                if (world == nullptr)
                {
                    return;
                }
                f64 const     edge   = terrain.voxel_size(key.lod) * static_cast<f64>(terrain::ChunkRequest{}.cells);
                f64vec3 const centre = f64vec3{key.origin} * terrain.base_voxel_size + edge * 0.5;
                debug_lines_.box(world->position + world->rotation * centre, world->rotation,
                                 f64vec3{edge * 0.5 * 0.99}, lod_colour(terrain.voxel_size(key.lod)));
            });
        }

        renderer_.set_debug_lines(&debug_lines_);
    }

    void App::spawn(scenario::Spawn const& spawn)
    {
        f64vec3 const count{static_cast<f64>(spawn.count[0]), static_cast<f64>(spawn.count[1]),
                            static_cast<f64>(spawn.count[2])};
        f64vec3 const spacing = to_vec(spawn.spacing);
        f64vec3 const first   = to_vec(spawn.position) - (count - 1.0) * spacing * 0.5;
        f64vec3 const size    = spawn.shape == scenario::Shape::Sphere ? f64vec3{spawn.size[0]}
                                                                        : to_vec(spawn.size);
        f64 const     tilt    = spawn.tilt * 3.14159265358979323846 / 180.0;

        u32 index = 0;
        for (u32 z = 0; z < spawn.count[2]; ++z)
        {
            for (u32 y = 0; y < spawn.count[1]; ++y)
            {
                for (u32 x = 0; x < spawn.count[0]; ++x, ++index)
                {
                    // An axis that turns with the index, the same every run.
                    f64 const     turn = static_cast<f64>(index);
                    f64vec3 const axis = glm::normalize(f64vec3{std::sin(turn * 1.3), 1.0, std::cos(turn * 0.7)});

                    entt::entity const entity = scene_.registry.create();
                    spawned_.push_back(entity);
                    scene_.registry.emplace<Transform>(
                        entity, Transform{
                                    .position = scene_.to_world(first + f64vec3{x, y, z} * spacing),
                                    .rotation = scene_.rotation() * glm::angleAxis(tilt, axis),
                                    .scale    = f32vec3{size},
                                });
                    scene_.registry.emplace<Renderable>(
                        entity, Renderable{
                                    .mesh     = spawn.shape == scenario::Shape::Sphere ? scene_.sphere_mesh
                                                                                       : scene_.cube_mesh,
                                    .material  = scene_.crate_material,
                                    .roughness = 1.0f,
                                    .metallic  = 1.0f,   // the map's, which is none
                                });
                    if (spawn.shape == scenario::Shape::Sphere)
                    {
                        physics_.add_sphere(scene_.registry, entity);
                    }
                    else
                    {
                        physics_.add_box(scene_.registry, entity);
                    }
                }
            }
        }
        log::info("physics: %u bodies spawned, %u in all", index, physics_.bodies());
    }

    App::StepResult App::run_step(scenario::Step const& step)
    {
        return std::visit(
            [this](auto const& s) -> StepResult {
                using S = std::decay_t<decltype(s)>;

                if constexpr (std::is_same_v<S, scenario::Settle>)
                {
                    // Frozen, the octree is not trying to reach what the
                    // camera wants, so it never goes idle; the assets still
                    // must.
                    bool const idle = (terrain_.frozen() || terrain_.idle()) && assets_.idle() &&
                                      renderer_.streaming_idle() && physics_.idle();
                    if (step_frames_ >= s.min_frames && idle)
                    {
                        return StepResult::Done;
                    }
                    if (step_frames_ >= s.timeout_frames)
                    {
                        log::error("scenario: not settled after %u frames", s.timeout_frames);
                        return StepResult::Failed;
                    }
                    return StepResult::Running;
                }
                else if constexpr (std::is_same_v<S, scenario::Capture>)
                {
                    // TAA's history holds every frame before, however many
                    // there were: discard it and wait one jitter cycle, so the
                    // capture is the same image on every run. The frame is
                    // taken by tick_scenario, once this says so.
                    if (step_frames_ == 0)
                    {
                        renderer_.reset_taa();
                    }
                    return StepResult::Running;
                }
                else if constexpr (std::is_same_v<S, scenario::Camera>)
                {
                    place_camera(to_vec(s.position), to_vec(s.target), to_vec(s.up));
                    return StepResult::Done;
                }
                else if constexpr (std::is_same_v<S, scenario::Fly>)
                {
                    if (step_frames_ >= s.frames)
                    {
                        return StepResult::Done;
                    }
                    Transform& camera = scene_.registry.get<Transform>(scene_.camera);
                    camera.position += scene_.rotation() * to_vec(s.velocity) / kScenarioStepHz;
                    return StepResult::Running;
                }
                else if constexpr (std::is_same_v<S, scenario::Wait>)
                {
                    return step_frames_ >= s.frames ? StepResult::Done : StepResult::Running;
                }
                else if constexpr (std::is_same_v<S, scenario::FreezeTerrain>)
                {
                    terrain_.set_frozen(true);
                    return StepResult::Done;
                }
                else if constexpr (std::is_same_v<S, scenario::ThawTerrain>)
                {
                    terrain_.set_frozen(false);
                    return StepResult::Done;
                }
                else if constexpr (std::is_same_v<S, scenario::Interactive>)
                {
                    return StepResult::Done;
                }
                else if constexpr (std::is_same_v<S, scenario::Spawn>)
                {
                    spawn(s);
                    return StepResult::Done;
                }
                else if constexpr (std::is_same_v<S, scenario::Walk>)
                {
                    if (movement_ != Movement::Walk)
                    {
                        log::error("scenario: walk needs \"movement\": \"walk\"");
                        return StepResult::Failed;
                    }
                    // Counted in steps, not frames: a frame waiting on new
                    // ground takes none, and counting it would walk a
                    // different distance whenever the pool ran slower.
                    if (step_frames_ == 0)
                    {
                        walk_until_ = physics_.steps() + s.frames;
                    }
                    if (physics_.steps() >= walk_until_)
                    {
                        return StepResult::Done;
                    }
                    input_.inject(actions_.walk_move,
                                  f32vec3{static_cast<f32>(s.move[0]), static_cast<f32>(s.move[1]), 0.0f});
                    input_.inject(actions_.sprint, f32vec3{s.sprint ? 1.0f : 0.0f, 0.0f, 0.0f});
                    input_.inject(actions_.jump, f32vec3{s.jump ? 1.0f : 0.0f, 0.0f, 0.0f});
                    return StepResult::Running;
                }
                else if constexpr (std::is_same_v<S, scenario::Action>)
                {
                    optional<input::ActionId> const action = input_.map().find_action(s.action);
                    if (!action.has_value())
                    {
                        log::error("scenario: no action \"%s\"", s.action.c_str());
                        return StepResult::Failed;
                    }
                    if (step_frames_ >= s.frames)
                    {
                        return StepResult::Done;
                    }
                    f32vec3 value{0.0f};
                    for (size_t axis = 0; axis < s.value.size(); ++axis)
                    {
                        value[axis] = s.value[axis];
                    }
                    input_.inject(*action, value);
                    return StepResult::Running;
                }
                else
                {
                    static_assert(std::is_same_v<S, scenario::Settings>);
                    apply_settings(s);
                    return StepResult::Done;
                }
            },
            step);
    }

    bool App::tick_scenario()
    {
        while (scenario_.has_value() && step_index_ < scenario_->steps.size())
        {
            scenario::Step const& step = scenario_->steps[step_index_];
            if (!step_started_)
            {
                step_started_ = true;
                step_frames_  = 0;
                log::info("scenario: step %zu: %s", step_index_, scenario::to_json(step).c_str());
            }

            if (std::holds_alternative<scenario::Interactive>(step))
            {
                log::info("scenario: handing over to the user");
                scenario_.reset();
                return false;
            }

            StepResult const result = run_step(step);
            if (result == StepResult::Failed)
            {
                scenario_failed_ = true;
                step_index_      = scenario_->steps.size();
                return false;
            }
            if (result == StepResult::Running)
            {
                // A capture's frame comes once TAA has had its jitter cycle.
                u32 const settle = renderer_.taa() ? config::kTaaJitterCount : 0u;
                return std::holds_alternative<scenario::Capture>(step) && step_frames_ >= settle;
            }

            ++step_index_;
            step_started_ = false;
        }
        return false;
    }

    void App::finish_capture()
    {
        scenario::Capture const& capture = std::get<scenario::Capture>(scenario_->steps[step_index_]);
        std::filesystem::path const path = scenario_output_ / capture.file;

        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error || !save_capture(path))
        {
            log::error("scenario: could not write %s", path.string().c_str());
            scenario_failed_ = true;
            step_index_      = scenario_->steps.size();
            return;
        }

        ++step_index_;
        step_started_ = false;
    }

    bool App::save_capture(std::filesystem::path const& path)
    {
        // The copy is in the frame just submitted.
        device_.wait_idle();

        optional<Renderer::Capture> capture = renderer_.take_capture();
        if (!capture.has_value())
        {
            log::error("no capture: the swapchain cannot be copied from");
            return false;
        }

        bool const bgra = capture->format == VK_FORMAT_B8G8R8A8_SRGB ||
                          capture->format == VK_FORMAT_B8G8R8A8_UNORM;
        bool const rgba = capture->format == VK_FORMAT_R8G8B8A8_SRGB ||
                          capture->format == VK_FORMAT_R8G8B8A8_UNORM;
        if (!bgra && !rgba)
        {
            log::error("no capture: swapchain format %d is not 8-bit RGBA or BGRA",
                       static_cast<int>(capture->format));
            return false;
        }

        Pixels pixels{.width = capture->width, .height = capture->height,
                      .rgba = std::move(capture->pixels)};
        for (size_t at = 0; at < pixels.rgba.size(); at += 4)
        {
            if (bgra)
            {
                std::swap(pixels.rgba[at], pixels.rgba[at + 2]);
            }
            pixels.rgba[at + 3] = 255;   // the swapchain's alpha means nothing
        }

        string const file = path.string();
        if (!save_png(file, pixels))
        {
            return false;
        }
        log::info("captured %ux%u to %s", pixels.width, pixels.height, file.c_str());
        return true;
    }

    void App::idle_until(std::chrono::steady_clock::time_point deadline,
                         std::chrono::steady_clock::time_point frame_start)
    {
        using clock = std::chrono::steady_clock;

        // SDL_DelayNS, not std::this_thread::sleep_for: on Windows SDL waits
        // on a high-resolution timer, where MinGW's sleep_for goes through
        // Sleep() and the default 15.6 ms tick.
        auto const sleep = [](clock::duration span) {
            auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(span).count();
            if (ns > 0)
            {
                SDL_DelayNS(static_cast<u64>(ns));
            }
        };

        auto const pump_period = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<f64>(1.0 / kIdlePumpHz));

        // Pumps fall on a grid from the frame's start, the deadline it woke
        // at and polled, so they hold their rate rather than drifting by
        // each wake's lateness. A slot the frame's work overran is pumped
        // as soon as the work ends; a slot at or past the deadline is left
        // to the next frame's poll, which pumps first.
        clock::time_point next_pump = frame_start + pump_period;
        clock::time_point now       = clock::now();
        while (next_pump < deadline)
        {
            sleep(next_pump - now);
            SDL_PumpEvents();

            now = clock::now();
            while (next_pump <= now)
            {
                next_pump += pump_period;
            }
        }
        sleep(deadline - now);
    }

    f64 App::limit_frame_rate()
    {
        using clock = std::chrono::steady_clock;

        if (!limit_frames_)
        {
            next_frame_.reset();
            return 0.0;
        }

        auto const period = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<f64>(1.0 / kFrameLimitHz));
        auto const start  = clock::now();

        if (next_frame_.has_value() && start < *next_frame_)
        {
            idle_until(*next_frame_, *next_frame_ - period);
        }

        auto const woke = clock::now();

        // Deadlines advance by whole periods, so oversleeping one frame is
        // made up by the next and the average holds the rate. A frame that
        // ran more than a period late restarts the schedule from now rather
        // than racing to catch up.
        if (!next_frame_.has_value() || woke - *next_frame_ > period)
        {
            next_frame_ = woke + period;
        }
        else
        {
            *next_frame_ += period;
        }

        return std::chrono::duration<f64, std::milli>(woke - start).count();
    }

    int App::run()
    {
        bool running = true;

        while (running)
        {
            // First, so input is polled as late as possible before the frame
            // that uses it.
            f64 const slept_ms = limit_frame_rate();

            FrameEvents const events = window_.poll();

            if (events.quit_requested)
            {
                running = false;
                continue;
            }

            // While a scenario runs, the keyboard and mouse change nothing
            // but quitting and copying the camera: the machine is shared,
            // and a stray key would change a capture. Only the engine
            // context is enabled under one.
            bool const scripted = scenario_.has_value();

            // This frame's events as actions, in the contexts last frame left
            // enabled.
            input_.update();
            if (input_.fired(actions_.quit))
            {
                running = false;
                continue;
            }

            focused_ = events.focus_gained || (focused_ && !events.focus_lost);

            if (window_.minimized())
            {
                // Nothing presentable; block rather than spin. The gap is not
                // a frame, so do not let it register as one.
                SDL_WaitEvent(nullptr);
                last_frame_.reset();
                last_tick_.reset();
                next_frame_.reset();
                continue;
            }

            // A resize event fires once at startup for the initial size, and
            // again for any move between displays. Rebuilding an identical
            // swapchain is wasteful, so compare before acting.
            if (events.resized && !swapchain_matches_window())
            {
                device_.wait_idle();
                if (!rebuild_swapchain())
                {
                    continue;
                }
            }

            // f64: the scene integrates animation over session-long times, and
            // f32 seconds lose millisecond resolution after a few hours.
            f64 const seconds =
                fixed_time_.value_or(static_cast<f64>(log::elapsed_ms()) / 1000.0);

            // Wall-clock time since this same point last iteration, for flying
            // and exposure adaptation. Stamp to stamp at one fixed point in
            // the loop, so it spans exactly one whole frame; measuring from
            // where the previous draw() returned would span only the poll and
            // the UI, a jittering sliver of it. Clamped, so a hitch does not
            // fling the camera. Not pinned with animation: a pinned camera
            // still has to move at a real speed when flown.
            auto const tick  = std::chrono::steady_clock::now();
            f64        delta = 0.0;
            if (last_tick_.has_value())
            {
                delta = std::min(std::chrono::duration<f64>(tick - *last_tick_).count(),
                                 kMaxFrameSeconds);
            }
            last_tick_ = tick;

            // Flown first, by the user or the scenario: the camera is an
            // entity, and Scene::update is what composes its world transform
            // for this frame.
            // A scenario's steps inject actions, so they run before the
            // actions are acted on.
            bool capturing = false;
            walk_input_    = {};
            if (scripted)
            {
                capturing = tick_scenario();
                if (scenario_.has_value() && step_index_ >= scenario_->steps.size() && !finish_scenario())
                {
                    running = false;
                    continue;
                }
            }
            handle_engine_actions();

            if (!scripted)
            {
                capture_mouse();
                if (movement_ == Movement::Jetpack)
                {
                    fly(delta);
                }
            }
            if (movement_ == Movement::Walk)
            {
                read_walk_input();
            }
            enable_contexts(scenario_.has_value());
            assets_.update();
            // Finished terrain chunks become meshes here, and the octree picks
            // the chunks this camera wants and swaps them in and out, all
            // before the scene composes world transforms. It reads the
            // camera's world transform from the last update, a frame behind.
            pool_.drain();
            terrain_.update(scene_, assets_, pool_);

            // Walking, the mouse turns the character and the keys ask it for
            // a velocity, which the steps below walk.
            if (movement_ == Movement::Walk)
            {
                if (optional<physics::CharacterState> const character = physics_.character())
                {
                    f64vec3 const move = walk_.steer(walk_input_, character->up);
                    physics_.set_character_input(physics::CharacterInput{.move = move, .jump = walk_input_.jump});
                }
            }

            // After the drain, which adds finished collision chunks; before
            // Scene::update, which composes the Transforms it writes. A
            // scenario steps once a frame, so its runs match.
            physics_.update(scene_.registry, pool_, delta, scripted);

            // The eye over where the character's feet are now, between the
            // last two steps like every body.
            if (movement_ == Movement::Walk)
            {
                if (optional<physics::CharacterState> const character = physics_.character())
                {
                    walk_.place(scene_.registry.get<Transform>(scene_.camera), character->feet, character->up);
                }
            }
            renderer_.set_terrain_palette(terrain_.palette());
            scene_.update(seconds, assets_);

            // Colliders for model parts spawned by the update, at the world
            // transforms it composed, before the next frame's step.
            physics_.add_static_colliders(scene_.registry);

            draw_ui();

            // After the UI, whose checkboxes may have changed what is shown,
            // and from the world transforms the update just composed.
            collect_debug_lines();

            // With animation pinned, exposure jumps straight to the metered
            // value, so two runs' captures match however long each took to
            // start.
            renderer_.set_frame_time(delta, fixed_time_.has_value());

            // Read back from the renderer rather than grabbed off the
            // desktop, so other windows and the compositor cannot get in.
            if (capturing)
            {
                renderer_.request_capture();
            }

            FrameResult const result = renderer_.draw(swapchain_, scene_, assets_, &ui_);

            // A step counts the frames drawn while it runs; one that was not
            // drawn is retried, a capture included.
            if (result == FrameResult::Ok && scenario_.has_value() && step_started_)
            {
                if (capturing)
                {
                    finish_capture();
                }
                else
                {
                    ++step_frames_;
                }
            }

            switch (result)
            {
            case FrameResult::Ok:
            {
                // Wall clock, not `seconds`: a scenario pins animation,
                // not the passage of real time the stats are measuring.
                auto const now = std::chrono::steady_clock::now();
                if (last_frame_.has_value())
                {
                    f64 const frame_ms =
                        std::chrono::duration<f64, std::milli>(now - *last_frame_).count();
                    f64 const stamp =
                        std::chrono::duration<f64>(now.time_since_epoch()).count();
                    // The limiter's sleep is idle time, like the renderer's
                    // blocking, and must not count as CPU busy.
                    stats_.record(stamp, frame_ms, renderer_.blocked_ms() + slept_ms,
                                  renderer_.gpu_timings());
                }
                last_frame_ = now;
                break;
            }

            case FrameResult::OutOfDate:
                device_.wait_idle();
                rebuild_swapchain();
                break;

            case FrameResult::Error:
                log::error("frame failed; stopping");
                running = false;
                break;
            }
        }

        // Everything below unwinds through destructors; they must not run
        // while the GPU is still reading the resources they free.
        device_.wait_idle();
        if (!failed_scenarios_.empty())
        {
            string names;
            for (string const& name : failed_scenarios_)
            {
                names += (names.empty() ? "" : ", ") + name;
            }
            log::error("%zu of %zu scenarios failed: %s", failed_scenarios_.size(), scenarios_.size(),
                       names.c_str());
            return EXIT_FAILURE;
        }
        log::info("shutting down");
        return EXIT_SUCCESS;
    }
}
