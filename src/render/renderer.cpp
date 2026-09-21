#include "core/pch.hpp"

#include "render/renderer.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "render/gpu_types.hpp"
#include "render/scene.hpp"
#include "vulkan/allocator.hpp"
#include "vulkan/context.hpp"
#include "vulkan/device.hpp"
#include "vulkan/swapchain.hpp"

#include <chrono>
#include <cmath>
#include <cstring>

namespace encke
{
    namespace
    {
        // -- G-buffer layout ----------------------------------------------------
        // Mirrored by the SV_Target order in shaders/gbuffer.slang.

        constexpr VkFormat kAlbedoFormat   = VK_FORMAT_R8G8B8A8_SRGB;       // albedo | ao
        constexpr VkFormat kNormalFormat   = VK_FORMAT_R16G16_UNORM;        // octahedral
        constexpr VkFormat kMaterialFormat = VK_FORMAT_R8G8_UNORM;          // rough | metal
        constexpr VkFormat kMotionFormat   = VK_FORMAT_R16G16_SFLOAT;       // uv prev - cur
        constexpr VkFormat kHdrFormat      = VK_FORMAT_R16G16B16A16_SFLOAT; // light accum

        // Visualisations: storage-writable (no sRGB format is), and linear, as
        // the UI expects every texture it samples to be.
        constexpr VkFormat kDebugFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

        constexpr char const* kDebugImageNames[kDebugWindowCount]{
            "debug: cluster heat", "debug: normals", "debug: motion",
        };

        constexpr VkFormat kGBufferFormats[]{
            kAlbedoFormat, kNormalFormat, kMaterialFormat, kMotionFormat, kHdrFormat,
        };

        // Reversed-Z. Near maps to 1.0, infinity to 0.0; clear to the far
        // value and test GREATER. See CLAUDE.md -- all four must agree.
        constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
        constexpr f32      kClearDepth  = 0.0f;

        // -- clusters -------------------------------------------------------------
        // Tuned for ship interiors: 16x9 tiles match a 16:9 screen, and 24
        // logarithmic slices between 10 cm and 400 m put most of the depth
        // resolution in the first few tens of metres, where a corridor or a
        // bridge actually has lights. Past 400 m everything shares the last
        // slice; lights that far away are exterior and will be handled
        // differently anyway.
        constexpr u32 kClustersX           = 16;
        constexpr u32 kClustersY           = 9;
        constexpr u32 kClustersZ           = 24;
        constexpr u32 kClusterCount        = kClustersX * kClustersY * kClustersZ;
        constexpr u32 kMaxLightsPerCluster = 64;
        constexpr f32 kClusterNear         = 0.1f;
        constexpr f32 kClusterFar          = 400.0f;

        constexpr u32 kMaxObjects = 256;
        constexpr u32 kMaxLights  = 1024;

        // Placeholder until there is a proper exposure model. Chosen by eye
        // for the test corridor.
        constexpr f32 kExposure = 0.06f;

        constexpr u64 kNoTimeout = ~0ULL;

        using Clock = std::chrono::steady_clock;

        struct Transition
        {
            VkImage               image;
            VkImageLayout         from;
            VkImageLayout         to;
            VkPipelineStageFlags2 src_stage;
            VkAccessFlags2        src_access;
            VkPipelineStageFlags2 dst_stage;
            VkAccessFlags2        dst_access;
            VkImageAspectFlags    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        };

        // One vkCmdPipelineBarrier2 for a batch of image transitions and
        // optional global memory barriers.
        void barrier(VkCommandBuffer command, span<Transition const> transitions,
                     span<VkMemoryBarrier2 const> memory = {})
        {
            array<VkImageMemoryBarrier2, 8> images{};
            size_t const count = transitions.size();

            for (size_t i = 0; i < count; ++i)
            {
                Transition const& t = transitions[i];
                images[i] = VkImageMemoryBarrier2{
                    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .pNext               = nullptr,
                    .srcStageMask        = t.src_stage,
                    .srcAccessMask       = t.src_access,
                    .dstStageMask        = t.dst_stage,
                    .dstAccessMask       = t.dst_access,
                    .oldLayout           = t.from,
                    .newLayout           = t.to,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image               = t.image,
                    .subresourceRange    = {t.aspect, 0, 1, 0, 1},
                };
            }

            VkDependencyInfo const dependency{
                .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext                    = nullptr,
                .dependencyFlags          = 0,
                .memoryBarrierCount       = static_cast<u32>(memory.size()),
                .pMemoryBarriers          = memory.data(),
                .bufferMemoryBarrierCount = 0,
                .pBufferMemoryBarriers    = nullptr,
                .imageMemoryBarrierCount  = static_cast<u32>(count),
                .pImageMemoryBarriers     = images.data(),
            };

            vkCmdPipelineBarrier2(command, &dependency);
        }

