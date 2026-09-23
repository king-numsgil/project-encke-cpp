#pragma once

VK_DEFINE_HANDLE(VmaAllocation)

namespace encke
{
    class VulkanAllocator;
    class VulkanDevice;

    // A sampled 2D image with a full mip chain, filled once and never written
    // again. Unlike Image, which covers render targets, it is left in
    // READ_ONLY_OPTIMAL for good, so nothing per-frame transitions it. Filled
    // either at once with init(), or by create() now and record_upload() in a
    // frame's command buffer.
    class Texture
    {
    public:
        struct Config
        {
            // Four bytes per texel: R8G8B8A8_SRGB for colour, _UNORM for data.
            VkFormat    format = VK_FORMAT_R8G8B8A8_UNORM;
            char const* name   = "texture";
        };

        Texture() = default;
        ~Texture();

        Texture(Texture const&)            = delete;
        Texture& operator=(Texture const&) = delete;
        Texture(Texture&&)                 = delete;
        Texture& operator=(Texture&&)      = delete;

        // Bytes of staging a width x height upload takes: the top level only,
        // since the mips are blitted on the GPU.
        static VkDeviceSize upload_bytes(u32 width, u32 height);

        // The image and view, contents undefined until record_upload() has
        // run on the GPU.
        bool create(VulkanAllocator const& allocator, VulkanDevice const& device,
                    Config const& config, u32 width, u32 height);

        // Records the copy of upload_bytes() of RGBA8 texels, top row first,
        // from `staging` at `offset` (a multiple of 4), then builds every mip
        // by blitting from the one above, and leaves the whole image in
        // READ_ONLY_OPTIMAL for fragment-shader sampling. Outside any
        // rendering scope. Blits filter an _SRGB format in linear space, so
        // colour mips come out right without decoding them here.
        void record_upload(VkCommandBuffer command, VkBuffer staging, VkDeviceSize offset) const;

        // create() and record_upload() through a temporary staging buffer, in
        // one blocking submit_immediate: startup work, not per-frame.
        bool init(VulkanAllocator const& allocator, VulkanDevice const& device,
                  Config const& config, u32 width, u32 height, span<u8 const> texels);

        void shutdown();

        VkImage     handle() const { return image_; }
        VkImageView view() const { return view_; }
        u32         mip_levels() const { return mip_levels_; }

    private:
        VulkanAllocator const* allocator_  = nullptr;
        VulkanDevice const*    device_     = nullptr;
        VkImage                image_      = VK_NULL_HANDLE;
        VkImageView            view_       = VK_NULL_HANDLE;
        VmaAllocation          allocation_ = nullptr;
        u32                    width_      = 0;
        u32                    height_     = 0;
        u32                    mip_levels_ = 0;
    };
}
