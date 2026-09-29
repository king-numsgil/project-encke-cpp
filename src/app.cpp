#include "core/pch.hpp"

#include "app.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "platform/cpu.hpp"
#include "render/config.hpp"
#include "render/pixels.hpp"
#include "ui/image_window.hpp"

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

        // The motion gain slider's range. Motion is per-frame displacement:
        // the top end suits an uncapped couple of thousand frames a second,
        // the bottom a frame-limited 60.
        constexpr f32 kMotionGainMin = 10.0f;
        constexpr f32 kMotionGainMax = 100000.0f;

        // Longest frame the camera and exposure will integrate over at once.
        constexpr f64 kMaxFrameSeconds = 0.1;

        // The frame limiter's rate, when it is on.
        constexpr f64 kFrameLimitHz = 60.0;

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

        // First, so a mistake in the file stops the run before a window opens.
        if (options.scenario.has_value())
        {
            scenario_ = scenario::load(*options.scenario);
            if (!scenario_.has_value())
            {
                return false;
            }
            scenario_output_ = options.output.value_or(options.scenario->parent_path() / "out" /
                                                       scenario_->name);
            log::info("scenario \"%s\": %zu steps, captures to %s", scenario_->name.c_str(),
                      scenario_->steps.size(), scenario_output_.string().c_str());
        }
        bool const srgb_ui = scenario_.has_value() && scenario_->settings.srgb_ui.value_or(false);

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
        window_.set_event_hook([this](SDL_Event const& event) { ui_.process_event(event); });

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
        u32 const collision_lod = scenario_.has_value()
                                      ? scenario_->settings.collision_lod.value_or(config::kCollisionLod)
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
        if (scenario_.has_value())
        {
            fixed_time_ = 0.0;
            show_ui_    = false;
            apply_settings(scenario_->settings);
        }
        renderer_.set_taa(taa_);

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
        bool const alt = window_.key_down(SDL_SCANCODE_LALT) || window_.key_down(SDL_SCANCODE_RALT);
        set_looking(focused_ && !alt);
    }

    void App::fly(FrameEvents const& events, f64 seconds)
    {
        if (!looking_)
        {
            return;
        }

        auto axis = [this](SDL_Scancode positive, SDL_Scancode negative) {
            return (window_.key_down(positive) ? 1.0 : 0.0) - (window_.key_down(negative) ? 1.0 : 0.0);
        };

        auto const either = [this](SDL_Scancode left, SDL_Scancode right) {
            return window_.key_down(left) || window_.key_down(right);
        };

        f64 const strafe_up = (window_.key_down(SDL_SCANCODE_SPACE) ? 1.0 : 0.0) -
                              (either(SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL) ? 1.0 : 0.0);

        FlyInput const input{
            .look  = events.mouse_delta,
            .move  = f64vec3{axis(SDL_SCANCODE_D, SDL_SCANCODE_A), strafe_up,
                             axis(SDL_SCANCODE_W, SDL_SCANCODE_S)},
            .roll  = axis(SDL_SCANCODE_Q, SDL_SCANCODE_E),
            .wheel = events.wheel,
            .fast  = either(SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT),
            // Alt frees the mouse, so slow is C.
            .slow  = window_.key_down(SDL_SCANCODE_C),
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

    void App::read_walk_input(FrameEvents const& events)
    {
        if (!looking_)
        {
            return;
        }

        auto axis = [this](SDL_Scancode positive, SDL_Scancode negative) {
            return (window_.key_down(positive) ? 1.0 : 0.0) - (window_.key_down(negative) ? 1.0 : 0.0);
        };

        walk_input_ = WalkInput{
            .look   = events.mouse_delta,
            .move   = f64vec2{axis(SDL_SCANCODE_D, SDL_SCANCODE_A), axis(SDL_SCANCODE_W, SDL_SCANCODE_S)},
            .sprint = window_.key_down(SDL_SCANCODE_LSHIFT) || window_.key_down(SDL_SCANCODE_RSHIFT),
            .jump   = window_.key_down(SDL_SCANCODE_SPACE),
        };
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
                    walk_input_ = WalkInput{.move   = f64vec2{s.move[0], s.move[1]},
                                            .sprint = s.sprint,
                                            .jump   = s.jump};
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
            // SDL_DelayNS, not std::this_thread::sleep_for: on Windows SDL
            // waits on a high-resolution timer, where MinGW's sleep_for goes
            // through Sleep() and the default 15.6 ms tick.
            auto const wait = std::chrono::duration_cast<std::chrono::nanoseconds>(*next_frame_ - start);
            SDL_DelayNS(static_cast<u64>(wait.count()));
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

            // While a scenario runs, the keyboard and mouse change nothing:
            // the machine is shared, and a stray key would change a capture.
            bool const scripted = scenario_.has_value();

            if (events.toggle_ui && !scripted)
            {
                show_ui_ = !show_ui_;
            }

            if (events.toggle_terrain_freeze && !scripted)
            {
                terrain_.set_frozen(!terrain_.frozen());
                frozen_pose_ = terrain_.frozen() ? optional<scenario::Camera>{camera_step()} : nullopt;
                log::info("terrain: %s", terrain_.frozen() ? "frozen" : "thawed");
            }

            // The pose as scenario steps, on the clipboard and in the log.
            // Frozen, they settle where the terrain froze, freeze it and move
            // to the camera, so the octree shows what was on screen.
            if (events.copy_camera)
            {
                string const current = scenario::to_json(camera_step());
                string const text =
                    frozen_pose_.has_value()
                        ? scenario::to_json(*frozen_pose_) + ",\n" +
                              scenario::to_json(scenario::Settle{}) + ",\n" +
                              scenario::to_json(scenario::FreezeTerrain{}) + ",\n" + current + ","
                        : current + ",";
                SDL_SetClipboardText(text.c_str());
                log::info("camera:\n%s", text.c_str());
            }

            focused_ = events.focus_gained || (focused_ && !events.focus_lost);

            if (events.toggle_jetpack && !ui_.wants_text() && !scripted)
            {
                set_movement(movement_ == Movement::Walk ? Movement::Jetpack : Movement::Walk);
            }

            if (events.cycle_wireframe && !scripted)
            {
                auto const next = static_cast<Wireframe>((static_cast<u32>(renderer_.wireframe()) + 1u) %
                                                         kWireframeCount);
                renderer_.set_wireframe(next);
                log::info("wireframe: %s", wireframe_name(next));
            }
            if (events.toggle_collision && !scripted)
            {
                show_collision_ = !show_collision_;
            }
            if (events.toggle_octree && !scripted)
            {
                show_octree_ = !show_octree_;
            }

            if (events.cycle_tonemap && !ui_.wants_text() && !scripted)
            {
                auto const next = static_cast<Tonemap>(
                    (static_cast<u32>(renderer_.tonemap()) + 1u) % kTonemapCount);
                renderer_.set_tonemap(next);
                log::info("tonemap: %s", tonemap_name(next));
            }

            // A digit typed into a UI text field is not a view switch.
            if (events.debug_view >= 0 && static_cast<u32>(events.debug_view) < kDebugKeyCount &&
                !ui_.wants_text() && !scripted)
            {
                on_debug_key(static_cast<u32>(events.debug_view));
            }

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
            bool capturing = false;
            walk_input_    = {};
            if (scripted)
            {
                capturing = tick_scenario();
                if (scenario_.has_value() && step_index_ >= scenario_->steps.size())
                {
                    running = false;
                    continue;
                }
            }
            else
            {
                capture_mouse();
                if (movement_ == Movement::Jetpack)
                {
                    fly(events, delta);
                }
                else
                {
                    read_walk_input(events);
                }
            }
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
        if (scenario_failed_)
        {
            log::error("scenario failed");
            return EXIT_FAILURE;
        }
        log::info("shutting down");
        return EXIT_SUCCESS;
    }
}
