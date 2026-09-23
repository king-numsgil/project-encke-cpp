#pragma once

#include "render/config.hpp"
#include "render/extract.hpp"
#include "render/geometry_pool.hpp"
#include "render/gltf.hpp"
#include "render/gpu_types.hpp"
#include "render/material.hpp"
#include "render/material_loader.hpp"
#include "render/mesh.hpp"
#include "render/model.hpp"
#include "render/pipeline.hpp"
#include "render/shadows.hpp"
#include "vulkan/bindless.hpp"
#include "vulkan/buffer.hpp"
#include "vulkan/image.hpp"
#include "vulkan/staging.hpp"
#include "vulkan/texture.hpp"
#include "vulkan/timestamps.hpp"

namespace encke
{
    class Scene;
    class VulkanAllocator;
    class VulkanDevice;
    class VulkanSwapchain;

    enum class FrameResult
    {
        Ok,
        // The swapchain no longer matches the surface. The caller must wait
        // for idle and recreate it; this frame produced nothing.
        OutOfDate,
        Error,
    };

    // How the frame itself is shaded. Values must match the debug_view check
    // in shaders/lighting.slang.
    enum class DebugView : u32
    {
        Lit        = 0,
        BruteForce = 1,   // every light, no clusters; must match Lit
    };

    // Visualisations drawn into their own images for UI windows, not over the
    // frame. debug_view in shaders/debug_views.slang is kFirstDebugWindowView
    // plus this value.
    enum class DebugWindow : u32
    {
        ClusterHeat = 0,
        Normals     = 1,
        Motion      = 2,
        Cascades    = 3,
    };

    // The display curve. Values must match the switch in shaders/tonemap.slang.
    enum class Tonemap : u32
    {
        Aces       = 0,   // Narkowicz's per-channel fit
        AgX        = 1,
        PbrNeutral = 2,   // Khronos PBR Neutral
    };

    inline constexpr u32 kTonemapCount = 3;

    inline constexpr u32 kDebugWindowCount      = 4;
    inline constexpr u32 kFirstDebugWindowView  = 2;

    // Clustered deferred. Per frame:
    //
    //   1. shadows    raster  depth only: the sun's cascades, then the chosen
    //                         spot lights' maps
    //   2. G-buffer   raster  albedo, normal, material, motion, depth; emissive
    //                         seeds the HDR target
    //   3. clusters   compute assign lights to the froxel grid
    //   4. lighting   compute shade each pixel against its cluster's lights,
    //                         adding onto the HDR target
    //   5. exposure   compute luminance histogram, then meter and adapt EV100
    //   6. debug      compute one visualisation image per open debug window
    //   7. tonemap    raster  HDR -> swapchain, at the adapted exposure
    //   8. overlay    raster  caller-recorded UI, through the swapchain's UI view
    class Renderer
    {
    public:
        static constexpr u32 kFramesInFlight = 2;

        // Whatever draws over the finished frame -- the UI. Two phases, because
        // uploads and barriers are illegal inside a rendering scope.
        class Overlay
        {
        public:
            virtual ~Overlay() = default;

            // Outside any rendering scope, before record(). `slot` is the
            // frame-in-flight index; its previous use has retired, so
            // anything the overlay keyed to that slot may be reused or freed.
            virtual void prepare(VkCommandBuffer command, u32 slot) = 0;

            // Inside the overlay rendering scope: one colour attachment, the
            // swapchain's ui_view() in ui_format(), contents loaded.
            virtual void record(VkCommandBuffer command) = 0;
        };

        Renderer() = default;
        ~Renderer();

        Renderer(Renderer const&)            = delete;
        Renderer& operator=(Renderer const&) = delete;
        Renderer(Renderer&&)                 = delete;
        Renderer& operator=(Renderer&&)      = delete;

        bool init(VulkanAllocator const& allocator, VulkanDevice const& device,
                  VulkanSwapchain const& swapchain);
        void shutdown();

        // Resizes every screen-sized target, rewrites their bindless slots in
        // place, and rebuilds per-image semaphores. Caller waits for idle.
        bool on_swapchain_changed(VulkanSwapchain const& swapchain);

