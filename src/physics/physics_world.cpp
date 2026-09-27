#include "core/pch.hpp"

#include "physics/physics_world.hpp"

#include "core/log.hpp"
#include "physics/components.hpp"
#include "platform/thread.hpp"
#include "platform/worker_pool.hpp"
#include "render/config.hpp"
#include "terrain/planet.hpp"
#include "terrain/surface_nets.hpp"
#include "world/bodies.hpp"
#include "world/transform.hpp"

// Jolt.h first: it sets up the configuration every other Jolt header reads.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#if ENCKE_USE_MIMALLOC
#   include <mimalloc.h>
#endif

namespace encke::physics
{
    namespace
    {
        constexpr JPH::ObjectLayer kStatic     = 0;   // terrain
        constexpr JPH::ObjectLayer kMoving     = 1;
        constexpr u32              kLayerCount = 2;

        constexpr u32 kMaxBodies     = 65'536;
        constexpr u32 kMaxBodyPairs  = 65'536;
        constexpr u32 kMaxContacts   = 16'384;
        constexpr u32 kTempBytes     = 16u << 20;

        JPH::RVec3 to_jolt(f64vec3 const& v)
        {
            return JPH::RVec3{v.x, v.y, v.z};
        }

        JPH::Quat to_jolt(f64quat const& q)
        {
            return JPH::Quat{static_cast<f32>(q.x), static_cast<f32>(q.y), static_cast<f32>(q.z),
                             static_cast<f32>(q.w)}.Normalized();
        }

        f64vec3 from_jolt(JPH::RVec3Arg v)
        {
            return f64vec3{v.GetX(), v.GetY(), v.GetZ()};
        }

        f64quat from_jolt(JPH::QuatArg q)
        {
            return glm::normalize(f64quat{q.GetW(), q.GetX(), q.GetY(), q.GetZ()});
        }

        JPH::BodyID body_id(RigidBody const& body)
        {
            return JPH::BodyID{body.body};
        }

        // Jolt's allocations go where everything else's do.
        void install_allocator()
        {
#if ENCKE_USE_MIMALLOC
            JPH::Allocate        = [](size_t size) { return mi_malloc(size); };
            JPH::Reallocate      = [](void* block, size_t, size_t size) { return mi_realloc(block, size); };
            JPH::Free            = [](void* block) { mi_free(block); };
            JPH::AlignedAllocate = [](size_t size, size_t alignment) { return mi_malloc_aligned(size, alignment); };
            JPH::AlignedFree     = [](void* block) { mi_free(block); };
#else
            JPH::RegisterDefaultAllocator();
#endif
        }

        // A collision chunk: a terrain body's chunk at config::kCollisionLod,
        // by its corner on the grid.
        struct ChunkId
        {
            terrain::BodyTerrain const* terrain = nullptr;
            i64vec3                     origin{0};

            bool operator==(ChunkId const&) const = default;
        };

        struct ChunkIdHash
        {
            size_t operator()(ChunkId const& id) const
            {
                size_t hash = std::hash<void const*>{}(id.terrain);
                for (i64 const value : {id.origin.x, id.origin.y, id.origin.z})
                {
                    hash = hash * 1'000'003u ^ std::hash<i64>{}(value);
                }
                return hash;
            }
        };

        struct Chunk
        {
            bool        ready = false;
            JPH::BodyID body;   // invalid where the chunk has no surface

            // Built and waiting to become a body: see Impl::add_built.
            JPH::ShapeRefC shape;
            f64vec3        corner{0.0};
            f64quat        rotation{1.0, 0.0, 0.0, 0.0};

            // The step count when a body was last near enough to keep it.
            u64 needed = 0;
        };

        bool before(ChunkId const& a, ChunkId const& b)
        {
            return std::tie(a.origin.x, a.origin.y, a.origin.z) < std::tie(b.origin.x, b.origin.y, b.origin.z);
        }

        // A body with terrain, where it is this frame.
        struct Ground
        {
            std::shared_ptr<terrain::BodyTerrain const> terrain;
            f64vec3                                     position{0.0};
            f64quat                                     rotation{1.0, 0.0, 0.0, 0.0};
        };

        vector<Ground> grounds(entt::registry const& registry)
        {
            vector<Ground> out;
            for (auto const [entity, planet, world] :
                 registry.view<terrain::PlanetTerrain const, WorldTransform const>().each())
            {
                if (planet.terrain)
                {
                    out.push_back(Ground{planet.terrain, world.position, world.rotation});
                }
            }
            return out;
        }

