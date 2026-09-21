#pragma once

namespace encke
{
    class VulkanDevice;

    // One global descriptor set, bound once per command buffer, that every
    // pipeline shares. Shaders index into it with integer handles passed in
    // push constants; see shaders/lib/bindless.slang for the matching side.
    //
    // The binding numbers here and there must agree, and nothing checks it but
    // validation at draw time.
    class BindlessSet
    {
    public:
        enum Binding : u32
        {
            kSampledImages  = 0,
            kStorageImages  = 1,
            kStorageBuffers = 2,   // read-only in shaders
            kSamplers       = 3,
            kWritableBuffers = 4,  // compute-only writes
        };

        // A slot that was never written. Reading it is undefined; the partially
        // bound flag only makes *unused* slots legal.
        static constexpr u32 kInvalid = ~0u;

        BindlessSet() = default;
        ~BindlessSet();

        BindlessSet(BindlessSet const&)            = delete;
        BindlessSet& operator=(BindlessSet const&) = delete;
        BindlessSet(BindlessSet&&)                 = delete;
        BindlessSet& operator=(BindlessSet&&)      = delete;

        bool init(VulkanDevice const& device);
        void shutdown();

        // Each returns a handle for shaders. The update_* forms rewrite an
        // existing slot in place, so a resized image keeps its handle. Neither
        // may touch a slot a pending command buffer uses -- wait for idle first.
        u32  add_sampled_image(VkImageView view, VkImageLayout layout);
        void update_sampled_image(u32 handle, VkImageView view, VkImageLayout layout);

        // Returns the slot for a later add_sampled_image to reuse. The caller
        // guarantees no pending command buffer still reads it. The descriptor
        // is left pointing at the old view, which is legal while unused.
        void release_sampled_image(u32 handle);

        u32  add_storage_image(VkImageView view);
        void update_storage_image(u32 handle, VkImageView view);

        // Read-only in shaders. Safe in every stage.
        u32 add_storage_buffer(VkBuffer buffer, VkDeviceSize size);

        // Writable, and therefore only usable from compute: a writable storage
        // buffer in the vertex or fragment stage would require
        // vertexPipelineStoresAndAtomics / fragmentStoresAndAtomics.
        u32 add_writable_buffer(VkBuffer buffer, VkDeviceSize size);

        u32 add_sampler(VkSampler sampler);

        VkDescriptorSetLayout layout() const { return layout_; }
        VkDescriptorSet       set() const { return set_; }

    private:
        void write_image(Binding binding, u32 index, VkDescriptorType type, VkImageView view,
                         VkImageLayout layout);

        VulkanDevice const*   device_ = nullptr;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
        VkDescriptorPool      pool_   = VK_NULL_HANDLE;
        VkDescriptorSet       set_    = VK_NULL_HANDLE;

        // Monotonic, except sampled images, whose released slots are reused
        // first. Everything else is reused only through the update_* calls.
        u32 add_buffer(Binding binding, u32& next, VkBuffer buffer, VkDeviceSize size);

        vector<u32> free_sampled_images_;

        u32 next_sampled_image_   = 0;
        u32 next_storage_image_   = 0;
        u32 next_storage_buffer_  = 0;
        u32 next_sampler_         = 0;
        u32 next_writable_buffer_ = 0;
    };
}
