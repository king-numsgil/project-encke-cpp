#include "core/pch.hpp"

#include "render/pipeline.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "vulkan/device.hpp"

namespace encke
{
    namespace
    {
        // Upper bound on colour attachments in one pass. The G-buffer uses five.
        constexpr size_t kMaxColourAttachments = 8;

        // SPIR-V sits in shaders/ beside the executable, written there by the
        // slangc custom command. SDL owns the base path; do not free it.
        VkShaderModule load_module(VkDevice device, char const* name)
        {
            char const* const base = SDL_GetBasePath();
            if (base == nullptr)
            {
                log::sdl_error("SDL_GetBasePath");
                return VK_NULL_HANDLE;
            }

            string const path = string{base} + "shaders/" + name;

            size_t size = 0;
            void* const bytes = SDL_LoadFile(path.c_str(), &size);
            if (bytes == nullptr)
            {
                log::error("cannot read %s: %s", path.c_str(), SDL_GetError());
                return VK_NULL_HANDLE;
            }

            // SPIR-V is a stream of 32-bit words; a truncated file would be
            // read as garbage opcodes rather than rejected.
            if (size == 0 || (size % sizeof(u32)) != 0)
            {
                log::error("%s is %zu bytes, not a whole number of SPIR-V words",
                           path.c_str(), size);
                SDL_free(bytes);
                return VK_NULL_HANDLE;
            }

            VkShaderModuleCreateInfo const info{
                .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext    = nullptr,
                .flags    = 0,
                .codeSize = size,
                .pCode    = static_cast<u32 const*>(bytes),
            };

            VkShaderModule module = VK_NULL_HANDLE;
            VkResult const created =
                vkCreateShaderModule(device, &info, memory::vulkan_callbacks(), &module);

            SDL_free(bytes);

            if (created != VK_SUCCESS)
            {
                log::vk_error("vkCreateShaderModule", created);
                return VK_NULL_HANDLE;
            }

            return module;
        }

        // The layout every pipeline in encke uses: bindless set 0, one push
        // constant range across all stages.
        VkPipelineLayout create_layout(VkDevice device, VkDescriptorSetLayout set_layout,
                                       u32 push_constant_size)
        {
            VkPushConstantRange const push_range{
                .stageFlags = VK_SHADER_STAGE_ALL,
                .offset     = 0,
                .size       = push_constant_size,
            };

            bool const has_set  = set_layout != VK_NULL_HANDLE;
            bool const has_push = push_constant_size > 0;

            VkPipelineLayoutCreateInfo const info{
                .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .pNext                  = nullptr,
                .flags                  = 0,
                .setLayoutCount         = has_set ? 1u : 0u,
                .pSetLayouts            = has_set ? &set_layout : nullptr,
                .pushConstantRangeCount = has_push ? 1u : 0u,
                .pPushConstantRanges    = has_push ? &push_range : nullptr,
            };

            VkPipelineLayout layout = VK_NULL_HANDLE;
            VkResult const   result =
                vkCreatePipelineLayout(device, &info, memory::vulkan_callbacks(), &layout);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkCreatePipelineLayout", result);
                return VK_NULL_HANDLE;
            }

            return layout;
        }

