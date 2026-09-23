#pragma once

#include "render/mesh.hpp"
#include "vulkan/buffer.hpp"

VK_DEFINE_HANDLE(VmaVirtualBlock)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VmaVirtualAllocation)

namespace encke
{
    class StagingArena;
    class VulkanAllocator;
    class VulkanDevice;

    // Every mesh's vertices in one device-local buffer and its indices in
    // another, each carved up by a VMA virtual block. A mesh is a range of
    // each, so a pass binds the pool once and draws any set of meshes with
    // one indirect call: vertexOffset and firstIndex pick the ranges.
    //
    // The virtual blocks count in elements, not bytes -- vertices in one,
    // indices in the other -- so a range's offset is directly a
    // vertexOffset or firstIndex, whatever the vertex size.
    //
    // Uploads go through the frame's StagingArena like textures: add()
    // reserves the ranges at once, stage() copies the data into staging
    // within the budget, record() records the copies. A mesh is drawable
    // from the frame it is staged in.
    class GeometryPool
    {
    public:
        struct Range
        {
            u32  first_vertex = 0;
            u32  vertex_count = 0;
            u32  first_index  = 0;
            u32  index_count  = 0;
            bool resident     = false;   // uploaded, or its copy recorded this frame
        };

        GeometryPool() = default;
        ~GeometryPool();

        GeometryPool(GeometryPool const&)            = delete;
        GeometryPool& operator=(GeometryPool const&) = delete;
        GeometryPool(GeometryPool&&)                 = delete;
        GeometryPool& operator=(GeometryPool&&)      = delete;

        bool init(VulkanAllocator const& allocator, VulkanDevice const& device,
                  u32 vertex_capacity, u32 index_capacity, u32 frames_in_flight);
        void shutdown();

        // Reserves room and queues the data for upload. The id is valid at
        // once; the mesh draws once resident. Ids are dense and reused after
        // release. Nullopt, logged, when the pool is full.
        optional<u32> add(span<Vertex const> vertices, span<u32 const> indices);

        // Frees `mesh` once every frame that might draw it has retired: the
        // ranges go back to the pool when `slot` next comes round. Stop
        // drawing it from the frame this is called in.
        void release(u32 mesh, u32 slot);

        // Frees what `slot` retired last time round, then stages queued
        // uploads in order while `staging` has room. After the slot's fence.
        void stage(StagingArena& staging, u32 slot);

        // Records the copies stage() queued and one barrier making them
        // visible to vertex and index fetch. Outside any rendering scope.
        void record(VkCommandBuffer command);

        void bind(VkCommandBuffer command) const;

        Range const& range(u32 mesh) const { return meshes_[mesh].range; }

        // No upload queued or staged-but-unrecorded.
        bool idle() const { return uploads_.empty() && copies_.empty(); }

    private:
        struct Entry
        {
            Range                range;
            VmaVirtualAllocation vertices = nullptr;
            VmaVirtualAllocation indices  = nullptr;
            bool                 live     = false;
        };

        struct Upload
        {
            u32            mesh = 0;
            vector<Vertex> vertices;
            vector<u32>    indices;
        };

        struct Copy
        {
            VkBuffer     source = VK_NULL_HANDLE;
            VkBuffer     target = VK_NULL_HANDLE;
            VkBufferCopy region{};
        };

        void free_entry(u32 mesh);

        Buffer          vertex_buffer_;
        Buffer          index_buffer_;
        VmaVirtualBlock vertex_block_ = nullptr;
        VmaVirtualBlock index_block_  = nullptr;

        vector<Entry> meshes_;
        vector<u32>   free_ids_;

        vector<Upload>      uploads_;   // queued, in order
        vector<Copy>        copies_;    // staged this frame, not yet recorded
        vector<vector<u32>> retired_;   // per frame slot
    };
}