        VkMemoryBarrier2 compute_to_compute(VkAccessFlags2 src, VkAccessFlags2 dst)
        {
            return VkMemoryBarrier2{
                .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .pNext         = nullptr,
                .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = src,
                .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dstAccessMask = dst,
            };
        }

        VkRenderingAttachmentInfo colour_attachment(VkImageView view, VkClearColorValue clear)
        {
            return VkRenderingAttachmentInfo{
                .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext              = nullptr,
                .imageView          = view,
                .imageLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .resolveMode        = VK_RESOLVE_MODE_NONE,
                .resolveImageView   = VK_NULL_HANDLE,
                .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .loadOp             = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp            = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue         = {.color = clear},
            };
        }

        void set_viewport(VkCommandBuffer command, VkExtent2D extent)
        {
            VkViewport const viewport = flipped_viewport(static_cast<f32>(extent.width),
                                                         static_cast<f32>(extent.height));
            VkRect2D const scissor{.offset = {0, 0}, .extent = extent};
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdSetScissor(command, 0, 1, &scissor);
        }

        u32 groups(u32 items, u32 per_group)
        {
            return (items + per_group - 1) / per_group;
        }

        constexpr VkVertexInputBindingDescription kVertexBindings[]{
            {.binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
        };

        constexpr VkVertexInputAttributeDescription kVertexAttributes[]{
            {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
             .offset = offsetof(Vertex, position)},
            {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
             .offset = offsetof(Vertex, normal)},
            {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
             .offset = offsetof(Vertex, colour)},
        };
    }

    Renderer::~Renderer()
    {
        shutdown();
    }

    bool Renderer::init(VulkanAllocator const& allocator, VulkanDevice const& device,
                        VulkanSwapchain const& swapchain)
    {
        allocator_ = &allocator;
        device_    = &device;

        if (!bindless_.init(device))
        {
            return false;
        }

        if (!create_targets(swapchain.extent()))
        {
            return false;
        }
        register_targets(true);

        VkBufferUsageFlags const storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        if (!cluster_counts_.init_device(allocator, device, kClusterCount * sizeof(u32), storage) ||
            !cluster_lights_.init_device(allocator, device,
                                         u64{kClusterCount} * kMaxLightsPerCluster * sizeof(u32),
                                         storage))
        {
            return false;
        }

        // Writable: the cluster pass stores into them. Only compute touches
        // these, which is what makes the writable binding legal.
        cluster_counts_handle_ =
            bindless_.add_writable_buffer(cluster_counts_.handle(), cluster_counts_.size());
        cluster_lights_handle_ =
            bindless_.add_writable_buffer(cluster_lights_.handle(), cluster_lights_.size());

        for (FrameResources& resources : frames_)
        {
            if (!resources.frame.init_mapped(allocator, sizeof(gpu::Frame), storage) ||
                !resources.objects.init_mapped(allocator, kMaxObjects * sizeof(gpu::Object),
                                               storage) ||
                !resources.lights.init_mapped(allocator, kMaxLights * sizeof(gpu::Light), storage))
            {
                return false;
            }

            resources.frame_handle =
                bindless_.add_storage_buffer(resources.frame.handle(), resources.frame.size());
            resources.objects_handle =
                bindless_.add_storage_buffer(resources.objects.handle(), resources.objects.size());
            resources.lights_handle =
                bindless_.add_storage_buffer(resources.lights.handle(), resources.lights.size());
        }

        vector<Vertex> vertices;
        vector<u32>    indices;
        build_cube(vertices, indices);
        if (!cube_.init(allocator, device, vertices, indices))
        {
            return false;
        }

        GraphicsPipeline::Config const gbuffer_config{
            .spirv_name         = "gbuffer.spv",
            .colour_formats     = kGBufferFormats,
            .depth_format       = kDepthFormat,
            .bindings           = kVertexBindings,
            .attributes         = kVertexAttributes,
            .cull_mode          = VK_CULL_MODE_BACK_BIT,
            .alpha_blend        = false,
            .set_layout         = bindless_.layout(),
            .push_constant_size = sizeof(gpu::Push),
        };

        VkFormat const swapchain_format = swapchain.format();

        GraphicsPipeline::Config const tonemap_config{
            .spirv_name         = "tonemap.spv",
            .colour_formats     = span<VkFormat const>{&swapchain_format, 1},
            .depth_format       = VK_FORMAT_UNDEFINED,
            // Full-screen triangle generated from SV_VertexID.
            .bindings           = {},
            .attributes         = {},
            .cull_mode          = VK_CULL_MODE_NONE,
            .alpha_blend        = false,
            .set_layout         = bindless_.layout(),
            .push_constant_size = sizeof(gpu::Push),
        };

        ComputePipeline::Config const cluster_config{
            .spirv_name         = "cluster_build.spv",
            .set_layout         = bindless_.layout(),
            .push_constant_size = sizeof(gpu::Push),
        };

        ComputePipeline::Config const lighting_config{
            .spirv_name         = "lighting.spv",
            .set_layout         = bindless_.layout(),
            .push_constant_size = sizeof(gpu::Push),
        };

        ComputePipeline::Config const debug_config{
            .spirv_name         = "debug_views.spv",
            .set_layout         = bindless_.layout(),
            .push_constant_size = sizeof(gpu::Push),
        };

        if (!gbuffer_pipeline_.init(device, gbuffer_config) ||
            !tonemap_pipeline_.init(device, tonemap_config) ||
            !cluster_pipeline_.init(device, cluster_config) ||
            !lighting_pipeline_.init(device, lighting_config) ||
            !debug_pipeline_.init(device, debug_config))
        {
            return false;
        }

        if (!timestamps_.init(device, kFramesInFlight))
        {
            return false;
        }

        VkCommandPoolCreateInfo const pool_info{
            .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = *device.families().graphics,
        };

        VkResult result = vkCreateCommandPool(device.handle(), &pool_info,
                                              memory::vulkan_callbacks(), &command_pool_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateCommandPool", result);
            return false;
        }

        VkCommandBufferAllocateInfo const alloc_info{
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext              = nullptr,
            .commandPool        = command_pool_,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = kFramesInFlight,
        };

        result = vkAllocateCommandBuffers(device.handle(), &alloc_info, commands_.data());
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkAllocateCommandBuffers", result);
            return false;
        }