        // Uploads a loaded glTF's meshes and textures and returns the ids a
        // scene places it by. Blocking, startup-time work, and it registers
        // new bindless slots: call it before the first frame, or with the
        // device idle.
        optional<Model> add_model(GltfModel const& model);

        // `overlay` may be null.
        FrameResult draw(VulkanSwapchain const& swapchain, Scene const& scene, Overlay* overlay);

        // The one global descriptor set. Mutable so the UI can register its
        // textures and samplers in it; every pipeline shares it.
        BindlessSet& bindless() { return bindless_; }

        void      set_debug_view(DebugView view) { debug_view_ = view; }
        DebugView debug_view() const { return debug_view_; }

        // Which visualisation images to draw this frame. Set it after the UI
        // has decided which windows are open and before draw(): a window
        // showing an image this frame did not draw would show stale content.
        void set_debug_window_open(DebugWindow window, bool open)
        {
            debug_open_[static_cast<u32>(window)] = open;
        }

        // Magnification for the motion visualisation. Motion is per-frame
        // displacement, so the right gain depends on the frame rate.
        void set_motion_gain(f32 gain) { motion_gain_ = gain; }

        // Wall-clock seconds since the previous frame, which auto-exposure
        // adapts over. `jump` skips adaptation and goes straight to the
        // metered value, so pinned-time captures come out identical.
        void set_frame_time(f64 seconds, bool jump)
        {
            frame_seconds_ = seconds;
            exposure_jump_ = jump;
        }

        // A finished frame, UI included, as the swapchain holds it: display
        // encoded, rows packed, four bytes per pixel in `format`'s order.
        struct Capture
        {
            vector<u8> pixels;
            u32        width  = 0;
            u32        height = 0;
            VkFormat   format = VK_FORMAT_UNDEFINED;
        };

        // Copies the next frame drawn into host memory. Ignored if the
        // swapchain cannot be copied from.
        void request_capture() { capture_requested_ = true; }

        // The copy request_capture() asked for, once the device is idle;
        // empty if none was recorded.
        optional<Capture> take_capture();

        // Every streamed material has been decoded and uploaded. Until then
        // frames differ as materials land, so a capture waits for it.
        bool streaming_idle() const
        {
            return loader_.idle() && decoded_.empty() && geometry_.idle();
        }

        void    set_tonemap(Tonemap tonemap) { tonemap_ = tonemap; }
        Tonemap tonemap() const { return tonemap_; }

        // Tonemap at this EV100 instead of the adapted one. Metering still
        // runs, so clearing it resumes from the metered value.
        void set_fixed_ev100(optional<f32> ev100) { fixed_ev100_ = ev100; }

        // Bindless sampled-image handle of a visualisation, in
        // READ_ONLY_OPTIMAL by the time the overlay runs. Stable across
        // resizes.
        u32 debug_window_image(DebugWindow window) const
        {
            return debug_sampled_handles_[static_cast<u32>(window)];
        }

        // Per-pass GPU time of the newest frame whose results are back, which
        // trails the frame being recorded by kFramesInFlight.
        span<GpuSection const> gpu_timings() const { return timestamps_.sections(); }

        // Time the last draw() spent blocked on the frame fence, image
        // acquisition and present, in milliseconds.
        f64 blocked_ms() const { return blocked_ms_; }

    private:
        // Rewritten by the CPU every frame, so one set per frame in flight.
        struct FrameResources
        {
            Buffer frame;
            Buffer objects;
            Buffer lights;
            Buffer shadow_views;
            Buffer shadow_matrices;

            // VkDrawIndexedIndirectCommand: the G-buffer's at 0, then shadow
            // view v's at (1 + v) * kMaxObjects.
            Buffer draws;

            u32 frame_handle           = BindlessSet::kInvalid;
            u32 objects_handle         = BindlessSet::kInvalid;
            u32 lights_handle          = BindlessSet::kInvalid;
            u32 shadow_views_handle    = BindlessSet::kInvalid;
            u32 shadow_matrices_handle = BindlessSet::kInvalid;
        };