        // The acceleration of gravity at `point`: toward the centre of the
        // pulling body whose surface is nearest.
        f64vec3 gravity_at(entt::registry const& registry, f64vec3 const& point)
        {
            f64vec3 best{0.0};
            f64     nearest = std::numeric_limits<f64>::infinity();
            for (auto const [entity, body, world] : registry.view<Body const, WorldTransform const>().each())
            {
                if (body.surface_gravity <= 0.0)
                {
                    continue;
                }
                f64vec3 const centre   = world.position + world.rotation * body.centre;
                f64vec3 const toward   = centre - point;
                f64 const     distance = glm::length(toward);
                if (distance <= 0.0 || distance - body.radius >= nearest)
                {
                    continue;
                }
                nearest = distance - body.radius;

                // Inverse square outside; held at the surface value within.
                f64 const r = std::max(distance, body.radius);
                best = toward / distance * (body.surface_gravity * (body.radius / r) * (body.radius / r));
            }
            return best;
        }
    }

    struct PhysicsWorld::Impl
    {
        bool live = false;

        JPH::BroadPhaseLayerInterfaceTable                       broad_phase{kLayerCount, kLayerCount};
        JPH::ObjectLayerPairFilterTable                          object_filter{kLayerCount};
        std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> broad_filter;
        std::unique_ptr<JPH::TempAllocatorImpl>                  temp;
        std::unique_ptr<JPH::JobSystemThreadPool>                jobs;
        std::unique_ptr<JPH::PhysicsSystem>                      system;

        std::unordered_map<ChunkId, Chunk, ChunkIdHash> chunks;
        u32                                             building = 0;

        // One per pool worker, each keyed by terrain, made on first use by
        // that worker alone.
        vector<std::unordered_map<terrain::BodyTerrain const*, std::unique_ptr<terrain::TerrainSampler>>> samplers;

        u32 lod         = config::kCollisionLod;
        f64 accumulator = 0.0;
        u32 held        = 0;
        u32 dynamic     = 0;
        u64 steps       = 0;

        ~Impl()
        {
            if (!live)
            {
                return;
            }
            system.reset();
            jobs.reset();
            temp.reset();
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }

        terrain::TerrainSampler& sampler(u32 worker, terrain::BodyTerrain const& terrain)
        {
            std::unique_ptr<terrain::TerrainSampler>& slot = samplers[worker][&terrain];
            if (!slot)
            {
                slot = std::make_unique<terrain::TerrainSampler>(terrain);
            }
            return *slot;
        }

        // Calls `visit` with every collision chunk's id within `reach` of
        // `point`, on the ground's grid.
        template<class Visit>
        void each_chunk(Ground const& ground, f64vec3 const& point, f64 reach, Visit&& visit) const
        {
            terrain::BodyTerrain const& terrain = *ground.terrain;
            i64 const span = static_cast<i64>(terrain::ChunkRequest{}.cells) << lod;
            f64 const edge = terrain.base_voxel_size * static_cast<f64>(span);

            f64vec3 const local = glm::inverse(ground.rotation) * (point - ground.position);
            i64vec3 const low{glm::floor((local - reach) / edge)};
            i64vec3 const high{glm::floor((local + reach) / edge)};

            for (i64 z = low.z; z <= high.z; ++z)
            {
                for (i64 y = low.y; y <= high.y; ++y)
                {
                    for (i64 x = low.x; x <= high.x; ++x)
                    {
                        visit(ChunkId{&terrain, i64vec3{x, y, z} * span});
                    }
                }
            }
        }

        // Every collision chunk within `reach` of `point`, requested if new.
        // Returns whether all of them are built.
        bool ensure(Ground const& ground, f64vec3 const& point, f64 reach, WorkerPool& pool)
        {
            bool ready = true;
            each_chunk(ground, point, reach, [&](ChunkId const& id) {
                auto const found = chunks.find(id);
                if (found == chunks.end())
                {
                    Chunk chunk;
                    chunk.needed = steps;
                    chunks.emplace(id, std::move(chunk));
                    build(ground, id, pool);
                    ready = false;
                }
                else if (!found->second.ready)
                {
                    ready = false;
                }
            });
            return ready;
        }

        // Marks every built or building chunk within `reach` of `point` as
        // needed now.
        void keep(Ground const& ground, f64vec3 const& point, f64 reach)
        {
            each_chunk(ground, point, reach, [&](ChunkId const& id) {
                if (auto const found = chunks.find(id); found != chunks.end())
                {
                    found->second.needed = steps;
                }
            });
        }