        void destroy(VkDevice device, VkPipeline& pipeline, VkPipelineLayout& layout)
        {
            if (pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(device, pipeline, memory::vulkan_callbacks());
                pipeline = VK_NULL_HANDLE;
            }

            if (layout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(device, layout, memory::vulkan_callbacks());
                layout = VK_NULL_HANDLE;
            }
        }
    }

    // -- graphics ---------------------------------------------------------------

    GraphicsPipeline::~GraphicsPipeline()
    {
        shutdown();
    }

    bool GraphicsPipeline::init(VulkanDevice const& device, Config const& config)
    {
        device_ = &device;

        if (config.colour_formats.size() > kMaxColourAttachments)
        {
            log::error("%s: %zu colour attachments exceeds %zu", config.spirv_name,
                       config.colour_formats.size(), kMaxColourAttachments);
            return false;
        }

        VkDevice const handle = device.handle();

        // One module, both entry points. slangc keeps the original names given
        // -fvk-use-entrypoint-name; without it they would both be "main".
        VkShaderModule const module = load_module(handle, config.spirv_name);
        if (module == VK_NULL_HANDLE)
        {
            return false;
        }

        bool const has_fragment = config.fragment_entry != nullptr;

        VkPipelineShaderStageCreateInfo const stages[]{
            {
                .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext               = nullptr,
                .flags               = 0,
                .stage               = VK_SHADER_STAGE_VERTEX_BIT,
                .module              = module,
                .pName               = config.vertex_entry,
                .pSpecializationInfo = nullptr,
            },
            {
                .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext               = nullptr,
                .flags               = 0,
                .stage               = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module              = module,
                .pName               = config.fragment_entry,
                .pSpecializationInfo = nullptr,
            },
        };

        VkPipelineVertexInputStateCreateInfo const vertex_input{
            .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext                           = nullptr,
            .flags                           = 0,
            .vertexBindingDescriptionCount   = static_cast<u32>(config.bindings.size()),
            .pVertexBindingDescriptions      = config.bindings.data(),
            .vertexAttributeDescriptionCount = static_cast<u32>(config.attributes.size()),
            .pVertexAttributeDescriptions    = config.attributes.data(),
        };

        VkPipelineInputAssemblyStateCreateInfo const input_assembly{
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext                  = nullptr,
            .flags                  = 0,
            .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE,
        };

        // Counts still matter with dynamic viewport/scissor; the pointers do not.
        VkPipelineViewportStateCreateInfo const viewport_state{
            .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext         = nullptr,
            .flags         = 0,
            .viewportCount = 1,
            .pViewports    = nullptr,
            .scissorCount  = 1,
            .pScissors     = nullptr,
        };

        VkPipelineRasterizationStateCreateInfo const rasterization{
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext                   = nullptr,
            .flags                   = 0,
            .depthClampEnable        = config.depth_clamp ? VK_TRUE : VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode             = VK_POLYGON_MODE_FILL,
            .cullMode                = config.cull_mode,
            // Facing is decided from the signed area in framebuffer
            // coordinates, i.e. after the viewport transform, and the
            // negative height reverses it. Geometry wound counter-clockwise in
            // NDC with +Y up therefore stays front-facing here.
            .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable         = config.depth_bias ? VK_TRUE : VK_FALSE,
            .depthBiasConstantFactor = 0.0f,
            .depthBiasClamp          = 0.0f,
            .depthBiasSlopeFactor    = 0.0f,
            .lineWidth               = 1.0f,
        };

        VkPipelineMultisampleStateCreateInfo const multisample{
            .sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable   = VK_FALSE,
            .minSampleShading      = 1.0f,
            .pSampleMask           = nullptr,
            .alphaToCoverageEnable = VK_FALSE,
            .alphaToOneEnable      = VK_FALSE,
        };

        // Reversed-Z: near is 1.0, far is 0.0, so a fragment passes when its
        // depth is GREATER than what is already there.
        VkPipelineDepthStencilStateCreateInfo const depth_stencil{
            .sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .depthTestEnable       = VK_TRUE,
            .depthWriteEnable      = config.depth_write ? VK_TRUE : VK_FALSE,
            .depthCompareOp        = VK_COMPARE_OP_GREATER_OR_EQUAL,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable     = VK_FALSE,
            .front                 = {},
            .back                  = {},
            .minDepthBounds        = 0.0f,
            .maxDepthBounds        = 1.0f,
        };

        bool const has_depth = config.depth_format != VK_FORMAT_UNDEFINED;

        // Only the UI blends; the G-buffer overwrites and tonemap is a
        // full-screen overwrite.
        bool const blend_on = config.alpha_blend;

        array<VkPipelineColorBlendAttachmentState, kMaxColourAttachments> blend_attachments{};
        for (size_t index = 0; index < config.colour_formats.size(); ++index)
        {
            blend_attachments[index] = VkPipelineColorBlendAttachmentState{
                .blendEnable         = blend_on ? VK_TRUE : VK_FALSE,
                .srcColorBlendFactor = blend_on ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE,
                .dstColorBlendFactor =
                    blend_on ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ZERO,
                .colorBlendOp        = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstAlphaBlendFactor =
                    blend_on ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ZERO,
                .alphaBlendOp        = VK_BLEND_OP_ADD,
                .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
            };
        }

        VkPipelineColorBlendStateCreateInfo const blend{
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext           = nullptr,
            .flags           = 0,
            .logicOpEnable   = VK_FALSE,
            .logicOp         = VK_LOGIC_OP_COPY,
            .attachmentCount = static_cast<u32>(config.colour_formats.size()),
            .pAttachments    = blend_attachments.data(),
            .blendConstants  = {0.0f, 0.0f, 0.0f, 0.0f},
        };

        VkDynamicState const dynamic_states[]{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_DEPTH_BIAS,
        };

        VkPipelineDynamicStateCreateInfo const dynamic{
            .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext             = nullptr,
            .flags             = 0,
            .dynamicStateCount = config.depth_bias ? 3u : 2u,
            .pDynamicStates    = dynamic_states,
        };

        layout_ = create_layout(handle, config.set_layout, config.push_constant_size);
        if (layout_ == VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(handle, module, memory::vulkan_callbacks());
            return false;
        }

        // Dynamic rendering replaces the render pass, so the attachment formats
        // the pipeline will be used with are declared here instead.
        VkPipelineRenderingCreateInfo const rendering{
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext                   = nullptr,
            .viewMask                = 0,
            .colorAttachmentCount    = static_cast<u32>(config.colour_formats.size()),
            .pColorAttachmentFormats = config.colour_formats.data(),
            .depthAttachmentFormat   = config.depth_format,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };

        VkGraphicsPipelineCreateInfo const pipeline_info{
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext               = &rendering,
            .flags               = 0,
            .stageCount          = has_fragment ? 2u : 1u,
            .pStages             = stages,
            .pVertexInputState   = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pTessellationState  = nullptr,
            .pViewportState      = &viewport_state,
            .pRasterizationState = &rasterization,
            .pMultisampleState   = &multisample,
            .pDepthStencilState  = has_depth ? &depth_stencil : nullptr,
            .pColorBlendState    = &blend,
            .pDynamicState       = &dynamic,
            .layout              = layout_,
            .renderPass          = VK_NULL_HANDLE,
            .subpass             = 0,
            .basePipelineHandle  = VK_NULL_HANDLE,
            .basePipelineIndex   = -1,
        };

        VkResult const result = vkCreateGraphicsPipelines(
            handle, VK_NULL_HANDLE, 1, &pipeline_info, memory::vulkan_callbacks(), &pipeline_);

        // The module is baked into the pipeline; nothing needs it afterwards.
        vkDestroyShaderModule(handle, module, memory::vulkan_callbacks());

        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateGraphicsPipelines", result);
            log::error("  while creating %s", config.spirv_name);
            return false;
        }

        log::info("graphics pipeline %s ready", config.spirv_name);
        return true;
    }

    void GraphicsPipeline::shutdown()
    {
        if (device_ == nullptr)
        {
            return;
        }

        destroy(device_->handle(), pipeline_, layout_);
        device_ = nullptr;
    }

    // -- compute ----------------------------------------------------------------

    ComputePipeline::~ComputePipeline()
    {
        shutdown();
    }

    bool ComputePipeline::init(VulkanDevice const& device, Config const& config)
    {
        device_ = &device;

        VkDevice const handle = device.handle();

        VkShaderModule const module = load_module(handle, config.spirv_name);
        if (module == VK_NULL_HANDLE)
        {
            return false;
        }

        layout_ = create_layout(handle, config.set_layout, config.push_constant_size);
        if (layout_ == VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(handle, module, memory::vulkan_callbacks());
            return false;
        }

        VkComputePipelineCreateInfo const info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = {
                .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext               = nullptr,
                .flags               = 0,
                .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
                .module              = module,
                .pName               = config.entry,
                .pSpecializationInfo = nullptr,
            },
            .layout             = layout_,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex  = -1,
        };

        VkResult const result = vkCreateComputePipelines(handle, VK_NULL_HANDLE, 1, &info,
                                                         memory::vulkan_callbacks(), &pipeline_);

        vkDestroyShaderModule(handle, module, memory::vulkan_callbacks());

        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateComputePipelines", result);
            log::error("  while creating %s", config.spirv_name);
            return false;
        }

        log::info("compute pipeline %s ready", config.spirv_name);
        return true;
    }

    void ComputePipeline::shutdown()
    {
        if (device_ == nullptr)
        {
            return;
        }

        destroy(device_->handle(), pipeline_, layout_);
        device_ = nullptr;
    }
}