        // Streamed in once and read-only after, so unlike the targets they
        // need no per-frame barriers. A tiling material repeats over
        // the surface at `tile` metres, stretched by the object's scale; a
        // non-tiling one is a glTF atlas whose UVs are used as they are.
        struct MaterialTextures
        {
            Texture albedo;
            Texture normal;
            Texture orm;
            Texture emission;
            u32vec4 handles{gpu::kNoTexture};   // as gpu::Object::textures
            f32vec2 tile{1.0f};                 // metres per repeat
            bool    tiling = true;

            // Objects draw with no emission until the maps land: set for a
            // material whose emissive factor is meant to be masked by a map.
            bool hide_emission = false;
        };

        bool create_targets(VkExtent2D extent);
        void register_targets(bool first_time);
        bool create_shadow_maps();
        bool create_materials();

        // Takes what the loader has finished and, within this frame's staging
        // budget, creates its textures and stages their texels. Call after
        // the slot's fence and the acquire, before upload(), which then sees
        // the new handles; the copies go into this frame's command buffer.
        void stream_materials();

        // Records the copies stream_materials() staged. First in the frame,
        // outside any rendering scope.
        void record_uploads(VkCommandBuffer command);

        // Extracts the scene into render_list_ and writes the frame's buffers
        // from it. Also plans this frame's shadows, which record() then draws.
        void upload(Scene const& scene, VkExtent2D extent, FrameResources& resources);
        bool record(VkCommandBuffer command, VulkanSwapchain const& swapchain, u32 image_index,
                    Scene const& scene, Overlay* overlay);
        void destroy_image_semaphores();

        VulkanAllocator const* allocator_ = nullptr;
        VulkanDevice const*    device_    = nullptr;

        BindlessSet bindless_;

        Image albedo_;
        Image normal_;
        Image material_;
        Image motion_;
        Image depth_;
        Image hdr_;

        u32 albedo_handle_      = BindlessSet::kInvalid;
        u32 normal_handle_      = BindlessSet::kInvalid;
        u32 material_handle_    = BindlessSet::kInvalid;
        u32 motion_handle_      = BindlessSet::kInvalid;
        u32 depth_handle_       = BindlessSet::kInvalid;
        u32 hdr_storage_handle_ = BindlessSet::kInvalid;
        u32 hdr_sampled_handle_ = BindlessSet::kInvalid;

        // Written by compute in GENERAL, sampled by the UI in
        // READ_ONLY_OPTIMAL, so each is registered twice like HDR.
        array<Image, kDebugWindowCount> debug_images_;
        array<u32, kDebugWindowCount>   debug_storage_handles_{};
        array<u32, kDebugWindowCount>   debug_sampled_handles_{};
        array<bool, kDebugWindowCount>  debug_open_{};
        f32                             motion_gain_ = 5000.0f;

        // GPU-written by the cluster pass and read by lighting within the same
        // frame, so a single copy suffices given the barriers between them.
        Buffer cluster_counts_;
        Buffer cluster_lights_;
        u32    cluster_counts_handle_ = BindlessSet::kInvalid;
        u32    cluster_lights_handle_ = BindlessSet::kInvalid;

        array<FrameResources, kFramesInFlight> frames_;

        // Cascades first, then spots; fixed size, so not rebuilt on resize.
        // Single-copy like the G-buffer: each frame's entry barrier waits on
        // the previous frame's compute reads.
        array<Image, config::kShadowViewCount> shadow_maps_;
        array<u32, config::kShadowViewCount>   shadow_map_handles_{};
        VkSampler                              shadow_sampler_        = VK_NULL_HANDLE;
        u32                                    shadow_sampler_handle_ = BindlessSet::kInvalid;

        // What upload() took from the scene this frame; the renderer reads
        // nothing else of it.
        RenderList render_list_;

        // Last drawn frame's world transforms, for motion vectors: each
        // object's by entity, and the camera's view and projection. A new
        // entity has none and moves nowhere on its first frame. Entries for
        // destroyed entities are never removed yet; nothing destroys any.
        entt::storage<f64mat4> previous_models_;
        optional<f64mat4>      previous_view_;
        f64mat4                previous_projection_{1.0};

