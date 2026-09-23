#include "core/pch.hpp"

#include "render/geometry_pool.hpp"

#include "core/log.hpp"
#include "vulkan/staging.hpp"

#include <algorithm>
#include <cstring>

#include <vk_mem_alloc.h>

namespace encke
{
    namespace
    {
        VmaVirtualBlock create_block(u32 elements)
        {
            VmaVirtualBlockCreateInfo const info{
                .size                 = elements,
                .flags                = 0,
                .pAllocationCallbacks = nullptr,
            };

            VmaVirtualBlock block = nullptr;
            VkResult const  result = vmaCreateVirtualBlock(&info, &block);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vmaCreateVirtualBlock", result);
                return nullptr;
            }
            return block;
        }

        // An offset in elements, or nullopt when the block is full.
        optional<u32> allocate(VmaVirtualBlock block, size_t count, VmaVirtualAllocation& allocation)
        {
            VmaVirtualAllocationCreateInfo const info{
                .size      = count,
                .alignment = 1,
                .flags     = 0,
                .pUserData = nullptr,
            };

            VkDeviceSize offset = 0;
            if (vmaVirtualAllocate(block, &info, &allocation, &offset) != VK_SUCCESS)
            {
                return nullopt;
            }
            return static_cast<u32>(offset);
        }
    }

    GeometryPool::~GeometryPool()
    {
        shutdown();
    }

    bool GeometryPool::init(VulkanAllocator const& allocator, VulkanDevice const& device,
                            u32 vertex_capacity, u32 index_capacity, u32 frames_in_flight)
    {
        constexpr VkBufferUsageFlags kTarget = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        if (!vertex_buffer_.init_device(allocator, device, VkDeviceSize{vertex_capacity} * sizeof(Vertex),
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | kTarget) ||
            !index_buffer_.init_device(allocator, device, VkDeviceSize{index_capacity} * sizeof(u32),
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | kTarget))
        {
            return false;
        }

        vertex_block_ = create_block(vertex_capacity);
        index_block_  = create_block(index_capacity);
        if (vertex_block_ == nullptr || index_block_ == nullptr)
        {
            return false;
        }

        retired_.assign(frames_in_flight, {});
        log::info("geometry pool: %u vertices, %u indices", vertex_capacity, index_capacity);
        return true;
    }

    void GeometryPool::shutdown()
    {
        // Clearing frees every allocation at once; a block must be empty to
        // be destroyed.
        if (vertex_block_ != nullptr)
        {
            vmaClearVirtualBlock(vertex_block_);
            vmaDestroyVirtualBlock(vertex_block_);
            vertex_block_ = nullptr;
        }
        if (index_block_ != nullptr)
        {
            vmaClearVirtualBlock(index_block_);
            vmaDestroyVirtualBlock(index_block_);
            index_block_ = nullptr;
        }

        meshes_.clear();
        free_ids_.clear();
        uploads_.clear();
        copies_.clear();
        retired_.clear();

        index_buffer_.shutdown();
        vertex_buffer_.shutdown();
    }

    optional<u32> GeometryPool::add(span<Vertex const> vertices, span<u32 const> indices)
    {
        if (vertices.empty() || indices.empty())
        {
            log::error("mesh needs both vertices and indices");
            return nullopt;
        }

        Entry entry;
        optional<u32> const first_vertex = allocate(vertex_block_, vertices.size(), entry.vertices);
        optional<u32> const first_index =
            first_vertex.has_value() ? allocate(index_block_, indices.size(), entry.indices)
                                     : nullopt;
        if (!first_vertex.has_value() || !first_index.has_value())
        {
            if (entry.vertices != nullptr)
            {
                vmaVirtualFree(vertex_block_, entry.vertices);
            }
            log::error("geometry pool full: no room for %zu vertices and %zu indices",
                       vertices.size(), indices.size());
            return nullopt;
        }

        entry.range = Range{
            .first_vertex = *first_vertex,
            .vertex_count = static_cast<u32>(vertices.size()),
            .first_index  = *first_index,
            .index_count  = static_cast<u32>(indices.size()),
            .resident     = false,
            .min          = vertices.front().position,
            .max          = vertices.front().position,
        };
        for (Vertex const& vertex : vertices)
        {
            entry.range.min = glm::min(entry.range.min, vertex.position);
            entry.range.max = glm::max(entry.range.max, vertex.position);
        }
        entry.live = true;

        u32 mesh = 0;
        if (!free_ids_.empty())
        {
            mesh = free_ids_.back();
            free_ids_.pop_back();
            meshes_[mesh] = entry;
        }
        else
        {
            mesh = static_cast<u32>(meshes_.size());
            meshes_.push_back(entry);
        }

        uploads_.push_back(Upload{
            .mesh     = mesh,
            .vertices = vector<Vertex>(vertices.begin(), vertices.end()),
            .indices  = vector<u32>(indices.begin(), indices.end()),
        });

        log::info("mesh %u: %zu vertices, %zu indices", mesh, vertices.size(), indices.size());
        return mesh;
    }

    void GeometryPool::release(u32 mesh, u32 slot)
    {
        if (mesh >= meshes_.size() || !meshes_[mesh].live)
        {
            return;
        }

        // Never staged: nothing on the GPU refers to it.
        std::erase_if(uploads_, [mesh](Upload const& upload) { return upload.mesh == mesh; });

        meshes_[mesh].live           = false;
        meshes_[mesh].range.resident = false;
        retired_[slot].push_back(mesh);
    }

    void GeometryPool::free_entry(u32 mesh)
    {
        Entry& entry = meshes_[mesh];
        vmaVirtualFree(vertex_block_, entry.vertices);
        vmaVirtualFree(index_block_, entry.indices);
        entry = Entry{};
        free_ids_.push_back(mesh);
    }

    void GeometryPool::stage(StagingArena& staging, u32 slot)
    {
        // The slot's fence has signalled, and with it every frame recorded
        // before the release: nothing can still be drawing these.
        for (u32 const mesh : retired_[slot])
        {
            free_entry(mesh);
        }
        retired_[slot].clear();

        size_t done = 0;
        for (; done < uploads_.size(); ++done)
        {
            Upload const&      upload       = uploads_[done];
            VkDeviceSize const vertex_bytes = upload.vertices.size() * sizeof(Vertex);
            VkDeviceSize const index_bytes  = upload.indices.size() * sizeof(u32);

            // Both allocations' alignment slack included.
            if (vertex_bytes + index_bytes + 32 > staging.bytes_per_slot())
            {
                log::error("mesh %u needs %llu bytes of staging, over the %llu a frame allows; "
                           "it never draws",
                           upload.mesh, static_cast<unsigned long long>(vertex_bytes + index_bytes),
                           static_cast<unsigned long long>(staging.bytes_per_slot()));
                continue;
            }
            if (vertex_bytes + index_bytes + 32 > staging.remaining())
            {
                break;
            }

            optional<StagingArena::Allocation> const vertices = staging.allocate(vertex_bytes);
            optional<StagingArena::Allocation> const indices  = staging.allocate(index_bytes);

            std::memcpy(vertices->data, upload.vertices.data(), vertex_bytes);
            std::memcpy(indices->data, upload.indices.data(), index_bytes);

            Range& range = meshes_[upload.mesh].range;
            copies_.push_back(Copy{
                .source = vertices->buffer,
                .target = vertex_buffer_.handle(),
                .region = {vertices->offset, VkDeviceSize{range.first_vertex} * sizeof(Vertex),
                           vertex_bytes},
            });
            copies_.push_back(Copy{
                .source = indices->buffer,
                .target = index_buffer_.handle(),
                .region = {indices->offset, VkDeviceSize{range.first_index} * sizeof(u32),
                           index_bytes},
            });

            range.resident = true;
        }

        uploads_.erase(uploads_.begin(), uploads_.begin() + static_cast<std::ptrdiff_t>(done));
    }

    void GeometryPool::record(VkCommandBuffer command)
    {
        if (copies_.empty())
        {
            return;
        }

        for (Copy const& copy : copies_)
        {
            vkCmdCopyBuffer(command, copy.source, copy.target, 1, &copy.region);
        }
        copies_.clear();

        // Ranges are only ever written while no frame in flight draws them,
        // so the copies need ordering against later fetches only.
        VkMemoryBarrier2 const barrier{
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext         = nullptr,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                             VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT,
        };

        VkDependencyInfo const dependency{
            .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext                    = nullptr,
            .dependencyFlags          = 0,
            .memoryBarrierCount       = 1,
            .pMemoryBarriers          = &barrier,
            .bufferMemoryBarrierCount = 0,
            .pBufferMemoryBarriers    = nullptr,
            .imageMemoryBarrierCount  = 0,
            .pImageMemoryBarriers     = nullptr,
        };
        vkCmdPipelineBarrier2(command, &dependency);
    }

    void GeometryPool::bind(VkCommandBuffer command) const
    {
        VkBuffer const     buffer = vertex_buffer_.handle();
        VkDeviceSize const offset = 0;

        vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
        vkCmdBindIndexBuffer(command, index_buffer_.handle(), 0, VK_INDEX_TYPE_UINT32);
    }
}
