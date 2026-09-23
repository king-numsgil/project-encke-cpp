#pragma once

namespace encke
{
    // A typed reference to an asset in the AssetManager: a slot index and
    // the generation the slot was at when the handle was made. Freeing a
    // slot bumps its generation, so an old handle stops resolving instead
    // of reaching whatever reuses the slot. Default-constructed, it is no
    // asset.
    //
    // Nothing frees slots yet, so every generation is still 0; the check is
    // there so eviction can come without auditing every handle.
    template<class Tag>
    struct Handle
    {
        static constexpr u32 kNone = ~0u;

        u32 index      = kNone;
        u32 generation = 0;

        explicit operator bool() const { return index != kNone; }
        bool     operator==(Handle const&) const = default;
    };

    using MeshHandle     = Handle<struct MeshTag>;
    using TextureHandle  = Handle<struct TextureTag>;
    using MaterialHandle = Handle<struct MaterialTag>;
    using ModelHandle    = Handle<struct ModelTag>;
}