        VkSemaphoreCreateInfo const semaphore_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };

        // Created signalled so the first wait on each frame slot returns at
        // once instead of deadlocking.
        VkFenceCreateInfo const fence_info{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };

        for (u32 index = 0; index < kFramesInFlight; ++index)
        {
            result = vkCreateSemaphore(device.handle(), &semaphore_info,
                                       memory::vulkan_callbacks(), &image_available_[index]);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkCreateSemaphore", result);
                return false;
            }

            result = vkCreateFence(device.handle(), &fence_info, memory::vulkan_callbacks(),
                                   &in_flight_[index]);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkCreateFence", result);
                return false;
            }
        }

        log::info("renderer: clustered deferred, %ux%ux%u clusters, %u lights/cluster max",
                  kClustersX, kClustersY, kClustersZ, kMaxLightsPerCluster);

        return on_swapchain_changed(swapchain);
    }

    bool Renderer::create_targets(VkExtent2D extent)
    {
        VkImageUsageFlags const attachment_sampled =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        for (u32 index = 0; index < kDebugWindowCount; ++index)
        {
            if (!debug_images_[index].init(
                    *allocator_, *device_,
                    {kDebugFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, kDebugImageNames[index]},
                    extent))
            {
                return false;
            }
        }

        return albedo_.init(*allocator_, *device_,
                            {kAlbedoFormat, attachment_sampled, VK_IMAGE_ASPECT_COLOR_BIT,
                             "gbuffer albedo"},
                            extent) &&
               normal_.init(*allocator_, *device_,
                            {kNormalFormat, attachment_sampled, VK_IMAGE_ASPECT_COLOR_BIT,
                             "gbuffer normal"},
                            extent) &&
               material_.init(*allocator_, *device_,
                              {kMaterialFormat, attachment_sampled, VK_IMAGE_ASPECT_COLOR_BIT,
                               "gbuffer material"},
                              extent) &&
               motion_.init(*allocator_, *device_,
                            {kMotionFormat, attachment_sampled, VK_IMAGE_ASPECT_COLOR_BIT,
                             "gbuffer motion"},
                            extent) &&
               depth_.init(*allocator_, *device_,
                           {kDepthFormat,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, "depth"},
                           extent) &&
               hdr_.init(*allocator_, *device_,
                         {kHdrFormat, attachment_sampled | VK_IMAGE_USAGE_STORAGE_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, "hdr"},
                         extent);
    }

    void Renderer::register_targets(bool first_time)
    {
        // Sampled reads always happen in READ_ONLY_OPTIMAL and storage access
        // in GENERAL; the descriptor bakes the layout in, so the frame's
        // transitions must land exactly there.
        VkImageLayout const read_only = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;

        if (first_time)
        {
            albedo_handle_      = bindless_.add_sampled_image(albedo_.view(), read_only);
            normal_handle_      = bindless_.add_sampled_image(normal_.view(), read_only);
            material_handle_    = bindless_.add_sampled_image(material_.view(), read_only);
            motion_handle_      = bindless_.add_sampled_image(motion_.view(), read_only);
            depth_handle_       = bindless_.add_sampled_image(depth_.view(), read_only);
            hdr_sampled_handle_ = bindless_.add_sampled_image(hdr_.view(), read_only);
            hdr_storage_handle_ = bindless_.add_storage_image(hdr_.view());

            for (u32 index = 0; index < kDebugWindowCount; ++index)
            {
                VkImageView const view = debug_images_[index].view();
                debug_sampled_handles_[index] = bindless_.add_sampled_image(view, read_only);
                debug_storage_handles_[index] = bindless_.add_storage_image(view);
            }
            return;
        }

        for (u32 index = 0; index < kDebugWindowCount; ++index)
        {
            VkImageView const view = debug_images_[index].view();
            bindless_.update_sampled_image(debug_sampled_handles_[index], view, read_only);
            bindless_.update_storage_image(debug_storage_handles_[index], view);
        }

        bindless_.update_sampled_image(albedo_handle_, albedo_.view(), read_only);
        bindless_.update_sampled_image(normal_handle_, normal_.view(), read_only);
        bindless_.update_sampled_image(material_handle_, material_.view(), read_only);
        bindless_.update_sampled_image(motion_handle_, motion_.view(), read_only);
        bindless_.update_sampled_image(depth_handle_, depth_.view(), read_only);
        bindless_.update_sampled_image(hdr_sampled_handle_, hdr_.view(), read_only);
        bindless_.update_storage_image(hdr_storage_handle_, hdr_.view());
    }

    bool Renderer::on_swapchain_changed(VulkanSwapchain const& swapchain)
    {
        VkExtent2D const extent = swapchain.extent();

        if (!albedo_.resize(extent) || !normal_.resize(extent) || !material_.resize(extent) ||
            !motion_.resize(extent) || !depth_.resize(extent) || !hdr_.resize(extent))
        {
            return false;
        }

        for (Image& image : debug_images_)
        {
            if (!image.resize(extent))
            {
                return false;
            }
        }
        register_targets(false);

        destroy_image_semaphores();

        VkSemaphoreCreateInfo const semaphore_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };

        render_finished_.resize(swapchain.image_count(), VK_NULL_HANDLE);
        for (VkSemaphore& semaphore : render_finished_)
        {
            VkResult const result = vkCreateSemaphore(device_->handle(), &semaphore_info,
                                                      memory::vulkan_callbacks(), &semaphore);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkCreateSemaphore", result);
                return false;
            }
        }

        return true;
    }

    void Renderer::upload(Scene const& scene, VkExtent2D extent, FrameResources& resources) const
    {
        f64 const aspect = static_cast<f64>(extent.width) / static_cast<f64>(extent.height);

        f64mat4 const view                = scene.camera.view();
        f64mat4 const projection          = scene.camera.projection(aspect);
        f64mat4 const previous_view       = scene.previous_camera.view();
        f64mat4 const previous_projection = scene.previous_camera.projection(aspect);

        // Log-depth slice mapping: slice = log(d) * scale + bias, which puts
        // kClusterNear at 0 and kClusterFar at kClustersZ.
        f32 const log_ratio = std::log(kClusterFar / kClusterNear);
        f32 const scale     = static_cast<f32>(kClustersZ) / log_ratio;
        f32 const bias      = -static_cast<f32>(kClustersZ) * std::log(kClusterNear) / log_ratio;

        // The sun is off: nothing casts shadows yet, so a directional light
        // would leak through every wall of an enclosed corridor. The path is
        // wired and ready for when shadows exist.
        f64vec3 const sun_world = glm::normalize(f64vec3{0.3, 0.8, 0.5});
        f64vec3 const sun_view  = f64vec3{view * f64vec4{sun_world, 0.0}};

        u32 const light_count  = static_cast<u32>(std::min<size_t>(scene.lights.size(), kMaxLights));
        u32 const object_count = static_cast<u32>(std::min<size_t>(scene.objects.size(), kMaxObjects));

        gpu::Frame const frame{
            .projection    = f32mat4{projection},
            .screen        = f32vec4{static_cast<f32>(extent.width), static_cast<f32>(extent.height),
                                     1.0f / static_cast<f32>(extent.width),
                                     1.0f / static_cast<f32>(extent.height)},
            .cluster_depth = f32vec4{kClusterNear, kClusterFar, scale, bias},
            .cluster_grid  = u32vec4{kClustersX, kClustersY, kClustersZ, kMaxLightsPerCluster},
            .sun_direction = f32vec4{f32vec3{glm::normalize(sun_view)}, 0.0f},
            .sun_radiance  = f32vec4{0.0f},
            // Stand-in for bounce light until there is any GI.
            .ambient       = f32vec4{1.2f, 1.3f, 1.5f, 0.0f},
            .counts        = u32vec4{light_count, 0u, 0u, 0u},
        };
        std::memcpy(resources.frame.mapped(), &frame, sizeof(frame));

        // Composing view * model in f64 cancels the large world translations
        // against each other; only then is the result narrowed.
        auto* const objects = static_cast<gpu::Object*>(resources.objects.mapped());
        for (u32 index = 0; index < object_count; ++index)
        {
            SceneObject const& object = scene.objects[index];

            f64mat4 const model_view          = view * object.model;
            f64mat4 const previous_model_view = previous_view * object.previous_model;
            f64mat3 const normal_matrix = glm::transpose(glm::inverse(f64mat3{model_view}));

            gpu::Object const gpu_object{
                .mvp               = f32mat4{projection * model_view},
                .prev_mvp          = f32mat4{previous_projection * previous_model_view},
                .normal_view       = f32mat4{f64mat4{normal_matrix}},
                .albedo_roughness  = f32vec4{object.albedo, object.roughness},
                .emissive_metallic = f32vec4{object.emissive, object.metallic},
            };
            std::memcpy(&objects[index], &gpu_object, sizeof(gpu_object));
        }

        auto* const lights = static_cast<gpu::Light*>(resources.lights.mapped());
        for (u32 index = 0; index < light_count; ++index)
        {
            SceneLight const& light = scene.lights[index];
            f64vec4 const     view_position = view * f64vec4{light.position, 1.0};

            gpu::Light const gpu_light{
                .position_radius  = f32vec4{f32vec3{view_position}, light.radius},
                .colour_intensity = f32vec4{light.colour, light.intensity},
            };
            std::memcpy(&lights[index], &gpu_light, sizeof(gpu_light));
        }
    }

    bool Renderer::record(VkCommandBuffer command, VulkanSwapchain const& swapchain,
                          u32 image_index, Scene const& scene, Overlay* overlay)
    {
        VkCommandBufferBeginInfo const begin{
            .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext            = nullptr,
            .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        };

        VkResult result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkBeginCommandBuffer", result);
            return false;
        }

        VkExtent2D const      extent    = swapchain.extent();
        FrameResources const& resources = frames_[frame_];

        // Each mark() below closes the section since the previous stamp. The
        // G-buffer section also absorbs any wait on the acquire semaphore,
        // which its colour output stage is gated on -- see CLAUDE.md.
        timestamps_.begin(command, frame_);

        gpu::Push push{
            .frame            = resources.frame_handle,
            .objects          = resources.objects_handle,
            .lights           = resources.lights_handle,
            .cluster_counts   = cluster_counts_handle_,
            .cluster_lights   = cluster_lights_handle_,
            .gbuffer_albedo   = albedo_handle_,
            .gbuffer_normal   = normal_handle_,
            .gbuffer_material = material_handle_,
            .gbuffer_depth    = depth_handle_,
            .hdr_storage      = hdr_storage_handle_,
            .hdr_sampled      = hdr_sampled_handle_,
            .object_index     = 0,
            .exposure         = kExposure,
            .debug_view       = static_cast<u32>(debug_view_),
            .gbuffer_motion   = motion_handle_,
            .debug_target     = BindlessSet::kInvalid,
            .ui_transform     = f32vec4{0.0f},
            .ui_texture       = BindlessSet::kInvalid,
            .ui_sampler       = BindlessSet::kInvalid,
            .ui_encode_srgb   = 0,
            .debug_gain       = motion_gain_,
        };

        // The pipeline layouts are identical in their set and push ranges, so
        // binding the set once per bind point serves every pipeline after it.
        VkDescriptorSet const set = bindless_.set();
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                gbuffer_pipeline_.layout(), 0, 1, &set, 0, nullptr);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                lighting_pipeline_.layout(), 0, 1, &set, 0, nullptr);

        // -- 1. entry --------------------------------------------------------
        // The targets are single-copy, so each transition's source stage must
        // cover the *previous* frame's reads as well: lighting read the
        // G-buffer in compute, and tonemap read HDR in the fragment stage. A
        // barrier's first scope reaches back through earlier submissions on
        // the same queue, which is what makes this correct. UNDEFINED discards
        // the old contents, which every pass then clears.
        {
            constexpr VkPipelineStageFlags2 kWrite = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            constexpr VkAccessFlags2 kWriteAccess  = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

            Transition const entry[]{
                {albedo_.handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 kCompute, 0, kWrite, kWriteAccess},
                {normal_.handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 kCompute, 0, kWrite, kWriteAccess},
                {material_.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kCompute, 0, kWrite, kWriteAccess},
                {motion_.handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 kCompute, 0, kWrite, kWriteAccess},
                {hdr_.handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, kWrite, kWriteAccess},
                {depth_.handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 kCompute, 0,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT},
            };
            barrier(command, entry);
        }

        // -- 2. G-buffer -----------------------------------------------------
        {
            // Linear space-black: the corridor is sealed, but anything left
            // uncovered should read as void rather than a debug colour.
            VkRenderingAttachmentInfo const colours[]{
                colour_attachment(albedo_.view(), {{0.0f, 0.0f, 0.0f, 0.0f}}),
                colour_attachment(normal_.view(), {{0.5f, 0.5f, 0.0f, 0.0f}}),
                colour_attachment(material_.view(), {{0.0f, 0.0f, 0.0f, 0.0f}}),
                colour_attachment(motion_.view(), {{0.0f, 0.0f, 0.0f, 0.0f}}),
                colour_attachment(hdr_.view(), {{0.0005f, 0.0007f, 0.0012f, 1.0f}}),
            };

            VkRenderingAttachmentInfo const depth{
                .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext              = nullptr,
                .imageView          = depth_.view(),
                .imageLayout        = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .resolveMode        = VK_RESOLVE_MODE_NONE,
                .resolveImageView   = VK_NULL_HANDLE,
                .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .loadOp             = VK_ATTACHMENT_LOAD_OP_CLEAR,
                // Lighting reads depth to rebuild positions, so it is kept.
                .storeOp            = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue         = {.depthStencil = {kClearDepth, 0}},
            };

            VkRenderingInfo const rendering{
                .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext                = nullptr,
                .flags                = 0,
                .renderArea           = {.offset = {0, 0}, .extent = extent},
                .layerCount           = 1,
                .viewMask             = 0,
                .colorAttachmentCount = static_cast<u32>(std::size(colours)),
                .pColorAttachments    = colours,
                .pDepthAttachment     = &depth,
                .pStencilAttachment   = nullptr,
            };

            vkCmdBeginRendering(command, &rendering);
            set_viewport(command, extent);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, gbuffer_pipeline_.handle());

            u32 const object_count =
                static_cast<u32>(std::min<size_t>(scene.objects.size(), kMaxObjects));
            for (u32 index = 0; index < object_count; ++index)
            {
                push.object_index = index;
                vkCmdPushConstants(command, gbuffer_pipeline_.layout(), VK_SHADER_STAGE_ALL, 0,
                                   sizeof(push), &push);
                cube_.draw(command);
            }

            vkCmdEndRendering(command);
        }

        timestamps_.mark(command, "G-buffer");

        // -- 3. G-buffer -> readable, HDR -> storage --------------------------
        {
            constexpr VkPipelineStageFlags2 kWrite = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            constexpr VkAccessFlags2 kWriteAccess  = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            constexpr VkAccessFlags2 kSampled      = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            constexpr VkImageLayout kReadOnly      = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;

            Transition const reads[]{
                {albedo_.handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kReadOnly, kWrite,
                 kWriteAccess, kCompute, kSampled},
                {normal_.handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kReadOnly, kWrite,
                 kWriteAccess, kCompute, kSampled},
                {material_.handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kReadOnly, kWrite,
                 kWriteAccess, kCompute, kSampled},
                {motion_.handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kReadOnly, kWrite,
                 kWriteAccess, kCompute, kSampled},
                {hdr_.handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                 kWrite, kWriteAccess, kCompute,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                {depth_.handle(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kReadOnly,
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kCompute, kSampled,
                 VK_IMAGE_ASPECT_DEPTH_BIT},
            };

            // The cluster lists were read by last frame's lighting; this
            // frame's build overwrites them. Execution-only for the hazard,
            // but stated as a memory barrier to keep the intent readable.
            VkMemoryBarrier2 const war = compute_to_compute(VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

            barrier(command, reads, span<VkMemoryBarrier2 const>{&war, 1});
        }

        // -- 4. clusters -----------------------------------------------------
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, cluster_pipeline_.handle());
        vkCmdPushConstants(command, cluster_pipeline_.layout(), VK_SHADER_STAGE_ALL, 0,
                           sizeof(push), &push);
        vkCmdDispatch(command, groups(kClusterCount, 64), 1, 1);

        timestamps_.mark(command, "clusters");

        {
            VkMemoryBarrier2 const raw = compute_to_compute(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            barrier(command, {}, span<VkMemoryBarrier2 const>{&raw, 1});
        }

        // -- 5. lighting -----------------------------------------------------
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, lighting_pipeline_.handle());
        vkCmdPushConstants(command, lighting_pipeline_.layout(), VK_SHADER_STAGE_ALL, 0,
                           sizeof(push), &push);
        vkCmdDispatch(command, groups(extent.width, 8), groups(extent.height, 8), 1);

        timestamps_.mark(command, "lighting");

        // -- 6. debug visualisations -----------------------------------------
        // Only for open windows. The inputs -- G-buffer, depth, cluster counts
        // -- were already made visible to compute for lighting. The images
        // themselves were sampled by last frame's UI, so the write here waits
        // on the fragment stage; UNDEFINED is fine because every pixel is
        // rewritten.
        {
            array<Transition, kDebugWindowCount> to_storage{};
            size_t                               count = 0;
            for (u32 index = 0; index < kDebugWindowCount; ++index)
            {
                if (debug_open_[index])
                {
                    to_storage[count++] = Transition{
                        debug_images_[index].handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                }
            }

            if (count > 0)
            {
                barrier(command, span<Transition const>{to_storage.data(), count});

                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, debug_pipeline_.handle());
                for (u32 index = 0; index < kDebugWindowCount; ++index)
                {
                    if (!debug_open_[index])
                    {
                        continue;
                    }

                    push.debug_view   = kFirstDebugWindowView + index;
                    push.debug_target = debug_storage_handles_[index];
                    vkCmdPushConstants(command, debug_pipeline_.layout(), VK_SHADER_STAGE_ALL, 0,
                                       sizeof(push), &push);
                    vkCmdDispatch(command, groups(extent.width, 8), groups(extent.height, 8), 1);
                }

                // Restored: tonemap and later passes read debug_view too.
                push.debug_view = static_cast<u32>(debug_view_);
            }
        }

        // Stamped even when no window is open, so the set of sections is
        // fixed and the stats history does not reset on every toggle.
        timestamps_.mark(command, "debug views");

        // -- 7. HDR and debug images -> sampled, swapchain -> attachment -----
        {
            VkImage const target = swapchain.image(image_index);

            // Filled by assignment: a std::array brace-initialised with only
            // some of its elements draws GCC's -Wmissing-braces.
            array<Transition, 2 + kDebugWindowCount> handoff{};

            handoff[0] = Transition{hdr_.handle(), VK_IMAGE_LAYOUT_GENERAL,
                                    VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

            // The acquire semaphore is waited on at COLOR_ATTACHMENT_OUTPUT,
            // so the transition has to chain from that stage -- TOP_OF_PIPE
            // would let it run before the image is actually available.
            handoff[1] = Transition{target, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
            size_t count = 2;

            // The UI samples these in the overlay pass. A closed window's
            // image is left alone; nothing reads it.
            for (u32 index = 0; index < kDebugWindowCount; ++index)
            {
                if (debug_open_[index])
                {
                    handoff[count++] = Transition{
                        debug_images_[index].handle(), VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
                }
            }

            barrier(command, span<Transition const>{handoff.data(), count});
        }

        // -- 8. tonemap ------------------------------------------------------
        {
            VkRenderingAttachmentInfo colour =
                colour_attachment(swapchain.view(image_index), {{0.0f, 0.0f, 0.0f, 1.0f}});
            // Every pixel is overwritten by the full-screen triangle.
            colour.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

            VkRenderingInfo const rendering{
                .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext                = nullptr,
                .flags                = 0,
                .renderArea           = {.offset = {0, 0}, .extent = extent},
                .layerCount           = 1,
                .viewMask             = 0,
                .colorAttachmentCount = 1,
                .pColorAttachments    = &colour,
                .pDepthAttachment     = nullptr,
                .pStencilAttachment   = nullptr,
            };

            vkCmdBeginRendering(command, &rendering);
            set_viewport(command, extent);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemap_pipeline_.handle());
            vkCmdPushConstants(command, tonemap_pipeline_.layout(), VK_SHADER_STAGE_ALL, 0,
                               sizeof(push), &push);
            vkCmdDraw(command, 3, 1, 0, 0);
            vkCmdEndRendering(command);
        }

        timestamps_.mark(command, "tonemap");

        // -- 9. overlay ------------------------------------------------------
        // A separate rendering scope because it may write through a different
        // view: UNORM over the same sRGB image, so the UI can blend in the
        // space it was designed in. Loading what tonemap stored is a read of
        // the same attachment memory, so its writes must be made visible first.
        if (overlay != nullptr)
        {
            // Texture uploads and vertex data; must precede the scope.
            overlay->prepare(command, frame_);
        }

        {
            constexpr VkPipelineStageFlags2 kOutput = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

            Transition const reload[]{
                {swapchain.image(image_index), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kOutput,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kOutput,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
            };
            barrier(command, reload);

            VkRenderingAttachmentInfo colour =
                colour_attachment(swapchain.ui_view(image_index), {{0.0f, 0.0f, 0.0f, 1.0f}});
            colour.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;

            VkRenderingInfo const rendering{
                .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext                = nullptr,
                .flags                = 0,
                .renderArea           = {.offset = {0, 0}, .extent = extent},
                .layerCount           = 1,
                .viewMask             = 0,
                .colorAttachmentCount = 1,
                .pColorAttachments    = &colour,
                .pDepthAttachment     = nullptr,
                .pStencilAttachment   = nullptr,
            };

            vkCmdBeginRendering(command, &rendering);
            if (overlay != nullptr)
            {
                overlay->record(command);
            }
            vkCmdEndRendering(command);
        }

        timestamps_.mark(command, "UI");

        // -- 10. present -----------------------------------------------------
        {
            Transition const present[]{
                {swapchain.image(image_index), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0},
            };
            barrier(command, present);
        }

        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkEndCommandBuffer", result);
            return false;
        }

        return true;
    }

    FrameResult Renderer::draw(VulkanSwapchain const& swapchain, Scene const& scene,
                               Overlay* overlay)
    {
        VkDevice const device = device_->handle();

        blocked_ms_ = 0.0;
        auto const blocked = [this](auto const& call) {
            auto const start  = Clock::now();
            auto const result = call();
            blocked_ms_ += std::chrono::duration<f64, std::milli>(Clock::now() - start).count();
            return result;
        };

        VkResult result = blocked([&] {
            return vkWaitForFences(device, 1, &in_flight_[frame_], VK_TRUE, kNoTimeout);
        });
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkWaitForFences", result);
            return FrameResult::Error;
        }

        // The fence covers the queries this slot wrote last time round.
        timestamps_.collect(frame_);

        u32 image_index = 0;
        result = blocked([&] {
            return vkAcquireNextImageKHR(device, swapchain.handle(), kNoTimeout,
                                         image_available_[frame_], VK_NULL_HANDLE, &image_index);
        });

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            return FrameResult::OutOfDate;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            log::vk_error("vkAcquireNextImageKHR", result);
            return FrameResult::Error;
        }

        // The fence proved this slot's buffers are no longer read by the GPU,
        // so they can be rewritten now.
        upload(scene, swapchain.extent(), frames_[frame_]);

        // Only reset once the frame is definitely going to be submitted --
        // returning early after this would leave the fence unsignalled forever.
        vkResetFences(device, 1, &in_flight_[frame_]);

        VkCommandBuffer const command = commands_[frame_];
        vkResetCommandBuffer(command, 0);

        if (!record(command, swapchain, image_index, scene, overlay))
        {
            return FrameResult::Error;
        }

        VkSemaphoreSubmitInfo const wait{
            .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext       = nullptr,
            .semaphore   = image_available_[frame_],
            .value       = 0,
            .stageMask   = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .deviceIndex = 0,
        };

        VkSemaphoreSubmitInfo const signal{
            .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext       = nullptr,
            .semaphore   = render_finished_[image_index],
            .value       = 0,
            // ALL_COMMANDS, not ALL_GRAPHICS: the final barrier hands the image
            // to the presentation engine with a NONE destination stage, which
            // ALL_GRAPHICS does not chain from. Sync validation reports that as
            // a PRESENT_AFTER_WRITE hazard.
            .stageMask   = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        VkCommandBufferSubmitInfo const command_info{
            .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .pNext         = nullptr,
            .commandBuffer = command,
            .deviceMask    = 0,
        };

        VkSubmitInfo2 const submit{
            .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext                    = nullptr,
            .flags                    = 0,
            .waitSemaphoreInfoCount   = 1,
            .pWaitSemaphoreInfos      = &wait,
            .commandBufferInfoCount   = 1,
            .pCommandBufferInfos      = &command_info,
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos    = &signal,
        };

        result = vkQueueSubmit2(device_->graphics_queue(), 1, &submit, in_flight_[frame_]);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkQueueSubmit2", result);
            return FrameResult::Error;
        }

        VkSwapchainKHR const chain = swapchain.handle();
        VkPresentInfoKHR const present{
            .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext              = nullptr,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores    = &render_finished_[image_index],
            .swapchainCount     = 1,
            .pSwapchains        = &chain,
            .pImageIndices      = &image_index,
            .pResults           = nullptr,
        };

        // FIFO blocks here, or in the next acquire, once the queue of
        // presentable images is full.
        result = blocked([&] { return vkQueuePresentKHR(device_->present_queue(), &present); });

        frame_ = (frame_ + 1) % kFramesInFlight;

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            return FrameResult::OutOfDate;
        }
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkQueuePresentKHR", result);
            return FrameResult::Error;
        }

        return FrameResult::Ok;
    }

    void Renderer::destroy_image_semaphores()
    {
        if (device_ == nullptr)
        {
            return;
        }

        for (VkSemaphore& semaphore : render_finished_)
        {
            if (semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(device_->handle(), semaphore, memory::vulkan_callbacks());
                semaphore = VK_NULL_HANDLE;
            }
        }

        render_finished_.clear();
    }

    void Renderer::shutdown()
    {
        if (device_ == nullptr)
        {
            return;
        }

        VkDevice const device = device_->handle();

        timestamps_.shutdown();

        gbuffer_pipeline_.shutdown();
        tonemap_pipeline_.shutdown();
        cluster_pipeline_.shutdown();
        lighting_pipeline_.shutdown();
        debug_pipeline_.shutdown();

        cube_.shutdown();

        for (FrameResources& resources : frames_)
        {
            resources.frame.shutdown();
            resources.objects.shutdown();
            resources.lights.shutdown();
        }

        cluster_counts_.shutdown();
        cluster_lights_.shutdown();

        albedo_.shutdown();
        normal_.shutdown();
        material_.shutdown();
        motion_.shutdown();
        depth_.shutdown();
        hdr_.shutdown();

        for (Image& image : debug_images_)
        {
            image.shutdown();
        }

        bindless_.shutdown();

        destroy_image_semaphores();

        for (u32 index = 0; index < kFramesInFlight; ++index)
        {
            if (image_available_[index] != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(device, image_available_[index], memory::vulkan_callbacks());
                image_available_[index] = VK_NULL_HANDLE;
            }

            if (in_flight_[index] != VK_NULL_HANDLE)
            {
                vkDestroyFence(device, in_flight_[index], memory::vulkan_callbacks());
                in_flight_[index] = VK_NULL_HANDLE;
            }
        }

        if (command_pool_ != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(device, command_pool_, memory::vulkan_callbacks());
            command_pool_ = VK_NULL_HANDLE;
        }

        device_    = nullptr;
        allocator_ = nullptr;
    }
}
