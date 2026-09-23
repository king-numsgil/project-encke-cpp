#include "core/pch.hpp"

#include "world/transform.hpp"

#include "core/log.hpp"

#include <cmath>

namespace encke
{
    namespace
    {
        // Hierarchies here are a turret on a hull, a hull's interior: a few
        // levels. A chain longer than this is a cycle, not a hierarchy.
        constexpr size_t kMaxDepth = 32;

        // Below this, `up` is too close to `forward` to say which way up is.
        constexpr f64 kDegenerate = 1.0e-9;

        // The last pass number, kept in the registry so the function needs
        // no state of its own.
        struct TransformPass
        {
            u32 value = 0;
        };

        bool scale_is_valid(f32vec3 const& scale)
        {
            return scale.x > 0.0f && scale.y > 0.0f && scale.z > 0.0f;
        }
    }

    void propagate_transforms(entt::registry& registry)
    {
        u32 const pass = ++registry.ctx().emplace<TransformPass>().value;

        auto& transforms = registry.storage<Transform>();
        auto& worlds     = registry.storage<WorldTransform>();
        auto const all   = registry.view<Transform>();

        // Added here, so nothing that creates an entity has to remember to.
        for (entt::entity const entity : all)
        {
            if (!worlds.contains(entity))
            {
                worlds.emplace(entity);
            }
        }

        // Once each per run: these are per-frame and would flood the log.
        static bool warned_orphan = false;
        static bool warned_cycle  = false;
        static bool warned_scale  = false;

        array<entt::entity, kMaxDepth> chain{};

        for (entt::entity const entity : all)
        {
            // Up from the entity to the first ancestor already composed this
            // pass, or to a root, remembering what was passed on the way.
            size_t       length = 0;
            entt::entity at     = entity;
            bool         rooted = false;
            while (at != entt::null && worlds.get(at).pass != pass)
            {
                if (length == kMaxDepth)
                {
                    if (!warned_cycle)
                    {
                        log::warn("transform: a parent chain is deeper than %zu, or a cycle; "
                                  "its top is placed as a root",
                                  kMaxDepth);
                        warned_cycle = true;
                    }
                    rooted = true;
                    break;
                }

                chain[length++] = at;

                entt::entity const parent = transforms.get(at).parent;
                if (parent != entt::null && !transforms.contains(parent))
                {
                    if (!warned_orphan)
                    {
                        log::warn("transform: a parent is gone or has no Transform; its "
                                  "children are placed as roots");
                        warned_orphan = true;
                    }
                    at = entt::null;
                    break;
                }
                at = parent;
            }

            // Down again, parents first. `at` is the composed ancestor, if any.
            WorldTransform const* above = (at == entt::null || rooted) ? nullptr : &worlds.get(at);
            for (size_t index = length; index-- > 0;)
            {
                Transform const& local = transforms.get(chain[index]);
                WorldTransform&  world = worlds.get(chain[index]);

                if (above != nullptr)
                {
                    world.position = above->position + above->rotation * local.position;
                    world.rotation = above->rotation * local.rotation;
                }
                else
                {
                    world.position = local.position;
                    world.rotation = local.rotation;
                }
                world.scale = local.scale;
                world.pass  = pass;

                if (!warned_scale && !scale_is_valid(local.scale))
                {
                    log::warn("transform: scale (%g, %g, %g) is not positive; mirrored or "
                              "degenerate meshes draw wrongly",
                              static_cast<f64>(local.scale.x), static_cast<f64>(local.scale.y),
                              static_cast<f64>(local.scale.z));
                    warned_scale = true;
                }

                above = &world;
            }
        }
    }

    f64mat4 model_matrix(WorldTransform const& world)
    {
        f64mat4 matrix = glm::mat4_cast(world.rotation);
        matrix[0] *= static_cast<f64>(world.scale.x);
        matrix[1] *= static_cast<f64>(world.scale.y);
        matrix[2] *= static_cast<f64>(world.scale.z);
        matrix[3] = f64vec4{world.position, 1.0};
        return matrix;
    }

    f64quat look_rotation(f64vec3 const& forward, f64vec3 const& up)
    {
        // Columns x right, y up, z back: -Z is the facing.
        f64vec3 const back = -glm::normalize(forward);

        f64vec3 right = glm::cross(up, back);
        if (glm::length(right) < kDegenerate * glm::length(up))
        {
            // Along `up`: any perpendicular will do, so take the axis least
            // aligned with the facing.
            f64vec3 const hint = std::abs(back.x) < 0.9 ? f64vec3{1.0, 0.0, 0.0}
                                                        : f64vec3{0.0, 0.0, 1.0};
            right = glm::cross(glm::cross(back, hint), back);
        }
        right = glm::normalize(right);

        f64vec3 const top = glm::cross(back, right);
        return glm::normalize(glm::quat_cast(f64mat3{right, top, back}));
    }
}
