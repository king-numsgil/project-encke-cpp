#include "core/pch.hpp"

#include "render/pipeline.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "vulkan/device.hpp"

namespace encke
{
    namespace
    {
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

            log::info("loaded %s (%zu bytes)", name, size);
            return module;
        }
    }

    GraphicsPipeline::~GraphicsPipeline()
    {
        shutdown();
    }

    bool GraphicsPipeline::init(VulkanDevice const& device, char const* spirv_name,
                                char const* vertex_entry, char const* fragment_entry,
                                VkFormat colour_format)
    {
        device_ = &device;

        VkDevice const handle = device.handle();

        // One module, both entry points. slangc keeps the original names given
        // -fvk-use-entrypoint-name; without it they would both be "main".
        VkShaderModule const module = load_module(handle, spirv_name);
        if (module == VK_NULL_HANDLE)
        {
            return false;
        }

        VkPipelineShaderStageCreateInfo const stages[]{
            {
                .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext               = nullptr,
                .flags               = 0,
                .stage               = VK_SHADER_STAGE_VERTEX_BIT,
                .module              = module,
                .pName               = vertex_entry,
                .pSpecializationInfo = nullptr,
            },
            {
                .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext               = nullptr,
                .flags               = 0,
                .stage               = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module              = module,
                .pName               = fragment_entry,
                .pSpecializationInfo = nullptr,
            },
        };

        // No vertex buffers: the shader derives positions from SV_VertexID.
        VkPipelineVertexInputStateCreateInfo const vertex_input{
            .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext                           = nullptr,
            .flags                           = 0,
            .vertexBindingDescriptionCount   = 0,
            .pVertexBindingDescriptions      = nullptr,
            .vertexAttributeDescriptionCount = 0,
            .pVertexAttributeDescriptions    = nullptr,
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
            .depthClampEnable        = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode             = VK_POLYGON_MODE_FILL,
            // Culling is off until the interaction with the negative-height
            // viewport is settled -- flipping Y reverses apparent winding, so
            // front-face orientation has to be decided against real geometry
            // rather than guessed at here.
            .cullMode                = VK_CULL_MODE_NONE,
            .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable         = VK_FALSE,
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

        VkPipelineColorBlendAttachmentState const blend_attachment{
            .blendEnable         = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp        = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp        = VK_BLEND_OP_ADD,
            .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };

        VkPipelineColorBlendStateCreateInfo const blend{
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext           = nullptr,
            .flags           = 0,
            .logicOpEnable   = VK_FALSE,
            .logicOp         = VK_LOGIC_OP_COPY,
            .attachmentCount = 1,
            .pAttachments    = &blend_attachment,
            .blendConstants  = {0.0f, 0.0f, 0.0f, 0.0f},
        };

        VkDynamicState const dynamic_states[]{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };

        VkPipelineDynamicStateCreateInfo const dynamic{
            .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext             = nullptr,
            .flags             = 0,
            .dynamicStateCount = static_cast<u32>(std::size(dynamic_states)),
            .pDynamicStates    = dynamic_states,
        };

        VkPipelineLayoutCreateInfo const layout_info{
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext                  = nullptr,
            .flags                  = 0,
            .setLayoutCount         = 0,
            .pSetLayouts            = nullptr,
            .pushConstantRangeCount = 0,
            .pPushConstantRanges    = nullptr,
        };

        VkResult result =
            vkCreatePipelineLayout(handle, &layout_info, memory::vulkan_callbacks(), &layout_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreatePipelineLayout", result);
            vkDestroyShaderModule(handle, module, memory::vulkan_callbacks());
            return false;
        }

        // Dynamic rendering replaces the render pass, so the attachment formats
        // the pipeline will be used with are declared here instead.
        VkPipelineRenderingCreateInfo const rendering{
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext                   = nullptr,
            .viewMask                = 0,
            .colorAttachmentCount    = 1,
            .pColorAttachmentFormats = &colour_format,
            .depthAttachmentFormat   = VK_FORMAT_UNDEFINED,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };

        VkGraphicsPipelineCreateInfo const pipeline_info{
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext               = &rendering,
            .flags               = 0,
            .stageCount          = static_cast<u32>(std::size(stages)),
            .pStages             = stages,
            .pVertexInputState   = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pTessellationState  = nullptr,
            .pViewportState      = &viewport_state,
            .pRasterizationState = &rasterization,
            .pMultisampleState   = &multisample,
            .pDepthStencilState  = nullptr,
            .pColorBlendState    = &blend,
            .pDynamicState       = &dynamic,
            .layout              = layout_,
            .renderPass          = VK_NULL_HANDLE,
            .subpass             = 0,
            .basePipelineHandle  = VK_NULL_HANDLE,
            .basePipelineIndex   = -1,
        };

        result = vkCreateGraphicsPipelines(handle, VK_NULL_HANDLE, 1, &pipeline_info,
                                           memory::vulkan_callbacks(), &pipeline_);

        // The module is baked into the pipeline; nothing needs it afterwards.
        vkDestroyShaderModule(handle, module, memory::vulkan_callbacks());

        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateGraphicsPipelines", result);
            return false;
        }

        log::info("pipeline ready (%s, %s)", vertex_entry, fragment_entry);
        return true;
    }

    void GraphicsPipeline::shutdown()
    {
        if (device_ == nullptr)
        {
            return;
        }

        VkDevice const handle = device_->handle();

        if (pipeline_ != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(handle, pipeline_, memory::vulkan_callbacks());
            pipeline_ = VK_NULL_HANDLE;
        }

        if (layout_ != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(handle, layout_, memory::vulkan_callbacks());
            layout_ = VK_NULL_HANDLE;
        }

        device_ = nullptr;
    }
}