        // Removes chunks no body has kept for config::kCollisionKeepSteps.
        // Only built ones: a chunk still building is needed by definition.
        // In grid order, since removing a body is a call Jolt's determinism
        // counts like adding one.
        void evict()
        {
            vector<ChunkId> stale;
            for (auto const& [id, chunk] : chunks)
            {
                if (chunk.ready && steps - chunk.needed > config::kCollisionKeepSteps)
                {
                    stale.push_back(id);
                }
            }
            if (stale.empty())
            {
                return;
            }
            std::ranges::sort(stale, before);

            JPH::BodyInterface& bodies = system->GetBodyInterface();
            for (ChunkId const& id : stale)
            {
                Chunk const& chunk = chunks.at(id);
                if (!chunk.body.IsInvalid())
                {
                    bodies.RemoveBody(chunk.body);
                    bodies.DestroyBody(chunk.body);
                }
                chunks.erase(id);
            }
        }

        // Meshes a collision chunk on the pool, the way the octree meshes a
        // drawn one without its morph targets or skirts, and adds it as a
        // static body once it is back.
        void build(Ground const& ground, ChunkId const& id, WorkerPool& pool)
        {
            if (samplers.size() < pool.size())
            {
                samplers.resize(pool.size());
            }
            ++building;

            std::shared_ptr<terrain::BodyTerrain const> const terrain = ground.terrain;
            f64vec3 const corner = ground.position + ground.rotation * (f64vec3{id.origin} * terrain->base_voxel_size);
            f64quat const rotation = ground.rotation;

            pool.submit([this, id, terrain, corner, rotation, level = lod](u32 worker) -> WorkerPool::Completion {
                terrain::TerrainSampler&    sampler = this->sampler(worker, *terrain);
                terrain::ChunkRequest const request = sampler.chunk(id.origin, level);

                vector<f32> samples(request.sample_count());
                sampler.sample_chunk(request, samples);

                terrain::SurfaceMesh mesh;
                terrain::surface_nets(samples, request.cells, terrain->voxel_size(level), mesh);

                JPH::ShapeRefC shape;
                if (!mesh.indices.empty())
                {
                    JPH::VertexList vertices;
                    vertices.reserve(mesh.positions.size());
                    for (f32vec3 const& p : mesh.positions)
                    {
                        vertices.push_back(JPH::Float3{p.x, p.y, p.z});
                    }

                    JPH::IndexedTriangleList triangles;
                    triangles.reserve(mesh.indices.size() / 3);
                    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
                    {
                        triangles.push_back(JPH::IndexedTriangle{mesh.indices[i], mesh.indices[i + 1],
                                                                 mesh.indices[i + 2], 0});
                    }

                    // Degenerate triangles, which Surface Nets makes, are
                    // dropped by the settings' Sanitize.
                    JPH::MeshShapeSettings const   settings{std::move(vertices), std::move(triangles)};
                    JPH::ShapeSettings::ShapeResult const result = settings.Create();
                    if (result.IsValid())
                    {
                        shape = result.Get();
                    }
                    else
                    {
                        log::error("physics: collision chunk failed: %s", result.GetError().c_str());
                    }
                }

                return [this, id, shape, corner, rotation] { finish(id, shape, corner, rotation); };
            });
        }

        void finish(ChunkId const& id, JPH::ShapeRefC const& shape, f64vec3 const& corner, f64quat const& rotation)
        {
            --building;
            Chunk& chunk   = chunks[id];
            chunk.shape    = shape;
            chunk.corner   = corner;
            chunk.rotation = rotation;
            if (shape == nullptr)
            {
                chunk.ready = true;
            }
        }

        // Jolt is deterministic only given the same calls in the same order,
        // body creation included, and chunks come back from the pool in
        // whatever order they finish. So none becomes a body until every
        // chunk asked for is back, and then all of them in grid order. Bodies
        // wait for ground (the world does not step), so what has been asked
        // for by then is the same on every run.
        void add_built()
        {
            if (building > 0)
            {
                return;
            }

            vector<ChunkId> built;
            for (auto const& [id, chunk] : chunks)
            {
                if (!chunk.ready && chunk.shape != nullptr)
                {
                    built.push_back(id);
                }
            }
            std::ranges::sort(built, before);

            JPH::BodyInterface& bodies = system->GetBodyInterface();
            for (ChunkId const& id : built)
            {
                Chunk&                          chunk = chunks[id];
                JPH::BodyCreationSettings const settings{chunk.shape, to_jolt(chunk.corner), to_jolt(chunk.rotation),
                                                         JPH::EMotionType::Static, kStatic};
                chunk.body  = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
                chunk.ready = true;
                chunk.shape = nullptr;
            }
        }