        // This frame's shadow views, and how many indirect draws upload()
        // wrote for the G-buffer and for each view's casters. Read by record().
        ShadowPlan                               shadow_plan_;
        u32                                      gbuffer_draws_ = 0;
        array<u32, config::kShadowViewCount>     shadow_draws_{};

        // Auto-exposure. The histogram is GPU-only and zeroed by the adapt
        // pass after reading, so it starts each frame empty. The EV100 image
        // persists across frames, which is the adaptation state; it is
        // registered twice, like HDR, because compute writes it in GENERAL and
        // tonemap samples it in READ_ONLY_OPTIMAL.
        Buffer exposure_histogram_;
        u32    exposure_histogram_handle_ = BindlessSet::kInvalid;
        Image  exposure_image_;
        u32    exposure_storage_handle_ = BindlessSet::kInvalid;
        u32    exposure_sampled_handle_ = BindlessSet::kInvalid;
        bool   exposure_written_        = false;   // image holds a value, in READ_ONLY_OPTIMAL
        f64    frame_seconds_           = 0.0;
        bool   exposure_jump_           = true;
        optional<f32> fixed_ev100_;

        // Neutral keeps the most colour and hue of the three; see CLAUDE.md.
        Tonemap tonemap_ = Tonemap::PbrNeutral;

        // Per-frame uploads. Decoded materials wait in decoded_ until the
        // budget has room; pending_uploads_ are this frame's staged copies.
        struct PendingUpload
        {
            Texture const* texture = nullptr;
            VkBuffer       buffer  = VK_NULL_HANDLE;
            VkDeviceSize   offset  = 0;
        };

        // A decoded material and how far through its maps (albedo, normal,
        // ORM, emission, in that order, absent ones skipped) staging has
        // got. Its handles go live with the last one.
        struct StreamingMaterial
        {
            MaterialLoader::Decoded decoded;
            u32                     staged = 0;
        };

        StagingArena              staging_;
        MaterialLoader            loader_;
        vector<StreamingMaterial> decoded_;
        vector<PendingUpload>     pending_uploads_;

        // Host memory the swapchain image is copied into, created on the
        // first capture. `capture_` describes what the copy recorded.
        Buffer  capture_buffer_;
        bool    capture_requested_ = false;
        Capture capture_;
        bool    capture_recorded_  = false;

        GraphicsPipeline gbuffer_pipeline_;
        GraphicsPipeline shadow_pipeline_;
        GraphicsPipeline tonemap_pipeline_;
        ComputePipeline  cluster_pipeline_;
        ComputePipeline  lighting_pipeline_;
        ComputePipeline  debug_pipeline_;
        ComputePipeline  histogram_pipeline_;
        ComputePipeline  adapt_pipeline_;

        // Renderable::mesh and ::material index these. The built-ins come
        // first, in MeshKind and MaterialKind order, then whatever add_model
        // loaded. MaterialTextures is not movable, hence the pointers.
        // materials_[0] is MaterialKind::None and stays null.
        GeometryPool                              geometry_;
        vector<std::unique_ptr<MaterialTextures>> materials_;
        VkSampler                            material_sampler_        = VK_NULL_HANDLE;
        u32                                  material_sampler_handle_ = BindlessSet::kInvalid;

        // 1x1 white, standing in for a glTF material's missing albedo or
        // ORM map so its factors alone decide.
        Texture white_srgb_;
        Texture white_unorm_;
        u32     white_srgb_handle_  = BindlessSet::kInvalid;
        u32     white_unorm_handle_ = BindlessSet::kInvalid;

        VkCommandPool command_pool_ = VK_NULL_HANDLE;

        array<VkCommandBuffer, kFramesInFlight> commands_{};
        array<VkSemaphore, kFramesInFlight>     image_available_{};
        array<VkFence, kFramesInFlight>         in_flight_{};

        // One per swapchain image, not per frame in flight: a frame slot maps
        // to a different image over time, and reusing a pending signal is
        // invalid.
        vector<VkSemaphore> render_finished_;

        GpuTimestamps timestamps_;
        f64           blocked_ms_ = 0.0;

        u32       frame_      = 0;
        DebugView debug_view_ = DebugView::Lit;
    };
}
