#pragma once

namespace encke
{
    // Where an entity sits relative to its parent, or to the world when it
    // has none. World positions are f64 like everything else on the CPU.
    //
    // Scale is NOT inherited. It sizes this entity's own mesh and nothing
    // else: a child's position is an offset in its parent's rotated frame,
    // unscaled, and its size is its own. Inherited non-uniform scale under a
    // rotated child is shear, which position, rotation and scale cannot
    // represent; leaving scale out of the chain keeps every world transform
    // exactly that triple. To scale an assembly, scale each part and its
    // offset, as spawning a model does.
    //
    // Scale must be positive. A negative one mirrors the mesh, which needs
    // the opposite winding, and a zero one has no inverse for the normal
    // matrix; propagate_transforms warns about either.
    struct Transform
    {
        f64vec3      position{0.0};
        f64quat      rotation{1.0, 0.0, 0.0, 0.0};   // (w, x, y, z)
        f32vec3      scale{1.0f};
        entt::entity parent = entt::null;
    };

    // Transform composed down the parent chain. Written only by
    // propagate_transforms, which adds it to every entity with a Transform,
    // so read it after that has run this frame.
    struct WorldTransform
    {
        f64vec3 position{0.0};
        f64quat rotation{1.0, 0.0, 0.0, 0.0};
        f32vec3 scale{1.0f};

        // The propagation pass that wrote it, which lets a pass compose each
        // entity once whatever order it meets parents and children in.
        u32 pass = 0;
    };

    // Composes every entity's WorldTransform from its Transform and its
    // ancestors'. Each entity is composed once, parents before children,
    // in no particular storage order. A parent that no longer exists, or
    // has no Transform, leaves its children placed as roots, with a warning.
    void propagate_transforms(entt::registry& registry);

    // World (or parent) space from the entity's own: translate, rotate,
    // scale. Only the renderer should need it, and only in f64.
    f64mat4 model_matrix(WorldTransform const& world);

    // The rotation that turns -Z toward `forward`, with +Y as close to `up`
    // as it can be: the camera's and a spot light's convention. Any up
    // works when `forward` lies along `up`; it is then picked from the axes.
    f64quat look_rotation(f64vec3 const& forward, f64vec3 const& up);

    // -Z, the way an entity with this rotation faces.
    inline f64vec3 forward(f64quat const& rotation)
    {
        return rotation * f64vec3{0.0, 0.0, -1.0};
    }
}
