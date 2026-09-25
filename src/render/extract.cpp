#include "core/pch.hpp"

#include "render/extract.hpp"

#include "assets/asset_manager.hpp"
#include "core/log.hpp"
#include "render/config.hpp"
#include "render/scene.hpp"

namespace encke
{
    void extract(Scene const& scene, AssetManager const& assets, RenderList& list)
    {
        list.objects.clear();
        list.lights.clear();

        // Per-frame, so once each per run.
        static bool warned_objects = false;
        static bool warned_lights  = false;
        static bool warned_camera  = false;

        entt::registry const& registry = scene.registry;

        if (registry.valid(scene.camera) && registry.all_of<WorldTransform, Camera>(scene.camera))
        {
            WorldTransform const& world = registry.get<WorldTransform>(scene.camera);
            list.camera = CameraView{
                .position    = world.position,
                .orientation = world.rotation,
                .lens        = registry.get<Camera>(scene.camera),
            };
        }
        else if (!warned_camera)
        {
            log::warn("extract: the scene's camera has no Transform or Camera; drawing from "
                      "the last one");
            warned_camera = true;
        }

        list.star         = starlight_at(registry, list.camera.position);
        list.surroundings = surroundings_at(registry, list.camera.position);
        list.atmosphere   = atmosphere_at(registry, list.camera.position);

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

            // A mesh that is not known yet has no size; it draws nothing
            // either until it is.
            MeshAsset const* const mesh   = assets.mesh(renderable.mesh);
            f64vec3 const          min    = mesh != nullptr ? f64vec3{mesh->min} : f64vec3{0.0};
            f64vec3 const          max    = mesh != nullptr ? f64vec3{mesh->max} : f64vec3{0.0};
            f64vec3 const          size   = glm::abs(f64vec3{world.scale});
            f64vec3 const          centre = (min + max) * 0.5;
            f64vec3 const          extent = (max - min) * 0.5;

            Geomorph const* const geomorph = registry.try_get<Geomorph>(entity);

            list.objects.push_back(RenderObject{
                .entity        = entity,
                .model         = model_matrix(world),
                .scale         = world.scale,
                .bounds_centre = world.position + world.rotation * (centre * size),
                .bounds_radius = glm::length(extent * size),
                .renderable    = renderable,
                .geomorph      = geomorph != nullptr ? optional<Geomorph>{*geomorph} : nullopt,
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