        // A dynamic body on `entity` at its Transform's pose, held.
        void add(entt::registry& registry, entt::entity entity, JPH::ShapeRefC const& shape, f64 bound)
        {
            Transform const&          transform = registry.get<Transform>(entity);
            JPH::BodyCreationSettings settings{shape, to_jolt(transform.position), to_jolt(transform.rotation),
                                               JPH::EMotionType::Dynamic, kMoving};
            // Swept, not only tested where each step ends. A body that moves
            // more than half its size in a step can otherwise pass a
            // one-sided terrain triangle between two steps: a half-metre box
            // dropped from 60 m went straight through the ground.
            settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
            JPH::Body* const body = system->GetBodyInterface().CreateBody(settings);
            if (body == nullptr)
            {
                log::error("physics: out of bodies");
                return;
            }

            registry.emplace<RigidBody>(entity, RigidBody{
                                                    .body              = body->GetID().GetIndexAndSequenceNumber(),
                                                    .previous_position = transform.position,
                                                    .position          = transform.position,
                                                    .previous_rotation = transform.rotation,
                                                    .rotation          = transform.rotation,
                                                    .bound             = bound,
                                                    .added             = false,
                                                });
            ++held;
        }

        // Nearest by the terrain's reference sphere.
        Ground const* nearest(vector<Ground> const& all, f64vec3 const& point) const
        {
            Ground const* best     = nullptr;
            f64           distance = std::numeric_limits<f64>::infinity();
            for (Ground const& ground : all)
            {
                f64 const d = std::abs(glm::length(point - ground.position) - ground.terrain->radius);
                if (d < distance)
                {
                    distance = d;
                    best     = &ground;
                }
            }
            return best;
        }

        void step(entt::registry& registry)
        {
            f32 const dt = static_cast<f32>(1.0 / config::kPhysicsHz);

            // Each awake body's own gravity, as a force for this step.
            JPH::BodyIDVector active;
            system->GetActiveBodies(JPH::EBodyType::RigidBody, active);
            JPH::BodyLockInterfaceNoLock const& locks = system->GetBodyLockInterfaceNoLock();
            for (JPH::BodyID const id : active)
            {
                JPH::BodyLockWrite lock{locks, id};
                if (!lock.Succeeded())
                {
                    continue;
                }
                JPH::Body& body = lock.GetBody();
                f32 const  inverse_mass = body.GetMotionProperties()->GetInverseMass();
                if (inverse_mass <= 0.0f)
                {
                    continue;
                }
                f64vec3 const g = gravity_at(registry, from_jolt(body.GetCenterOfMassPosition()));
                body.AddForce(JPH::Vec3{static_cast<f32>(g.x), static_cast<f32>(g.y), static_cast<f32>(g.z)} /
                              inverse_mass);
            }

            system->Update(dt, 1, temp.get(), jobs.get());
            ++steps;

            // Every body, not only the awake ones: one that fell asleep in
            // this step moved in it too.
            JPH::BodyInterface const& bodies = system->GetBodyInterfaceNoLock();
            for (auto const [entity, rigid] : registry.view<RigidBody>().each())
            {
                rigid.previous_position = rigid.position;
                rigid.previous_rotation = rigid.rotation;
                if (rigid.added)
                {
                    JPH::RVec3 position;
                    JPH::Quat  rotation;
                    bodies.GetPositionAndRotation(body_id(rigid), position, rotation);
                    rigid.position = from_jolt(position);
                    rigid.rotation = from_jolt(rotation);
                }
            }
        }
    };

    PhysicsWorld::PhysicsWorld() = default;

    PhysicsWorld::~PhysicsWorld() = default;

    void PhysicsWorld::init(u32 threads, u32 collision_lod)
    {
        // Before anything of Jolt's exists, the Impl's tables included: they
        // allocate through these.
        install_allocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        impl_       = std::make_unique<Impl>();
        impl_->live = true;
        impl_->lod  = collision_lod;

        impl_->broad_phase.MapObjectToBroadPhaseLayer(kStatic, JPH::BroadPhaseLayer{0});
        impl_->broad_phase.MapObjectToBroadPhaseLayer(kMoving, JPH::BroadPhaseLayer{1});
        impl_->object_filter.EnableCollision(kStatic, kMoving);
        impl_->object_filter.EnableCollision(kMoving, kMoving);
        impl_->broad_filter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
            impl_->broad_phase, kLayerCount, impl_->object_filter, kLayerCount);

