#include "core/pch.hpp"

#include "render/extract.hpp"

#include "core/log.hpp"
#include "render/config.hpp"
#include "render/geometry_pool.hpp"
#include "render/scene.hpp"

namespace encke
{
    void extract(Scene const& scene, GeometryPool const& geometry, RenderList& list)
    {
        list.objects.clear();
        list.lights.clear();

        // Per-frame, so once each per run.
        static bool warned_objects = false;
        static bool warned_lights  = false;

        entt::registry const& registry = scene.registry;

        for (auto const [entity, world, renderable] :
             registry.view<WorldTransform const, Renderable const>().each())
        {
            if (list.objects.size() == config::kMaxObjects)
            {
                if (!warned_objects)
                {
                    log::warn("extract: more than %u renderables; the rest are not drawn",
                              config::kMaxObjects);
                    warned_objects = true;
                }
                break;
            }

            GeometryPool::Range const& range = geometry.range(renderable.mesh);
            f64vec3 const size   = glm::abs(f64vec3{world.scale});
            f64vec3 const centre = (f64vec3{range.min} + f64vec3{range.max}) * 0.5;
            f64vec3 const extent = (f64vec3{range.max} - f64vec3{range.min}) * 0.5;

            list.objects.push_back(RenderObject{
                .entity        = entity,
                .model         = model_matrix(world),
                .scale         = world.scale,
                .bounds_centre = world.position + world.rotation * (centre * size),
                .bounds_radius = glm::length(extent * size),
                .renderable    = renderable,
            });
        }

        for (auto const [entity, world, light] : registry.view<WorldTransform const, Light const>().each())
        {
            if (list.lights.size() == config::kMaxLights)
            {
                if (!warned_lights)
                {
                    log::warn("extract: more than %u lights; the rest are not lit",
                              config::kMaxLights);
                    warned_lights = true;
                }
                break;
            }

            list.lights.push_back(RenderLight{
                .position  = world.position,
                .direction = forward(world.rotation),
                .light     = light,
            });
        }
    }
}
