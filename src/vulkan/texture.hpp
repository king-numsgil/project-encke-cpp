#pragma once

VK_DEFINE_HANDLE(VmaAllocation)

namespace encke
{
    class VulkanAllocator;
    class VulkanDevice;

    // A sampled 2D image with a full mip chain, filled once at creation and
    // never written again. Unlike Image, which covers render targets, it is
    // left in READ_ONLY_OPTIMAL for good, so nothing per-frame transitions it.
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

        // `texels` is width * height RGBA8, top row first. Uploads through a
        // staging buffer and builds every mip by blitting from the one above,
        // in a single blocking submit_immediate: startup work, not per-frame.
        // Blits filter an _SRGB format in linear space, so colour mips come
        // out right without decoding them here.
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
        u32                    mip_levels_ = 0;
    };
}