        impl_->system = std::make_unique<JPH::PhysicsSystem>();
        impl_->system->Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContacts, impl_->broad_phase,
                            *impl_->broad_filter, impl_->object_filter);
        // Gravity is each body's, applied as a force before every step.
        impl_->system->SetGravity(JPH::Vec3::sZero());

        impl_->temp = std::make_unique<JPH::TempAllocatorImpl>(kTempBytes);
        impl_->jobs = std::make_unique<JPH::JobSystemThreadPool>();
        impl_->jobs->SetThreadInitFunction([](int index) {
            set_current_thread_name("physics" + std::to_string(index));
        });
        impl_->jobs->Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(threads));

        log::info("physics: Jolt, %u threads of its own and the main thread, %.0f Hz, collision at LOD %u",
                  threads, config::kPhysicsHz, collision_lod);
    }

    void PhysicsWorld::add_box(entt::registry& registry, entt::entity entity)
    {
        f32vec3 const half = registry.get<Transform>(entity).scale * 0.5f;

        // Jolt's default convex radius rounds the box's edges by 5 cm; a box
        // thinner than that takes a smaller one.
        f32 const radius = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({half.x, half.y, half.z}));
        impl_->add(registry, entity, new JPH::BoxShape{JPH::Vec3{half.x, half.y, half.z}, radius},
                   static_cast<f64>(glm::length(half)));
    }

    void PhysicsWorld::add_sphere(entt::registry& registry, entt::entity entity)
    {
        f32 const radius = registry.get<Transform>(entity).scale.x * 0.5f;
        impl_->add(registry, entity, new JPH::SphereShape{radius}, static_cast<f64>(radius));
    }

    void PhysicsWorld::update(entt::registry& registry, WorkerPool& pool, f64 seconds, bool fixed_frame)
    {
        if (!impl_)
        {
            return;
        }

        impl_->add_built();

        // The ground every held or moving body will touch is built; asleep, a
        // body stays where it is, over ground already built. Every body,
        // asleep or not, keeps the ground around it, out to twice the margin
        // it is built to, so a body resting near a chunk's edge does not make
        // the chunks past it come and go.
        vector<Ground> const all     = grounds(registry);
        JPH::BodyInterface&  bodies  = impl_->system->GetBodyInterface();
        bool                 waiting = false;
        for (auto const [entity, rigid] : registry.view<RigidBody>().each())
        {
            Ground const* const ground = impl_->nearest(all, rigid.position);
            if (ground == nullptr)
            {
                continue;
            }
            if (!rigid.added || bodies.IsActive(body_id(rigid)))
            {
                waiting |= !impl_->ensure(*ground, rigid.position, rigid.bound + config::kCollisionMargin, pool);
            }
            impl_->keep(*ground, rigid.position, rigid.bound + 2.0 * config::kCollisionMargin);
        }

        f64 alpha = 1.0;
        if (!waiting)
        {
            for (auto const [entity, rigid] : registry.view<RigidBody>().each())
            {
                if (!rigid.added)
                {
                    bodies.AddBody(body_id(rigid), JPH::EActivation::Activate);
                    rigid.added = true;
                    --impl_->held;
                    ++impl_->dynamic;
                }
            }

            f64 const period = 1.0 / config::kPhysicsHz;
            u32       steps  = 1;
            if (!fixed_frame)
            {
                impl_->accumulator += seconds;
                steps = static_cast<u32>(impl_->accumulator / period);
                impl_->accumulator -= static_cast<f64>(steps) * period;
                if (steps > config::kPhysicsMaxSteps)
                {
                    steps              = config::kPhysicsMaxSteps;
                    impl_->accumulator = 0.0;
                }
                alpha = impl_->accumulator / period;
            }

            for (u32 step = 0; step < steps; ++step)
            {
                impl_->step(registry);
            }
            impl_->evict();
        }
        else
        {
            // Time spent waiting for ground is not simulated later.
            impl_->accumulator = 0.0;
        }

        for (auto const [entity, rigid, transform] : registry.view<RigidBody, Transform>().each())
        {
            if (!rigid.added)
            {
                continue;
            }
            transform.position = glm::mix(rigid.previous_position, rigid.position, alpha);
            transform.rotation = glm::slerp(rigid.previous_rotation, rigid.rotation, alpha);
        }
    }

    bool PhysicsWorld::idle() const
    {
        return !impl_ || (impl_->held == 0 && impl_->building == 0 &&
                          impl_->system->GetNumActiveBodies(JPH::EBodyType::RigidBody) == 0);
    }

    u32 PhysicsWorld::bodies() const
    {
        return impl_ ? impl_->dynamic + impl_->held : 0;
    }

    u32 PhysicsWorld::collision_chunks() const
    {
        return impl_ ? static_cast<u32>(impl_->chunks.size()) : 0;
    }

    u32 PhysicsWorld::awake() const
    {
        return impl_ ? impl_->system->GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0;
    }
}
