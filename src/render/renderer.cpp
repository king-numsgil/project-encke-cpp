#include "core/pch.hpp"

#include "render/renderer.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "render/config.hpp"
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
            "debug: cluster heat", "debug: normals", "debug: motion", "debug: cascades",
        };

        constexpr VkFormat kGBufferFormats[]{
            kAlbedoFormat, kNormalFormat, kMaterialFormat, kMotionFormat, kHdrFormat,
        };

        // Reversed-Z. Near maps to 1.0, infinity to 0.0; clear to the far
        // value and test GREATER. See CLAUDE.md -- all four must agree.
        constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
        constexpr f32      kClearDepth  = 0.0f;

        // Shadow maps are reversed-Z too, on the same format and clear.
        constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

        using config::kClusterCount;
        using config::kClusterFar;
        using config::kClusterNear;
        using config::kClustersX;
        using config::kClustersY;
        using config::kClustersZ;
        using config::kMaxLights;
        using config::kMaxLightsPerCluster;
        using config::kMaxObjects;
        using config::kShadowViewCount;

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
            array<VkImageMemoryBarrier2, 16> images{};
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

        VkRenderingAttachmentInfo depth_attachment(VkImageView view)
        {
            return VkRenderingAttachmentInfo{
                .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext              = nullptr,
                .imageView          = view,
                .imageLayout        = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .resolveMode        = VK_RESOLVE_MODE_NONE,
                .resolveImageView   = VK_NULL_HANDLE,
                .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .loadOp             = VK_ATTACHMENT_LOAD_OP_CLEAR,
                // Read later by lighting, so kept.
                .storeOp            = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue         = {.depthStencil = {kClearDepth, 0}},
            };
        }

        // 1 / (1.2 * 2^EV100): the saturation-based exposure for a camera set
        // to that EV at ISO 100.
        f32 exposure_from_ev100(f32 ev100)
        {
            return 1.0f / (1.2f * std::exp2(ev100));
        }

        // One mesh, one instance: `instance` reaches the shader as
        // SV_VulkanInstanceID and says which object, or matrix, it is.
        VkDrawIndexedIndirectCommand draw_command(GeometryPool::Range const& range, u32 instance)
        {
            return VkDrawIndexedIndirectCommand{
                .indexCount    = range.index_count,
                .instanceCount = 1,
                .firstIndex    = range.first_index,
                .vertexOffset  = static_cast<i32>(range.first_vertex),
                .firstInstance = instance,
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
            {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
             .offset = offsetof(Vertex, tangent)},
            {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
             .offset = offsetof(Vertex, uv)},
        };

        // The shadow pass reads position only, from the same vertex buffers.
        constexpr VkVertexInputAttributeDescription kShadowAttributes[]{
            {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
             .offset = offsetof(Vertex, position)},
        };

        // Tessellation of the built-in meshes.
        constexpr u32 kSphereSlices     = 48;
        constexpr u32 kSphereStacks     = 24;
        constexpr u32 kPlanetSlices     = 192;
        constexpr f64 kPlanetFirstRing  = 0.25;   // metres of arc from the pole
        constexpr f64 kPlanetRingGrowth = 1.08;
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

            if (!resources.shadow_views.init_mapped(
                    allocator, kShadowViewCount * sizeof(gpu::ShadowView), storage) ||
                !resources.shadow_matrices.init_mapped(
                    allocator, u64{kShadowViewCount} * kMaxObjects * sizeof(f32mat4), storage))
            {
                return false;
            }

            if (!resources.draws.init_mapped(
                    allocator, u64{1 + kShadowViewCount} * kMaxObjects *
                                   sizeof(VkDrawIndexedIndirectCommand),
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT))
            {
                return false;
            }

            resources.shadow_views_handle = bindless_.add_storage_buffer(
                resources.shadow_views.handle(), resources.shadow_views.size());
            resources.shadow_matrices_handle = bindless_.add_storage_buffer(
                resources.shadow_matrices.handle(), resources.shadow_matrices.size());
        }

        if (!create_shadow_maps())
        {
            return false;
        }

        // Zeroed once here; after that the adapt pass clears it each frame.
        vector<u32> const empty_histogram(config::kExposureBins, 0u);
        if (!exposure_histogram_.init_device(allocator, device,
                                             config::kExposureBins * sizeof(u32), storage,
                                             empty_histogram.data()) ||
            !exposure_image_.init(allocator, device,
                                  {VK_FORMAT_R32_SFLOAT,
                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, "exposure"},
                                  VkExtent2D{1, 1}))
        {
            return false;
        }

        exposure_histogram_handle_ =
            bindless_.add_writable_buffer(exposure_histogram_.handle(), exposure_histogram_.size());
        exposure_storage_handle_ = bindless_.add_storage_image(exposure_image_.view());
        exposure_sampled_handle_ =
            bindless_.add_sampled_image(exposure_image_.view(), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);

        if (!geometry_.init(allocator, device, config::kGeometryVertexCapacity,
                            config::kGeometryIndexCapacity, kFramesInFlight))
        {
            return false;
        }

        vector<Vertex> vertices;
        vector<u32>    indices;

        // First into an empty pool, in MeshKind order, so a MeshKind is its
        // own mesh id. They upload with the first frame.
        u32  expected = 0;
        auto add_mesh = [&] {
            optional<u32> const mesh = geometry_.add(vertices, indices);
            return mesh.has_value() && *mesh == expected++;
        };

        build_cube(vertices, indices);
        if (!add_mesh())
        {
            return false;
        }

        build_sphere(vertices, indices, kSphereSlices, kSphereStacks);
        if (!add_mesh())
        {
            return false;
        }

        build_planet(vertices, indices, kEarthRadius, kPlanetSlices, kPlanetFirstRing,
                     kPlanetRingGrowth);
        if (!add_mesh())
        {
            return false;
        }

        if (!create_materials())
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

        // Depth only. Clamped, so cascade casters nearer the sun than the near
        // plane flatten onto it and still cast; biased per pass.
        GraphicsPipeline::Config const shadow_config{
            .spirv_name         = "shadow_depth.spv",
            .fragment_entry     = nullptr,
            .colour_formats     = {},
            .depth_format       = kShadowFormat,
            .bindings           = kVertexBindings,
            .attributes         = kShadowAttributes,
            .cull_mode          = VK_CULL_MODE_BACK_BIT,
            .depth_clamp        = true,
            .depth_bias         = true,
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

        ComputePipeline::Config const histogram_config{
            .spirv_name         = "exposure.spv",
            .entry              = "histogram_main",
            .set_layout         = bindless_.layout(),
            .push_constant_size = sizeof(gpu::Push),
        };

        ComputePipeline::Config const adapt_config{
            .spirv_name         = "exposure.spv",
            .entry              = "adapt_main",
            .set_layout         = bindless_.layout(),
            .push_constant_size = sizeof(gpu::Push),
        };

        if (!gbuffer_pipeline_.init(device, gbuffer_config) ||
            !shadow_pipeline_.init(device, shadow_config) ||
            !tonemap_pipeline_.init(device, tonemap_config) ||
            !cluster_pipeline_.init(device, cluster_config) ||
            !lighting_pipeline_.init(device, lighting_config) ||
            !debug_pipeline_.init(device, debug_config) ||
            !histogram_pipeline_.init(device, histogram_config) ||
            !adapt_pipeline_.init(device, adapt_config))
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
        log::info("shadows: %u sun cascades at %u^2 to %.0f m, up to %u spots at %u^2",
                  config::kCascadeCount, config::kCascadeResolution,
                  static_cast<f64>(config::kShadowDistance), config::kMaxShadowedSpots,
                  config::kSpotShadowResolution);

        return on_swapchain_changed(swapchain);
    }

    bool Renderer::create_shadow_maps()
    {
        for (u32 index = 0; index < kShadowViewCount; ++index)
        {
            bool const cascade = index < config::kCascadeCount;
            u32 const  size    = cascade ? config::kCascadeResolution : config::kSpotShadowResolution;

            if (!shadow_maps_[index].init(
                    *allocator_, *device_,
                    {kShadowFormat,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT, cascade ? "shadow cascade" : "shadow spot"},
                    VkExtent2D{size, size}))
            {
                return false;
            }

            shadow_map_handles_[index] =
                bindless_.add_sampled_image(shadow_maps_[index].view(),
                                            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
        }

        // Hardware PCF: each tap is a bilinear blend of four comparisons.
        // GREATER_OR_EQUAL because depth is reversed; the black border reads
        // as depth 0, infinitely far, so off-map lookups come out lit.
        VkSamplerCreateInfo const info{
            .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext                   = nullptr,
            .flags                   = 0,
            .magFilter               = VK_FILTER_LINEAR,
            .minFilter               = VK_FILTER_LINEAR,
            .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .mipLodBias              = 0.0f,
            .anisotropyEnable        = VK_FALSE,
            .maxAnisotropy           = 1.0f,
            .compareEnable           = VK_TRUE,
            .compareOp               = VK_COMPARE_OP_GREATER_OR_EQUAL,
            .minLod                  = 0.0f,
            .maxLod                  = 0.0f,
            .borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };

        VkResult const result = vkCreateSampler(device_->handle(), &info,
                                                memory::vulkan_callbacks(), &shadow_sampler_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateSampler (shadow)", result);
            return false;
        }
        shadow_sampler_handle_ = bindless_.add_sampler(shadow_sampler_);

        return true;
    }

    bool Renderer::create_materials()
    {
        // Trilinear and anisotropic, repeating: surface textures seen at
        // grazing angles across the ground would blur to mush without it.
        VkSamplerCreateInfo const info{
            .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext                   = nullptr,
            .flags                   = 0,
            .magFilter               = VK_FILTER_LINEAR,
            .minFilter               = VK_FILTER_LINEAR,
            .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU            = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV            = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW            = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .mipLodBias              = 0.0f,
            .anisotropyEnable        = VK_TRUE,
            .maxAnisotropy           = std::min(config::kMaxAnisotropy,
                                                device_->properties().limits.maxSamplerAnisotropy),
            .compareEnable           = VK_FALSE,
            .compareOp               = VK_COMPARE_OP_ALWAYS,
            .minLod                  = 0.0f,
            .maxLod                  = VK_LOD_CLAMP_NONE,
            .borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };

        VkResult const result = vkCreateSampler(device_->handle(), &info,
                                                memory::vulkan_callbacks(), &material_sampler_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateSampler (material)", result);
            return false;
        }
        material_sampler_handle_ = bindless_.add_sampler(material_sampler_);

        VkImageLayout const read_only = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;

        array<u8, 4> const white{{255, 255, 255, 255}};
        if (!white_srgb_.init(*allocator_, *device_, {VK_FORMAT_R8G8B8A8_SRGB, "white srgb"}, 1, 1,
                              white) ||
            !white_unorm_.init(*allocator_, *device_, {VK_FORMAT_R8G8B8A8_UNORM, "white unorm"}, 1,
                               1, white))
        {
            return false;
        }
        white_srgb_handle_  = bindless_.add_sampled_image(white_srgb_.view(), read_only);
        white_unorm_handle_ = bindless_.add_sampled_image(white_unorm_.view(), read_only);

        if (!staging_.init(*allocator_, config::kStagingBytesPerFrame, kFramesInFlight))
        {
            return false;
        }

        // In MaterialKind order, so a MaterialKind is its own material id.
        // None has no textures. The rest start untextured -- kNoTexture, so
        // their objects draw with flat factors -- and fill in as the loader
        // finishes them.
        materials_.clear();
        materials_.push_back(nullptr);
        loader_.start();

        for (u32 index = 1; index < kMaterialKindCount; ++index)
        {
            auto const kind = static_cast<MaterialKind>(index);

            materials_.push_back(std::make_unique<MaterialTextures>());
            MaterialTextures& material = *materials_.back();
            material.tile   = material_tile_metres(kind);
            material.tiling = true;

            loader_.submit(MaterialLoader::Job{
                .material = index,
                .name     = material_name(kind),
                .decode   = [kind](MaterialMaps& maps) { return load_material(kind, maps); },
            });
        }

        return true;
    }

    void Renderer::stream_materials()
    {
        for (MaterialLoader::Decoded& decoded : loader_.take())
        {
            decoded_.push_back(StreamingMaterial{.decoded = std::move(decoded), .staged = 0});
        }

        VkImageLayout const read_only = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;

        // In completion order, map by map, as many as fit. A map that does
        // not fit waits, and so does everything behind it, so materials land
        // in the order they were decoded. A material's handles go live only
        // with its last map, so nothing samples a half-uploaded set; the
        // earlier maps' copies were recorded in earlier frames, which the
        // queue has ordered ahead of this one.
        size_t done = 0;
        for (; done < decoded_.size(); ++done)
        {
            StreamingMaterial&             entry   = decoded_[done];
            MaterialLoader::Decoded const& decoded = entry.decoded;
            if (!decoded.ok)
            {
                continue;   // the loader logged why; the material stays flat
            }

            MaterialTextures& material = *materials_[decoded.material];

            struct Map
            {
                Texture&                texture;
                VkFormat                format;
                char const*             name;
                optional<Pixels> const& pixels;
            };
            Map const maps[]{
                {material.albedo, VK_FORMAT_R8G8B8A8_SRGB, "material albedo", decoded.maps.albedo},
                {material.normal, VK_FORMAT_R8G8B8A8_UNORM, "material normal", decoded.maps.normal},
                {material.orm, VK_FORMAT_R8G8B8A8_UNORM, "material orm", decoded.maps.orm},
                {material.emission, VK_FORMAT_R8G8B8A8_SRGB, "material emission",
                 decoded.maps.emission},
            };

            bool out_of_budget = false;
            bool failed        = false;
            for (; entry.staged < std::size(maps); ++entry.staged)
            {
                Map const& map = maps[entry.staged];
                if (!map.pixels.has_value())
                {
                    continue;
                }

                Pixels const&      pixels = *map.pixels;
                VkDeviceSize const bytes  = Texture::upload_bytes(pixels.width, pixels.height);

                // 16 bytes of slack covers the allocation's alignment.
                if (bytes + 16 > staging_.bytes_per_slot())
                {
                    log::error("material %s: a %ux%u map needs %llu bytes of staging, over the "
                               "%llu a frame allows", decoded.name.c_str(), pixels.width,
                               pixels.height, static_cast<unsigned long long>(bytes),
                               static_cast<unsigned long long>(staging_.bytes_per_slot()));
                    failed = true;
                    break;
                }

                optional<StagingArena::Allocation> const staged = staging_.allocate(bytes);
                if (!staged.has_value())
                {
                    out_of_budget = true;
                    break;
                }
                if (!map.texture.create(*allocator_, *device_, {map.format, map.name},
                                        pixels.width, pixels.height))
                {
                    failed = true;
                    break;
                }

                std::memcpy(staged->data, pixels.rgba.data(), bytes);
                pending_uploads_.push_back(
                    PendingUpload{.texture = &map.texture, .buffer = staged->buffer,
                                  .offset = staged->offset});
            }

            if (out_of_budget)
            {
                break;
            }
            if (failed)
            {
                // Maps already uploaded are never sampled; they are freed
                // with the rest of the materials.
                log::error("material %s stays untextured", decoded.name.c_str());
                continue;
            }

            // Registered now, sampled from this very frame: the copies are
            // recorded ahead of every pass, and the slots are fresh, so no
            // pending command buffer can be reading them. Albedo and ORM are
            // always sampled once an object is textured, so a missing one is
            // white; normal and emission are skipped.
            auto handle = [&](Map const& map, u32 missing) {
                return map.pixels.has_value() ? bindless_.add_sampled_image(map.texture.view(),
                                                                            read_only)
                                              : missing;
            };
            material.handles = u32vec4{
                handle(maps[0], white_srgb_handle_),
                handle(maps[1], gpu::kNoTexture),
                handle(maps[2], white_unorm_handle_),
                handle(maps[3], gpu::kNoTexture),
            };
            material.hide_emission = false;
            log::info("material %s streamed in", decoded.name.c_str());
        }

        decoded_.erase(decoded_.begin(), decoded_.begin() + static_cast<std::ptrdiff_t>(done));
    }

    void Renderer::record_uploads(VkCommandBuffer command)
    {
        geometry_.record(command);

        for (PendingUpload const& upload : pending_uploads_)
        {
            upload.texture->record_upload(command, upload.buffer, upload.offset);
        }
        pending_uploads_.clear();
    }

    optional<Model> Renderer::add_model(GltfModel const& source)
    {
        // Textures stream in like the built-in materials: each material
        // starts untextured, with its emission held back if a map is meant
        // to mask it, and its decode job goes to the loader.
        u32 const first_material = static_cast<u32>(materials_.size());
        for (size_t index = 0; index < source.materials.size(); ++index)
        {
            GltfMaterial const& material = source.materials[index];

            materials_.push_back(std::make_unique<MaterialTextures>());
            MaterialTextures& textures = *materials_.back();
            textures.tiling        = false;
            textures.hide_emission = material.has_emission_map;

            if (material.decode)
            {
                loader_.submit(MaterialLoader::Job{
                    .material = static_cast<u32>(materials_.size() - 1),
                    .name     = "gltf material " + std::to_string(index),
                    .decode   = material.decode,
                });
            }
        }

        Model model;
        model.min = f64vec3{source.min};
        model.max = f64vec3{source.max};

        for (GltfPrimitive const& primitive : source.primitives)
        {
            optional<u32> const mesh = geometry_.add(primitive.vertices, primitive.indices);
            if (!mesh.has_value())
            {
                return nullopt;
            }

            GltfMaterial const& material = source.materials[primitive.material];
            f64vec3 const       min{primitive.min};
            f64vec3 const       max{primitive.max};

            model.parts.push_back(ModelPart{
                .mesh          = *mesh,
                .material      = first_material + primitive.material,
                .albedo        = material.base_colour,
                .roughness     = material.roughness,
                .metallic      = material.metallic,
                .emissive      = material.emissive,
                .bounds_centre = (min + max) * 0.5,
                .bounds_radius = glm::length(max - min) * 0.5,
            });
        }

        return model;
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

    void Renderer::upload(Scene const& scene, VkExtent2D extent, FrameResources& resources)
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

        // Direction and strength of the star from where the camera is: the
        // whole scene is small against an astronomical unit, so one direction
        // and one illuminance serve every pixel.
        f64vec3 const sun_world   = scene.star.direction_from(scene.camera.position);
        f64vec3 const sun_view    = f64vec3{view * f64vec4{sun_world, 0.0}};
        f32 const     illuminance = static_cast<f32>(scene.star.illuminance_at(scene.camera.position));

        // The environment's ground is a Lambertian plane lit by the star:
        // radiance albedo * E * cos(elevation) / pi, zero once the star sets.
        constexpr f64 kPi = 3.14159265358979323846;

        f64vec3 const up_world  = scene.up_at(scene.camera.position);
        f64vec3 const up_view   = glm::normalize(f64vec3{view * f64vec4{up_world, 0.0}});
        f64 const     sun_up    = std::max(glm::dot(up_world, sun_world), 0.0);
        f32vec3 const ground    = scene.ground_albedo * scene.star.colour *
                                  static_cast<f32>(static_cast<f64>(illuminance) * sun_up / kPi);
        f32vec3 const sky       = ground * scene.sky_fill;

        u32 const light_count  = static_cast<u32>(std::min<size_t>(scene.lights.size(), kMaxLights));
        u32 const object_count = static_cast<u32>(std::min<size_t>(scene.objects.size(), kMaxObjects));

        plan_shadows(scene, aspect, shadow_plan_);

        f32 const shadow_far = config::kShadowDistance;

        gpu::Frame const frame{
            .projection    = f32mat4{projection},
            .screen        = f32vec4{static_cast<f32>(extent.width), static_cast<f32>(extent.height),
                                     1.0f / static_cast<f32>(extent.width),
                                     1.0f / static_cast<f32>(extent.height)},
            .cluster_depth = f32vec4{kClusterNear, kClusterFar, scale, bias},
            .cluster_grid  = u32vec4{kClustersX, kClustersY, kClustersZ, kMaxLightsPerCluster},
            .sun_direction = f32vec4{f32vec3{glm::normalize(sun_view)}, 0.0f},
            .sun_radiance  = f32vec4{scene.star.colour * illuminance, 0.0f},
            .env_up        = f32vec4{f32vec3{up_view}, 0.0f},
            .env_sky       = f32vec4{sky, 0.0f},
            .env_ground    = f32vec4{ground, 0.0f},
            .counts        = u32vec4{light_count, shadow_plan_.cascade_count, 0u, 0u},
            .shadow        = f32vec4{shadow_far, shadow_far * (1.0f - config::kShadowFadeFraction),
                                     config::kShadowNormalOffset, 0.0f},
            .exposure_range  = f32vec4{config::kExposureLogMin, config::kExposureLogRange,
                                       config::kExposureLowPercentile,
                                       config::kExposureHighPercentile},
            .exposure_adapt  = f32vec4{static_cast<f32>(frame_seconds_),
                                       config::kExposureSpeedBrighter, config::kExposureSpeedDarker,
                                       config::kExposureCompensation},
            // Nothing stored yet on the first frame, so it jumps too.
            .exposure_limits = f32vec4{config::kExposureMinEv, config::kExposureMaxEv,
                                       exposure_jump_ || !exposure_written_ ? 1.0f : 0.0f,
                                       scene.ev100},
        };
        std::memcpy(resources.frame.mapped(), &frame, sizeof(frame));

        // Each map's lookup goes view -> world -> light clip. The inverse view
        // carries the camera's large world position, which the light's view
        // cancels; composed in f64, only the small result is narrowed.
        f64mat4 const view_to_world = glm::inverse(view);

        u32 const shadow_view_count = shadow_plan_.cascade_count + shadow_plan_.spot_count;
        auto* const shadow_views = static_cast<gpu::ShadowView*>(resources.shadow_views.mapped());
        auto* const shadow_matrices = static_cast<f32mat4*>(resources.shadow_matrices.mapped());
        auto* const draws = static_cast<VkDrawIndexedIndirectCommand*>(resources.draws.mapped());
        gbuffer_draws_    = 0;

        for (u32 index = 0; index < shadow_view_count; ++index)
        {
            ShadowMapView const& map = shadow_plan_.views[index];
            f64mat4 const world_to_clip = map.projection * map.light_view;

            gpu::ShadowView const gpu_view{
                .view_to_clip = f32mat4{world_to_clip * view_to_world},
                .params       = f32vec4{static_cast<f32>(map.far_distance),
                                        static_cast<f32>(map.texel), map.strength,
                                        map.perspective ? 1.0f : 0.0f},
                .image        = u32vec4{shadow_map_handles_[index], 0u, 0u, 0u},
            };
            std::memcpy(&shadow_views[index], &gpu_view, sizeof(gpu_view));

            // One indirect draw per caster, its firstInstance the matrix slot.
            u32& casters = shadow_draws_[index];
            casters      = 0;
            for (u32 object = 0; object < object_count; ++object)
            {
                SceneObject const& scene_object = scene.objects[object];
                if (!geometry_.range(scene_object.mesh).resident ||
                    !map.may_cast(scene_object.bounds_centre, scene_object.bounds_radius))
                {
                    continue;
                }

                u32 const slot = index * kMaxObjects + object;
                f32mat4 const matrix{world_to_clip * scene_object.model};
                std::memcpy(&shadow_matrices[slot], &matrix, sizeof(matrix));

                draws[(1 + index) * kMaxObjects + casters++] =
                    draw_command(geometry_.range(scene_object.mesh), slot);
            }
        }
        for (u32 index = shadow_view_count; index < kShadowViewCount; ++index)
        {
            shadow_draws_[index] = 0;
        }

        // Composing view * model in f64 cancels the large world translations
        // against each other; only then is the result narrowed.
        auto* const objects = static_cast<gpu::Object*>(resources.objects.mapped());
        for (u32 index = 0; index < object_count; ++index)
        {
            SceneObject const& object = scene.objects[index];

            f64mat4 const model_view          = view * object.model;
            f64mat4 const previous_model_view = previous_view * object.previous_model;
            f64mat3 const normal_matrix = glm::transpose(glm::inverse(f64mat3{model_view}));

            u32vec4 textures{gpu::kNoTexture};
            f32vec4 texture_scale{0.0f};
            bool    hide_emission = false;
            if (object.material != 0)
            {
                MaterialTextures const& material = *materials_[object.material];
                textures      = material.handles;
                hide_emission = material.hide_emission;

                // (1, 1, 1, 1) makes the shader's stretch exactly 1, which
                // leaves an atlas's UVs alone whatever the object's scale.
                texture_scale = material.tiling
                                    ? f32vec4{f32vec3{object.scale} / material.tile.x,
                                              material.tile.x / material.tile.y}
                                    : f32vec4{1.0f};
            }

            gpu::Object const gpu_object{
                .mvp               = f32mat4{projection * model_view},
                .prev_mvp          = f32mat4{previous_projection * previous_model_view},
                .model_view        = f32mat4{model_view},
                .normal_view       = f32mat4{f64mat4{normal_matrix}},
                .albedo_roughness  = f32vec4{object.albedo, object.roughness},
                .emissive_metallic = f32vec4{hide_emission ? f32vec3{0.0f} : object.emissive,
                                             object.metallic},
                .textures          = textures,
                .texture_scale     = texture_scale,
            };
            std::memcpy(&objects[index], &gpu_object, sizeof(gpu_object));

            // A mesh still waiting for its upload is left out, not drawn
            // from memory that holds nothing yet.
            if (geometry_.range(object.mesh).resident)
            {
                draws[gbuffer_draws_++] = draw_command(geometry_.range(object.mesh), index);
            }
        }

        auto* const lights = static_cast<gpu::Light*>(resources.lights.mapped());
        for (u32 index = 0; index < light_count; ++index)
        {
            SceneLight const& light = scene.lights[index];
            f64vec4 const     view_position  = view * f64vec4{light.position, 1.0};
            f64vec3 const     view_direction = f64vec3{view * f64vec4{light.direction, 0.0}};

            u32 shadow = gpu::kNoShadow;
            for (u32 slot = 0; slot < shadow_plan_.spot_count; ++slot)
            {
                if (shadow_plan_.spot_lights[slot] == index)
                {
                    shadow = config::kCascadeCount + slot;
                }
            }

            gpu::Light const gpu_light{
                .position_radius     = f32vec4{f32vec3{view_position}, light.radius},
                .colour_intensity    = f32vec4{light.colour, light.intensity},
                .direction_cos_outer = f32vec4{f32vec3{glm::normalize(view_direction)},
                                               light.cos_outer},
                .cos_inner           = light.cos_inner,
                .shadow              = shadow,
                .pad0                = 0,
                .pad1                = 0,
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

        // -- 0. uploads ------------------------------------------------------
        // Before every pass, so anything uploaded is readable this frame.
        // Each texture's own barriers end in READ_ONLY_OPTIMAL for the
        // fragment stage, which is where materials are sampled.
        record_uploads(command);
        timestamps_.mark(command, "uploads");

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
            .pad0             = 0,
            .exposure         = exposure_from_ev100(scene.ev100),
            .debug_view       = static_cast<u32>(debug_view_),
            .gbuffer_motion   = motion_handle_,
            .debug_target     = BindlessSet::kInvalid,
            .ui_transform     = f32vec4{0.0f},
            .ui_texture       = BindlessSet::kInvalid,
            .ui_sampler       = BindlessSet::kInvalid,
            .ui_encode_srgb   = 0,
            .debug_gain       = motion_gain_,
            .shadow_views     = resources.shadow_views_handle,
            .shadow_matrices  = resources.shadow_matrices_handle,
            .shadow_sampler     = shadow_sampler_handle_,
            .exposure_image     = BindlessSet::kInvalid,
            .exposure_histogram = exposure_histogram_handle_,
            .material_sampler   = material_sampler_handle_,
            .tonemap            = static_cast<u32>(tonemap_),
        };

        u32 const shadow_view_count = shadow_plan_.cascade_count + shadow_plan_.spot_count;

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
            constexpr VkPipelineStageFlags2 kDepthTests =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            constexpr VkAccessFlags2 kDepthWrite = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

            array<Transition, 6 + kShadowViewCount> entry{};
            entry[0] = {albedo_.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kCompute, 0, kWrite, kWriteAccess};
            entry[1] = {normal_.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kCompute, 0, kWrite, kWriteAccess};
            entry[2] = {material_.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kCompute, 0, kWrite, kWriteAccess};
            entry[3] = {motion_.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kCompute, 0, kWrite, kWriteAccess};
            entry[4] = {hdr_.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, kWrite, kWriteAccess};
            entry[5] = {depth_.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kCompute, 0, kDepthTests,
                        kDepthWrite, VK_IMAGE_ASPECT_DEPTH_BIT};
            size_t count = 6;

            // Shadow maps this frame draws. Last frame's lighting and debug
            // views sampled them in compute.
            for (u32 index = 0; index < shadow_view_count; ++index)
            {
                entry[count++] = {shadow_maps_[index].handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kCompute, 0,
                                  kDepthTests, kDepthWrite, VK_IMAGE_ASPECT_DEPTH_BIT};
            }

            barrier(command, span<Transition const>{entry.data(), count});
        }

        // -- 2. shadow maps --------------------------------------------------
        // Depth only, before the G-buffer. Nothing here waits on the acquire
        // semaphore, which gates colour output only.
        // Every pass draws from the one geometry pool, bound once here: vertex
        // and index buffers are command buffer state, not pipeline state.
        constexpr VkDeviceSize kDrawStride = sizeof(VkDrawIndexedIndirectCommand);
        geometry_.bind(command);

        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_.handle());
        vkCmdPushConstants(command, shadow_pipeline_.layout(), VK_SHADER_STAGE_ALL, 0,
                           sizeof(push), &push);
        for (u32 index = 0; index < shadow_view_count; ++index)
        {
            u32 const  size = index < config::kCascadeCount ? config::kCascadeResolution
                                                            : config::kSpotShadowResolution;
            VkExtent2D const map_extent{size, size};

            VkRenderingAttachmentInfo const depth = depth_attachment(shadow_maps_[index].view());

            VkRenderingInfo const rendering{
                .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext                = nullptr,
                .flags                = 0,
                .renderArea           = {.offset = {0, 0}, .extent = map_extent},
                .layerCount           = 1,
                .viewMask             = 0,
                .colorAttachmentCount = 0,
                .pColorAttachments    = nullptr,
                .pDepthAttachment     = &depth,
                .pStencilAttachment   = nullptr,
            };

            vkCmdBeginRendering(command, &rendering);
            // Flipped like the frame, so lookups share ndc_to_uv with it.
            set_viewport(command, map_extent);
            vkCmdSetDepthBias(command, config::kShadowBiasConstant, 0.0f,
                              config::kShadowBiasSlope);

            // Still begun and cleared with no casters: lighting samples every
            // map the plan names.
            if (shadow_draws_[index] > 0)
            {
                vkCmdDrawIndexedIndirect(command, resources.draws.handle(),
                                         (1 + index) * kMaxObjects * kDrawStride,
                                         shadow_draws_[index], static_cast<u32>(kDrawStride));
            }

            vkCmdEndRendering(command);
        }

        timestamps_.mark(command, "shadows");

        // -- 3. G-buffer -----------------------------------------------------
        {
            // Linear space-black: empty sky reads as void rather than a debug
            // colour. At daylight exposure it tonemaps to pure black.
            VkRenderingAttachmentInfo const colours[]{
                colour_attachment(albedo_.view(), {{0.0f, 0.0f, 0.0f, 0.0f}}),
                colour_attachment(normal_.view(), {{0.5f, 0.5f, 0.0f, 0.0f}}),
                colour_attachment(material_.view(), {{0.0f, 0.0f, 0.0f, 0.0f}}),
                colour_attachment(motion_.view(), {{0.0f, 0.0f, 0.0f, 0.0f}}),
                colour_attachment(hdr_.view(), {{0.0005f, 0.0007f, 0.0012f, 1.0f}}),
            };

            // Lighting reads depth to rebuild positions, so it is kept.
            VkRenderingAttachmentInfo const depth = depth_attachment(depth_.view());

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
            vkCmdPushConstants(command, gbuffer_pipeline_.layout(), VK_SHADER_STAGE_ALL, 0,
                               sizeof(push), &push);

            if (gbuffer_draws_ > 0)
            {
                vkCmdDrawIndexedIndirect(command, resources.draws.handle(), 0, gbuffer_draws_,
                                         static_cast<u32>(kDrawStride));
            }

            vkCmdEndRendering(command);
        }

        timestamps_.mark(command, "G-buffer");

        // -- 4. G-buffer and shadow maps -> readable, HDR -> storage ----------
        {
            constexpr VkPipelineStageFlags2 kWrite = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            constexpr VkAccessFlags2 kWriteAccess  = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            constexpr VkAccessFlags2 kSampled      = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            constexpr VkImageLayout kReadOnly      = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
            constexpr VkPipelineStageFlags2 kDepthStored = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            constexpr VkAccessFlags2 kDepthWrite = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

            array<Transition, 6 + kShadowViewCount> reads{};
            reads[0] = {albedo_.handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kReadOnly,
                        kWrite, kWriteAccess, kCompute, kSampled};
            reads[1] = {normal_.handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kReadOnly,
                        kWrite, kWriteAccess, kCompute, kSampled};
            reads[2] = {material_.handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kReadOnly,
                        kWrite, kWriteAccess, kCompute, kSampled};
            reads[3] = {motion_.handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kReadOnly,
                        kWrite, kWriteAccess, kCompute, kSampled};
            reads[4] = {hdr_.handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL, kWrite, kWriteAccess, kCompute,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
            reads[5] = {depth_.handle(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kReadOnly,
                        kDepthStored, kDepthWrite, kCompute, kSampled, VK_IMAGE_ASPECT_DEPTH_BIT};
            size_t count = 6;

            for (u32 index = 0; index < shadow_view_count; ++index)
            {
                reads[count++] = {shadow_maps_[index].handle(),
                                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kReadOnly, kDepthStored,
                                  kDepthWrite, kCompute, kSampled, VK_IMAGE_ASPECT_DEPTH_BIT};
            }

            // The cluster lists were read by last frame's lighting; this
            // frame's build overwrites them. Execution-only for the hazard,
            // but stated as a memory barrier to keep the intent readable.
            VkMemoryBarrier2 const war = compute_to_compute(VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

            barrier(command, span<Transition const>{reads.data(), count},
                    span<VkMemoryBarrier2 const>{&war, 1});
        }

        // -- 5. clusters -----------------------------------------------------
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

        // -- 6. lighting -----------------------------------------------------
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, lighting_pipeline_.handle());
        vkCmdPushConstants(command, lighting_pipeline_.layout(), VK_SHADER_STAGE_ALL, 0,
                           sizeof(push), &push);
        vkCmdDispatch(command, groups(extent.width, 8), groups(extent.height, 8), 1);

        timestamps_.mark(command, "lighting");

        // -- 7. exposure -----------------------------------------------------
        // The histogram reads the HDR target lighting just wrote, and adds
        // into bins last frame's adapt pass zeroed: one compute-to-compute
        // memory barrier covers both, since its first scope reaches back
        // through earlier submissions. The EV100 image keeps its contents, so
        // after the first frame it comes from READ_ONLY_OPTIMAL, where last
        // frame's tonemap sampled it, not UNDEFINED.
        if (config::kAutoExposure)
        {
            VkMemoryBarrier2 const hdr_written = compute_to_compute(
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

            Transition const to_storage[]{
                {exposure_image_.handle(),
                 exposure_written_ ? VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
            };
            barrier(command, to_storage, span<VkMemoryBarrier2 const>{&hdr_written, 1});

            push.exposure_image = exposure_storage_handle_;

            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, histogram_pipeline_.handle());
            vkCmdPushConstants(command, histogram_pipeline_.layout(), VK_SHADER_STAGE_ALL, 0,
                               sizeof(push), &push);
            vkCmdDispatch(command, groups(extent.width, 16), groups(extent.height, 16), 1);

            VkMemoryBarrier2 const binned = compute_to_compute(
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            barrier(command, {}, span<VkMemoryBarrier2 const>{&binned, 1});

            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, adapt_pipeline_.handle());
            vkCmdDispatch(command, 1, 1, 1);

            // From here on the handle is the sampled view, for tonemap.
            push.exposure_image = exposure_sampled_handle_;
        }

        timestamps_.mark(command, "exposure");

        // -- 8. debug visualisations -----------------------------------------
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

        // -- 9. HDR, exposure and debug images -> sampled, swapchain -> attachment
        {
            VkImage const target = swapchain.image(image_index);

            // Filled by assignment: a std::array brace-initialised with only
            // some of its elements draws GCC's -Wmissing-braces.
            array<Transition, 3 + kDebugWindowCount> handoff{};

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

            if (config::kAutoExposure)
            {
                handoff[count++] = Transition{
                    exposure_image_.handle(), VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
                exposure_written_ = true;
            }

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

        // -- 10. tonemap -----------------------------------------------------
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
            // A pinned EV overrides whatever was metered.
            if (fixed_ev100_.has_value())
            {
                push.exposure       = exposure_from_ev100(*fixed_ev100_);
                push.exposure_image = BindlessSet::kInvalid;
            }

            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemap_pipeline_.handle());
            vkCmdPushConstants(command, tonemap_pipeline_.layout(), VK_SHADER_STAGE_ALL, 0,
                               sizeof(push), &push);
            vkCmdDraw(command, 3, 1, 0, 0);
            vkCmdEndRendering(command);
        }

        timestamps_.mark(command, "tonemap");

        // -- 11. overlay -----------------------------------------------------
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

        // -- 12. capture, when asked for ------------------------------------
        // Detours the image through TRANSFER_SRC and copies it out whole; the
        // present transition below then starts from there.
        VkImageLayout ready_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkPipelineStageFlags2 ready_stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkAccessFlags2        ready_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

        if (capture_requested_ && swapchain.readable())
        {
            capture_requested_ = false;

            VkDeviceSize const bytes = VkDeviceSize{extent.width} * extent.height * 4u;
            if (capture_buffer_.size() < bytes)
            {
                capture_buffer_.shutdown();
                if (!capture_buffer_.init_mapped(*allocator_, bytes,
                                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT))
                {
                    return false;
                }
            }

            VkImage const target = swapchain.image(image_index);

            Transition const to_copy[]{
                {target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT},
            };
            barrier(command, to_copy);

            VkBufferImageCopy const region{
                .bufferOffset      = 0,
                .bufferRowLength   = 0,
                .bufferImageHeight = 0,
                .imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageOffset       = {0, 0, 0},
                .imageExtent       = {extent.width, extent.height, 1},
            };
            vkCmdCopyImageToBuffer(command, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   capture_buffer_.handle(), 1, &region);

            // The host reads it after the fence, which makes device writes
            // available; this makes them visible to the host domain.
            VkMemoryBarrier2 const to_host{
                .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .pNext         = nullptr,
                .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT,
                .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
            };
            barrier(command, {}, span<VkMemoryBarrier2 const>{&to_host, 1});

            capture_.width    = extent.width;
            capture_.height   = extent.height;
            capture_.format   = swapchain.format();
            capture_recorded_ = true;

            ready_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            ready_stage  = VK_PIPELINE_STAGE_2_COPY_BIT;
            ready_access = 0;
        }

        // -- 13. present -----------------------------------------------------
        {
            Transition const present[]{
                {swapchain.image(image_index), ready_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 ready_stage, ready_access, VK_PIPELINE_STAGE_2_NONE, 0},
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
        // so they can be rewritten now. Not before the acquire: a frame that
        // returns OutOfDate reuses this slot, and would rewind the staging
        // arena over copies it never recorded.
        // Geometry first, so objects draw as soon as they can, even untextured.
        staging_.begin(frame_);
        geometry_.stage(staging_, frame_);
        stream_materials();
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

    optional<Renderer::Capture> Renderer::take_capture()
    {
        if (!capture_recorded_)
        {
            return nullopt;
        }
        capture_recorded_ = false;

        Capture capture = capture_;
        size_t const bytes = size_t{capture.width} * capture.height * 4u;
        auto const*  source = static_cast<u8 const*>(capture_buffer_.mapped());
        capture.pixels.assign(source, source + bytes);
        return capture;
    }

    void Renderer::shutdown()
    {
        if (device_ == nullptr)
        {
            return;
        }

        VkDevice const device = device_->handle();

        // The worker writes into loader state only, but it must be gone
        // before anything it could be decoding for is torn down.
        loader_.stop();
        decoded_.clear();
        pending_uploads_.clear();
        staging_.shutdown();

        timestamps_.shutdown();

        gbuffer_pipeline_.shutdown();
        shadow_pipeline_.shutdown();
        tonemap_pipeline_.shutdown();
        cluster_pipeline_.shutdown();
        lighting_pipeline_.shutdown();
        debug_pipeline_.shutdown();
        histogram_pipeline_.shutdown();
        adapt_pipeline_.shutdown();

        exposure_histogram_.shutdown();
        exposure_image_.shutdown();
        capture_buffer_.shutdown();

        geometry_.shutdown();
        materials_.clear();
        white_srgb_.shutdown();
        white_unorm_.shutdown();

        if (material_sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, material_sampler_, memory::vulkan_callbacks());
            material_sampler_ = VK_NULL_HANDLE;
        }

        for (FrameResources& resources : frames_)
        {
            resources.frame.shutdown();
            resources.objects.shutdown();
            resources.lights.shutdown();
            resources.shadow_views.shutdown();
            resources.shadow_matrices.shutdown();
        }

        for (Image& map : shadow_maps_)
        {
            map.shutdown();
        }

        if (shadow_sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, shadow_sampler_, memory::vulkan_callbacks());
            shadow_sampler_ = VK_NULL_HANDLE;
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
