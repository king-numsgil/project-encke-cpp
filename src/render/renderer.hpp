#pragma once

#include "render/mesh.hpp"
#include "render/pipeline.hpp"
#include "vulkan/bindless.hpp"
#include "vulkan/buffer.hpp"
#include "vulkan/image.hpp"
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

    // Values must match the debug_view switch in shaders/lighting.slang.
    enum class DebugView : u32
    {
        Lit         = 0,
        BruteForce  = 1,   // every light, no clusters; must match Lit
        ClusterHeat = 2,
        Normals     = 3,
        Motion      = 4,
    };

    // Clustered deferred, first draft. Per frame:
    //
    //   1. G-buffer   raster  albedo, normal, material, motion, depth; emissive
    //                         seeds the HDR target
    //   2. clusters   compute assign lights to a 16x9x24 froxel grid
    //   3. lighting   compute shade each pixel against its cluster's lights,
    //                         adding onto the HDR target
    //   4. tonemap    raster  HDR -> swapchain
    //   5. overlay    raster  caller-recorded UI, through the swapchain's UI view
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

        // `overlay` may be null.
        FrameResult draw(VulkanSwapchain const& swapchain, Scene const& scene, Overlay* overlay);

        // The one global descriptor set. Mutable so the UI can register its
        // textures and samplers in it; every pipeline shares it.
        BindlessSet& bindless() { return bindless_; }

        void      set_debug_view(DebugView view) { debug_view_ = view; }
        DebugView debug_view() const { return debug_view_; }

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

            u32 frame_handle   = BindlessSet::kInvalid;
            u32 objects_handle = BindlessSet::kInvalid;
            u32 lights_handle  = BindlessSet::kInvalid;
        };

        bool create_targets(VkExtent2D extent);
        void register_targets(bool first_time);
        void upload(Scene const& scene, VkExtent2D extent, FrameResources& resources) const;
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

        // GPU-written by the cluster pass and read by lighting within the same
        // frame, so a single copy suffices given the barriers between them.
        Buffer cluster_counts_;
        Buffer cluster_lights_;
        u32    cluster_counts_handle_ = BindlessSet::kInvalid;
        u32    cluster_lights_handle_ = BindlessSet::kInvalid;

        array<FrameResources, kFramesInFlight> frames_;

        GraphicsPipeline gbuffer_pipeline_;
        GraphicsPipeline tonemap_pipeline_;
        ComputePipeline  cluster_pipeline_;
        ComputePipeline  lighting_pipeline_;

        Mesh cube_;

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
