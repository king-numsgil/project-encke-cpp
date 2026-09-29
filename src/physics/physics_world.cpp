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
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <unordered_map>

#include <glm/gtx/quaternion.hpp>

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

        // A static body's user data when it is a StaticCollider, not ground.
        constexpr u64 kStaticProp = 1;

        constexpr u32 kMaxBodies     = 65'536;
        constexpr u32 kMaxBodyPairs  = 65'536;
        constexpr u32 kMaxContacts   = 16'384;
        constexpr u32 kTempBytes     = 16u << 20;

        // A body's shape taken apart into the leaf shapes that make it up.
        struct LeafShapes final : JPH::TransformedShapeCollector
        {
            vector<JPH::TransformedShape> shapes;

            void AddHit(JPH::TransformedShape const& shape) override { shapes.push_back(shape); }
        };

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

        JPH::Vec3 to_jolt_vector(f64vec3 const& v)
        {
            return JPH::Vec3{static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)};
        }

        f64vec3 from_jolt_vector(JPH::Vec3Arg v)
        {
            return f64vec3{v.GetX(), v.GetY(), v.GetZ()};
        }

        // The capsule standing on its feet: Jolt's capsule is centred and
        // along +Y, so it is lifted half its height, which puts the
        // character's position at its feet.
        JPH::ShapeRefC character_shape()
        {
            auto const radius = static_cast<f32>(config::kCharacterRadius);
            auto const half   = static_cast<f32>(0.5 * config::kCharacterHeight);
            JPH::RotatedTranslatedShapeSettings const settings{
                JPH::Vec3{0.0f, half, 0.0f}, JPH::Quat::sIdentity(), new JPH::CapsuleShape{half - radius, radius}};
            return settings.Create().Get();
        }

        // A turn taking +Y to `up`, for the capsule to stand along it.
        f64quat upright(f64vec3 const& up)
        {
            return glm::rotation(f64vec3{0.0, 1.0, 0.0}, up);
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

        // Away from gravity, or +Y where there is none.
        f64vec3 up_from(f64vec3 const& gravity)
        {
            return glm::length(gravity) > 0.0 ? -glm::normalize(gravity) : f64vec3{0.0, 1.0, 0.0};
        }

        // A physics space: one Jolt system, the world's or a ship's. A ship's
        // frame is its entity's world pose, read once an update; the world's
        // is the identity, so the conversions hold for both.
        // A convex piece of a ship's volume, as the planes bounding it in the
        // ship's frame: xyz an outward unit normal, w its distance from the
        // origin, so a point is inside where every dot(n, p) - w is under 0.
        struct Volume
        {
            vector<f64vec4> planes;
        };

        // A piece of a ship's hull, waiting for the ship's body to be made.
        struct HullPiece
        {
            JPH::ShapeRefC shape;
            f64vec3        position{0.0};
            f64quat        rotation{1.0, 0.0, 0.0, 0.0};
        };

        struct Space
        {
            entt::entity                        ship = entt::null;   // null: the world's
            vector<Volume>                      volumes;
            std::unique_ptr<JPH::PhysicsSystem> system;

            // A ship whose Hull shapes have been taken in: its pieces until
            // its body is made, and then whether it has one, whose pose and
            // motion the frame follows step by step.
            vector<HullPiece> hull;
            bool              body = false;

            f64vec3 position{0.0};
            f64quat rotation{1.0, 0.0, 0.0, 0.0};

            // The frame's velocity, spin (radians a second about a world
            // axis) and acceleration, from its pose across updates.
            f64vec3 velocity{0.0};
            f64vec3 spin{0.0};
            f64vec3 acceleration{0.0};
            bool    placed = false;

            // A ship's gravity in its own frame, this update's: the world's
            // at the ship less the ship's acceleration.
            f64vec3 gravity{0.0};

            bool world() const { return ship == entt::null; }

            f64vec3 to_world(f64vec3 const& local) const { return position + rotation * local; }
            f64vec3 to_local(f64vec3 const& point) const { return glm::conjugate(rotation) * (point - position); }
            f64vec3 vector_to_world(f64vec3 const& v) const { return rotation * v; }
            f64vec3 vector_to_local(f64vec3 const& v) const { return glm::conjugate(rotation) * v; }

            // The frame's own velocity at a world point, which a body keeps
            // leaving it and gives up entering it.
            f64vec3 carried(f64vec3 const& point) const { return velocity + glm::cross(spin, point - position); }

            // How far a point in the frame is outside the volume, negative
            // inside: the least over its pieces of the most over each
            // piece's planes, which is exact inside and never more than the
            // true distance outside. Infinite with no volume.
            f64 distance(f64vec3 const& local) const
            {
                f64 nearest = std::numeric_limits<f64>::infinity();
                for (Volume const& piece : volumes)
                {
                    f64 outside = -std::numeric_limits<f64>::infinity();
                    for (f64vec4 const& plane : piece.planes)
                    {
                        outside = std::max(outside, glm::dot(f64vec3{plane}, local) - plane.w);
                    }
                    nearest = std::min(nearest, outside);
                }
                return nearest;
            }
        };

        // A box's six planes, or a convex mesh's, one per triangle, placed by
        // `position` and `rotation` after `scale`. A triangle's normal is
        // turned outward from the mesh's centroid, whichever way it winds.
        Volume volume_from(NodeShape const& shape, CollisionMesh const* mesh, f64vec3 const& position,
                           f64quat const& rotation, f64vec3 const& scale)
        {
            Volume out;
            if (shape.kind == NodeShape::Kind::Box)
            {
                for (glm::length_t axis = 0; axis < 3; ++axis)
                {
                    f64vec3 direction{0.0};
                    direction[axis]    = 1.0;
                    f64vec3 const n    = rotation * direction;
                    f64 const     half = shape.half_extents[axis] * scale[axis];
                    out.planes.emplace_back(n, glm::dot(n, position) + half);
                    out.planes.emplace_back(-n, glm::dot(-n, position) + half);
                }
                return out;
            }
            if (mesh == nullptr || mesh->positions.empty())
            {
                return out;
            }

            vector<f64vec3> points;
            points.reserve(mesh->positions.size());
            f64vec3 centroid{0.0};
            for (f32vec3 const& p : mesh->positions)
            {
                points.push_back(position + rotation * (f64vec3{p} * scale));
                centroid += points.back();
            }
            centroid /= static_cast<f64>(points.size());

            for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3)
            {
                f64vec3 const a = points[mesh->indices[i]];
                f64vec3 n = glm::cross(points[mesh->indices[i + 1]] - a, points[mesh->indices[i + 2]] - a);
                f64 const length = glm::length(n);
                if (length < 1.0e-12)
                {
                    continue;
                }
                n /= length;
                if (glm::dot(n, a - centroid) < 0.0)
                {
                    n = -n;
                }
                out.planes.emplace_back(n, glm::dot(n, a));
            }
            return out;
        }
    }

    struct PhysicsWorld::Impl
    {
        bool live = false;

        // Shared by every space's system: they are fixed tables, and the
        // spaces step one after another, never at once.
        JPH::BroadPhaseLayerInterfaceTable                       broad_phase{kLayerCount, kLayerCount};
        JPH::ObjectLayerPairFilterTable                          object_filter{kLayerCount};
        std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> broad_filter;
        std::unique_ptr<JPH::TempAllocatorImpl>                  temp;
        std::unique_ptr<JPH::JobSystemThreadPool>                jobs;

        // The world's first, then ships' in the order they were found, which
        // is the order they step in.
        vector<std::unique_ptr<Space>> spaces;

        std::unordered_map<ChunkId, Chunk, ChunkIdHash> chunks;
        u32                                             building = 0;

        // One per pool worker, each keyed by terrain, made on first use by
        // that worker alone.
        vector<std::unordered_map<terrain::BodyTerrain const*, std::unique_ptr<terrain::TerrainSampler>>> samplers;

        // Static mesh and hull shapes, by their triangles, kind and scale,
        // so a mesh instanced many times is built once. Null where the
        // shape failed, so it fails once.
        std::map<std::tuple<std::shared_ptr<CollisionMesh const>, StaticCollider::Shape, f32, f32, f32>,
                 JPH::ShapeRefC>
            static_shapes;

        u32 lod         = config::kCollisionLod;
        f64 accumulator = 0.0;
        u32 held        = 0;
        u32 dynamic     = 0;
        u64 steps       = 0;

        // Steps the last update took, which the ships' poses moved across.
        u32 last_steps = 0;

        // The walking character, if there is one, in `character_space`: its
        // up and its velocity along the ground in that space's frame, which
        // eases toward what is asked, and its feet at the last two steps in
        // the world, for the camera to go between as bodies' Transforms do.
        JPH::Ref<JPH::CharacterVirtual> character;
        Space*                          character_space = nullptr;
        bool                            character_held  = true;
        CharacterInput                  character_input;
        f64vec3                         character_previous{0.0};
        f64vec3                         character_feet{0.0};
        f64vec3                         character_up{0.0, 1.0, 0.0};
        JPH::Vec3                       character_across = JPH::Vec3::sZero();
        f64                             alpha = 1.0;

        Space&       world() { return *spaces.front(); }
        Space const& world() const { return *spaces.front(); }

        // The space of a ship, or the world's for null. Null for a ship
        // that has none.
        Space* find(entt::entity ship)
        {
            for (std::unique_ptr<Space>& space : spaces)
            {
                if (space->ship == ship)
                {
                    return space.get();
                }
            }
            return nullptr;
        }

        Space& space_of(entt::entity ship)
        {
            Space* const space = find(ship);
            return space != nullptr ? *space : world();
        }

        // The space whose volume a sphere at a world point reaches into, or
        // the world's.
        Space const& containing(f64vec3 const& point, f64 radius) const
        {
            for (size_t index = 1; index < spaces.size(); ++index)
            {
                if (spaces[index]->distance(spaces[index]->to_local(point)) < radius)
                {
                    return *spaces[index];
                }
            }
            return world();
        }

        Space& containing(f64vec3 const& point, f64 radius)
        {
            return const_cast<Space&>(std::as_const(*this).containing(point, radius));
        }

        // Where a sphere at a world point, now in `current`, belongs. It
        // enters a ship as soon as it reaches into the volume, so a body
        // pressed against a closed door reaches the door's volume, which
        // stands out past the leaves, before the hull stops it; and it
        // leaves only once it is clear of the volume by the margin.
        Space& destination(f64vec3 const& point, f64 radius, Space& current)
        {
            if (!current.world() &&
                current.distance(current.to_local(point)) < radius + config::kShipSpaceHysteresis)
            {
                return current;
            }
            for (size_t index = 1; index < spaces.size(); ++index)
            {
                Space& space = *spaces[index];
                if (&space != &current && space.distance(space.to_local(point)) < radius)
                {
                    return space;
                }
            }
            return world();
        }

        std::unique_ptr<JPH::PhysicsSystem> make_system(u32 max_bodies)
        {
            auto system = std::make_unique<JPH::PhysicsSystem>();
            system->Init(max_bodies, 0, kMaxBodyPairs, kMaxContacts, broad_phase, *broad_filter, object_filter);
            // The world's gravity is each body's, applied as a force before
            // every step; a ship's is set on its system then.
            system->SetGravity(JPH::Vec3::sZero());
            return system;
        }

        // A space for every ship not yet given one, and, with `elapsed`, the
        // simulated time since the last call, every ship's pose, motion and
        // gravity. The pose is its entity's WorldTransform, so this reads
        // where the last Scene::update put it.
        void sync(entt::registry const& registry, optional<f64> elapsed)
        {
            for (auto const [entity, ship, world_transform] : registry.view<ShipSpace const, WorldTransform const>().each())
            {
                Space* space = find(entity);
                if (space == nullptr)
                {
                    auto made    = std::make_unique<Space>();
                    made->ship   = entity;
                    made->system = make_system(config::kShipMaxBodies);
                    space        = made.get();
                    spaces.push_back(std::move(made));
                    log::info("physics: a ship's space, %.0f t", ship.mass / 1000.0);
                }

                // A ship with a body follows it step by step instead.
                if (space->body || (space->placed && !elapsed.has_value()))
                {
                    continue;
                }
                if (space->placed && *elapsed > 0.0)
                {
                    f64 const     dt       = *elapsed;
                    f64vec3 const velocity = (world_transform.position - space->position) / dt;
                    space->acceleration    = (velocity - space->velocity) / dt;
                    space->velocity        = velocity;

                    f64quat turn = world_transform.rotation * glm::conjugate(space->rotation);
                    if (turn.w < 0.0)
                    {
                        turn = -turn;
                    }
                    f64 const angle = glm::angle(turn);
                    space->spin     = angle > 1.0e-12 ? glm::axis(turn) * (angle / dt) : f64vec3{0.0};
                }
                space->position = world_transform.position;
                space->rotation = world_transform.rotation;
                space->placed   = true;
                space->gravity  = space->vector_to_local(gravity_at(registry, space->position) - space->acceleration);
            }
        }

        // Every ShipShape not yet taken in, into its ship: a Volume piece as
        // planes in the ship's frame, a Hull piece as a convex shape placed
        // in it. Then a body for every ship with hull pieces and none yet.
        // Poses are relative to the ship's WorldTransform, so they hold
        // wherever the ship is.
        void take_shapes(entt::registry& registry)
        {
            vector<entt::entity> taken;
            for (auto const [entity, piece, world_transform] :
                 registry.view<ShipShape const, WorldTransform const>(entt::exclude<ShipShapeTaken>).each())
            {
                Space* const space = find(piece.ship);
                if (space == nullptr || piece.ship == entt::null)
                {
                    continue;
                }
                taken.push_back(entity);

                WorldTransform const& ship     = registry.get<WorldTransform>(piece.ship);
                f64quat const         undo     = glm::conjugate(ship.rotation);
                f64vec3 const         position = undo * (world_transform.position - ship.position);
                f64quat const         rotation = undo * world_transform.rotation;
                f64vec3 const         scale{world_transform.scale};

                if (piece.role == ShipShape::Role::Volume)
                {
                    space->volumes.push_back(volume_from(piece.shape, piece.mesh.get(), position, rotation, scale));
                    continue;
                }

                JPH::ShapeSettings::ShapeResult result;
                if (piece.shape.kind == NodeShape::Kind::Box)
                {
                    f64vec3 const half = piece.shape.half_extents * scale;
                    f32 const radius = std::min(JPH::cDefaultConvexRadius,
                                                0.5f * static_cast<f32>(std::min({half.x, half.y, half.z})));
                    result = JPH::BoxShapeSettings{to_jolt_vector(half), radius}.Create();
                }
                else if (piece.mesh != nullptr)
                {
                    JPH::Array<JPH::Vec3> points;
                    points.reserve(piece.mesh->positions.size());
                    for (f32vec3 const& p : piece.mesh->positions)
                    {
                        points.push_back(to_jolt_vector(f64vec3{p} * scale));
                    }
                    result = JPH::ConvexHullShapeSettings{points}.Create();
                }
                if (!result.IsValid())
                {
                    log::warn("physics: a piece of a ship's hull was not made: %s",
                              result.HasError() ? result.GetError().c_str() : "no vertices");
                    continue;
                }
                space->hull.push_back(HullPiece{result.Get(), position, rotation});
            }
            for (entt::entity const entity : taken)
            {
                registry.emplace<ShipShapeTaken>(entity);
            }

            for (size_t index = 1; index < spaces.size(); ++index)
            {
                Space& space = *spaces[index];
                if (!space.body && !space.hull.empty())
                {
                    make_ship_body(registry, space);
                }
            }
        }

        // The ship's body in the world: its hull pieces as one compound, of
        // the ship's mass, at its Transform's pose, held like any new body.
        // The ship's root is then a RigidBody, written by the world.
        void make_ship_body(entt::registry& registry, Space& space)
        {
            JPH::StaticCompoundShapeSettings compound;
            for (HullPiece const& piece : space.hull)
            {
                compound.AddShape(to_jolt_vector(piece.position), to_jolt(piece.rotation), piece.shape);
            }
            JPH::ShapeSettings::ShapeResult const result = compound.Create();
            if (!result.IsValid())
            {
                log::error("physics: a ship's hull was not made: %s", result.GetError().c_str());
                space.hull.clear();
                return;
            }

            f64 const          mass  = registry.get<ShipSpace>(space.ship).mass;
            JPH::AABox const   box   = result.Get()->GetLocalBounds();
            f64 const          bound = std::max(glm::length(from_jolt_vector(box.mMin)),
                                                glm::length(from_jolt_vector(box.mMax)));
            JPH::BodyCreationSettings settings = dynamic_settings(registry.get<Transform>(space.ship), result.Get());
            if (mass > 0.0)
            {
                settings.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = static_cast<f32>(mass);
            }
            add_body(registry, space.ship, settings, bound);
            space.body = true;
            space.hull.clear();
            log::info("physics: a ship's hull of %zu pieces, %.0f t, %.1f m from its origin at most",
                      compound.mSubShapes.size(), mass / 1000.0, bound);
        }

        // Before each step, a ship with a body takes its frame from it: its
        // pose, velocity and spin, and its acceleration from the change in
        // velocity since the last step, which the gravity aboard subtracts.
        void follow_ships(entt::registry const& registry, f64 dt)
        {
            JPH::BodyInterface const& bodies = world().system->GetBodyInterfaceNoLock();
            for (size_t index = 1; index < spaces.size(); ++index)
            {
                Space& space = *spaces[index];
                if (!space.body)
                {
                    continue;
                }
                RigidBody const& rigid = registry.get<RigidBody>(space.ship);
                JPH::BodyID const id   = body_id(rigid);

                JPH::RVec3 position;
                JPH::Quat  rotation;
                bodies.GetPositionAndRotation(id, position, rotation);
                f64vec3 const velocity = from_jolt_vector(bodies.GetLinearVelocity(id));

                space.acceleration = space.placed ? (velocity - space.velocity) / dt : f64vec3{0.0};
                space.velocity     = velocity;
                space.spin         = from_jolt_vector(bodies.GetAngularVelocity(id));
                space.position     = from_jolt(position);
                space.rotation     = from_jolt(rotation);
                space.placed       = true;
                space.gravity = space.vector_to_local(gravity_at(registry, space.position) - space.acceleration);
            }
        }

        // A StaticCollider's shape at `scale`, which is baked into a mesh's
        // or hull's points. Null if it could not be made, logged once.
        JPH::ShapeRefC static_shape(StaticCollider const& collider, f32vec3 const& scale)
        {
            f32vec3 const half = scale * 0.5f;
            switch (collider.shape)
            {
            case StaticCollider::Shape::Sphere:
                return new JPH::SphereShape{half.x};
            case StaticCollider::Shape::Box:
            {
                f32 const radius = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({half.x, half.y, half.z}));
                return new JPH::BoxShape{JPH::Vec3{half.x, half.y, half.z}, radius};
            }
            case StaticCollider::Shape::Mesh:
            case StaticCollider::Shape::Hull:
                break;
            }
            if (collider.mesh == nullptr)
            {
                return nullptr;
            }

            auto const key = std::make_tuple(collider.mesh, collider.shape, scale.x, scale.y, scale.z);
            if (auto const found = static_shapes.find(key); found != static_shapes.end())
            {
                return found->second;
            }

            CollisionMesh const&             mesh = *collider.mesh;
            JPH::ShapeSettings::ShapeResult result;
            if (collider.shape == StaticCollider::Shape::Mesh)
            {
                JPH::VertexList vertices;
                vertices.reserve(mesh.positions.size());
                for (f32vec3 const& p : mesh.positions)
                {
                    vertices.push_back(JPH::Float3{p.x * scale.x, p.y * scale.y, p.z * scale.z});
                }
                JPH::IndexedTriangleList triangles;
                triangles.reserve(mesh.indices.size() / 3);
                for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
                {
                    triangles.push_back(JPH::IndexedTriangle{mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2], 0});
                }
                JPH::MeshShapeSettings const settings{std::move(vertices), std::move(triangles)};
                result = settings.Create();
            }
            else
            {
                JPH::Array<JPH::Vec3> points;
                points.reserve(mesh.positions.size());
                for (f32vec3 const& p : mesh.positions)
                {
                    points.push_back(JPH::Vec3{p.x * scale.x, p.y * scale.y, p.z * scale.z});
                }
                JPH::ConvexHullShapeSettings const settings{points};
                result = settings.Create();
            }

            JPH::ShapeRefC shape;
            if (result.IsValid())
            {
                shape = result.Get();
            }
            else
            {
                log::warn("physics: a static %s of %zu triangles was not made: %s",
                          collider.shape == StaticCollider::Shape::Mesh ? "mesh" : "hull", mesh.indices.size() / 3,
                          result.GetError().c_str());
            }
            static_shapes.emplace(key, shape);
            return shape;
        }

        // The character's feet, in the world.
        f64vec3 feet() const
        {
            return character ? character_space->to_world(from_jolt(character->GetPosition())) : f64vec3{0.0};
        }

        // Awake bodies in every space, the character's inner body left out:
        // it is kinematic, moved every step, and Jolt keeps it active.
        u32 awake_bodies() const
        {
            u32 awake = 0;
            for (std::unique_ptr<Space> const& space : spaces)
            {
                awake += space->system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
            }
            if (character && character_space->system->GetBodyInterfaceNoLock().IsActive(character->GetInnerBodyID()))
            {
                --awake;
            }
            return awake;
        }

        // A new character in `space`, feet and up in its frame; the one
        // there was goes, and its inner body with it.
        void make_character(Space& space, f64vec3 const& feet_local, f64vec3 const& up_local)
        {
            character    = nullptr;
            character_up = up_local;

            JPH::CharacterVirtualSettings settings;
            settings.mShape          = character_shape();
            settings.mInnerBodyShape = settings.mShape;
            settings.mInnerBodyLayer = kMoving;
            settings.mMaxSlopeAngle  = JPH::DegreesToRadians(static_cast<f32>(config::kMaxSlopeDegrees));
            settings.mMass           = static_cast<f32>(config::kCharacterMass);
            settings.mUp             = to_jolt_vector(up_local);
            // Contacts count as ground only on the bottom hemisphere, not the
            // capsule's side.
            settings.mSupportingVolume = JPH::Plane{JPH::Vec3::sAxisY(), -static_cast<f32>(config::kCharacterRadius)};

            character       = new JPH::CharacterVirtual{&settings, to_jolt(feet_local), to_jolt(upright(up_local)), 0,
                                                        space.system.get()};
            character_space = &space;
        }

        // One step of the character, before the bodies': Jolt's
        // CharacterVirtual sample, generalised to an up that turns with the
        // ground's body. On walkable ground it takes the ground's velocity
        // plus its own, easing toward what is asked, and a jump; in the air
        // it keeps its fall and most of what it had across. All of it in its
        // space's frame.
        void step_character(entt::registry const& registry, f32 dt)
        {
            Space& space = *character_space;

            f64vec3 const g = space.world() ? gravity_at(registry, feet()) : space.gravity;
            f64vec3 const up = glm::length(g) > 0.0 ? -glm::normalize(g) : character_up;
            character_up     = up;

            JPH::Vec3 const up_j = to_jolt_vector(up);
            character->SetUp(up_j);
            character->SetRotation(to_jolt(upright(up)));
            character->UpdateGroundVelocity();

            JPH::Vec3 const velocity = character->GetLinearVelocity();
            JPH::Vec3 const vertical = up_j * velocity.Dot(up_j);
            JPH::Vec3 const ground   = character->GetGroundVelocity();

            f64vec3 const   asked  = space.vector_to_local(character_input.move);
            f64vec3 const   move   = asked - up * glm::dot(asked, up);
            JPH::Vec3 const wanted = to_jolt_vector(move);

            bool const toward_ground = velocity.Dot(up_j) - ground.Dot(up_j) < 0.1f;
            bool const standing      = character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround &&
                                  !character->IsSlopeTooSteep(character->GetGroundNormal()) && toward_ground;

            JPH::Vec3 next;
            if (standing)
            {
                f32 const ease = std::min(1.0f, dt * static_cast<f32>(config::kGroundResponse));
                character_across += (wanted - character_across) * ease;
                next = ground + character_across;
                if (character_input.jump)
                {
                    next += up_j * static_cast<f32>(config::kJumpSpeed);
                }
            }
            else
            {
                f32 const ease   = std::min(1.0f, dt * static_cast<f32>(config::kAirResponse));
                character_across = velocity - vertical;
                character_across += (wanted - character_across) * ease;
                next = vertical + character_across;
            }

            // Standing on walkable ground, no gravity: along a slope it slid
            // the character downhill whenever it stopped, and even pulled
            // along the ground's normal it crept, by the few millimetres a
            // second the contact solver left of the sideways part. The
            // step-down in ExtendedUpdate keeps it on the ground instead.
            JPH::Vec3 const gravity = to_jolt_vector(g);
            character->SetLinearVelocity(standing ? next : next + gravity * dt);

            JPH::CharacterVirtual::ExtendedUpdateSettings update;
            update.mStickToFloorStepDown = -up_j * static_cast<f32>(config::kStepDown);
            update.mWalkStairsStepUp     = up_j * static_cast<f32>(config::kStepUp);

            JPH::IgnoreSingleBodyFilter const self{character->GetInnerBodyID()};
            character->ExtendedUpdate(dt, gravity, update, space.system->GetDefaultBroadPhaseLayerFilter(kMoving),
                                      space.system->GetDefaultLayerFilter(kMoving), self, {}, *temp);

            character_previous = character_feet;
            character_feet     = feet();
        }

        // Moves a body into another space: made there with its pose and its
        // velocity in the world, then removed here. Its Transform is
        // reparented to the new space's ship, and its last two poses are
        // carried into its frame, so it draws where it was.
        void move_body(RigidBody& rigid, Transform& transform, Space& from, Space& to)
        {
            JPH::BodyID const         id = body_id(rigid);
            JPH::BodyCreationSettings settings;
            {
                JPH::BodyLockRead const lock{from.system->GetBodyLockInterfaceNoLock(), id};
                if (!lock.Succeeded())
                {
                    return;
                }
                settings = lock.GetBody().GetBodyCreationSettings();
            }

            f64vec3 const point = from.to_world(from_jolt(settings.mPosition));
            f64quat const turn  = from.rotation * from_jolt(settings.mRotation);
            f64vec3 const velocity =
                from.vector_to_world(from_jolt_vector(settings.mLinearVelocity)) + from.carried(point);
            f64vec3 const spin = from.vector_to_world(from_jolt_vector(settings.mAngularVelocity)) + from.spin;

            settings.mPosition        = to_jolt(to.to_local(point));
            settings.mRotation        = to_jolt(glm::conjugate(to.rotation) * turn);
            settings.mLinearVelocity  = to_jolt_vector(to.vector_to_local(velocity - to.carried(point)));
            settings.mAngularVelocity = to_jolt_vector(to.vector_to_local(spin - to.spin));

            JPH::BodyID const moved = to.system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
            if (moved.IsInvalid())
            {
                log::error("physics: out of bodies; one stays where it was");
                return;
            }
            JPH::BodyInterface& source = from.system->GetBodyInterface();
            source.RemoveBody(id);
            source.DestroyBody(id);

            auto const carry  = [&](f64vec3 const& p) { return to.to_local(from.to_world(p)); };
            auto const rotate = [&](f64quat const& q) { return glm::conjugate(to.rotation) * (from.rotation * q); };
            rigid.previous_position = carry(rigid.previous_position);
            rigid.position          = carry(rigid.position);
            rigid.previous_rotation = rotate(rigid.previous_rotation);
            rigid.rotation          = rotate(rigid.rotation);
            rigid.body              = moved.GetIndexAndSequenceNumber();
            rigid.space             = to.ship;
            transform.parent        = to.ship;

            log::info("physics: a body moved into %s", to.world() ? "the world" : "a ship");
        }

        // Moves the character into another space, keeping where it stands,
        // how it moves in the world and which way is up.
        void move_character(Space& to)
        {
            Space const&  from     = *character_space;
            f64vec3 const point    = feet();
            f64vec3 const velocity = from.vector_to_world(from_jolt_vector(character->GetLinearVelocity())) +
                                     from.carried(point);
            f64vec3 const across   = from.vector_to_world(from_jolt_vector(character_across));
            f64vec3 const up       = from.vector_to_world(character_up);

            make_character(to, to.to_local(point), to.vector_to_local(up));
            character->SetLinearVelocity(to_jolt_vector(to.vector_to_local(velocity - to.carried(point))));
            character_across = to_jolt_vector(to.vector_to_local(across));

            log::info("physics: the character moved into %s", to.world() ? "the world" : "a ship");
        }

        // Everything that has crossed a ship's volume moves to the space it
        // is now in. Before each step, so every body steps in its own.
        void transfer(entt::registry& registry)
        {
            if (spaces.size() < 2)
            {
                return;
            }
            // Bodies by their bounding sphere; a ship itself stays in the world.
            for (auto const [entity, rigid, transform] :
                 registry.view<RigidBody, Transform>(entt::exclude<ShipSpace>).each())
            {
                if (!rigid.added)
                {
                    continue;
                }
                Space& from = space_of(rigid.space);
                Space& to   = destination(from.to_world(rigid.position), rigid.bound, from);
                if (&to != &from)
                {
                    move_body(rigid, transform, from, to);
                }
            }

            // The character by its capsule's middle and radius.
            if (character && !character_held)
            {
                f64vec3 const middle = feet() + character_space->vector_to_world(character_up) *
                                                    (0.5 * config::kCharacterHeight);
                Space& to = destination(middle, config::kCharacterRadius, *character_space);
                if (&to != character_space)
                {
                    move_character(to);
                }
            }
        }

        ~Impl()
        {
            if (!live)
            {
                return;
            }
            // It removes its inner body from its system as it goes.
            character = nullptr;
            spaces.clear();
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

            JPH::BodyInterface& bodies = world().system->GetBodyInterface();
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

            JPH::BodyInterface& bodies = world().system->GetBodyInterface();
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

        // A dynamic body of `shape` at a Transform's pose.
        static JPH::BodyCreationSettings dynamic_settings(Transform const& transform, JPH::ShapeRefC const& shape)
        {
            JPH::BodyCreationSettings settings{shape, to_jolt(transform.position), to_jolt(transform.rotation),
                                               JPH::EMotionType::Dynamic, kMoving};
            // Swept, not only tested where each step ends. A body that moves
            // more than half its size in a step can otherwise pass a
            // one-sided terrain triangle between two steps: a half-metre box
            // dropped from 60 m went straight through the ground.
            settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
            return settings;
        }

        // A dynamic body on `entity` at its Transform's pose, held, in the
        // world: a root. Once added it moves into a ship whose volume it is
        // in.
        void add(entt::registry& registry, entt::entity entity, JPH::ShapeRefC const& shape, f64 bound)
        {
            add_body(registry, entity, dynamic_settings(registry.get<Transform>(entity), shape), bound);
        }

        void add_body(entt::registry& registry, entt::entity entity, JPH::BodyCreationSettings const& settings,
                      f64 bound)
        {
            Transform const& transform = registry.get<Transform>(entity);
            JPH::Body* const body      = world().system->GetBodyInterface().CreateBody(settings);
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
                                                    .space             = entt::null,
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

            follow_ships(registry, static_cast<f64>(dt));
            transfer(registry);

            // Each awake body in the world gets its own gravity, as a force
            // for this step.
            JPH::PhysicsSystem& system = *world().system;
            JPH::BodyIDVector   active;
            system.GetActiveBodies(JPH::EBodyType::RigidBody, active);
            JPH::BodyLockInterfaceNoLock const& locks = system.GetBodyLockInterfaceNoLock();
            for (JPH::BodyID const id : active)
            {
                JPH::BodyLockWrite lock{locks, id};
                if (!lock.Succeeded())
                {
                    continue;
                }
                // The character's inner body is kinematic: moved, not pulled.
                JPH::Body& body = lock.GetBody();
                if (!body.IsDynamic())
                {
                    continue;
                }
                f32 const inverse_mass = body.GetMotionProperties()->GetInverseMass();
                if (inverse_mass <= 0.0f)
                {
                    continue;
                }
                f64vec3 const g = gravity_at(registry, from_jolt(body.GetCenterOfMassPosition()));
                body.AddForce(JPH::Vec3{static_cast<f32>(g.x), static_cast<f32>(g.y), static_cast<f32>(g.z)} /
                              inverse_mass);
            }

            // A ship's is one vector in its frame, the same for everything
            // aboard.
            for (size_t index = 1; index < spaces.size(); ++index)
            {
                spaces[index]->system->SetGravity(to_jolt_vector(spaces[index]->gravity));
            }

            if (character && !character_held)
            {
                step_character(registry, dt);
            }

            for (std::unique_ptr<Space> const& space : spaces)
            {
                space->system->Update(dt, 1, temp.get(), jobs.get());
            }
            ++steps;

            // Every body, not only the awake ones: one that fell asleep in
            // this step moved in it too.
            for (auto const [entity, rigid] : registry.view<RigidBody>().each())
            {
                rigid.previous_position = rigid.position;
                rigid.previous_rotation = rigid.rotation;
                if (rigid.added)
                {
                    JPH::BodyInterface const& bodies = space_of(rigid.space).system->GetBodyInterfaceNoLock();
                    JPH::RVec3                position;
                    JPH::Quat                 rotation;
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

        auto world    = std::make_unique<Space>();
        world->system = impl_->make_system(kMaxBodies);
        world->placed = true;
        impl_->spaces.push_back(std::move(world));

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

    void PhysicsWorld::add_static_colliders(entt::registry& registry)
    {
        if (!impl_)
        {
            return;
        }
        impl_->sync(registry, nullopt);

        // Marked after the view is done with, since marking changes a pool
        // the view excludes.
        vector<entt::entity> made;
        u32                  bodies = 0;
        u32                  failed = 0;
        for (auto const [entity, collider, world] :
             registry.view<StaticCollider const, WorldTransform const>(entt::exclude<StaticBody>).each())
        {
            made.push_back(entity);
            JPH::ShapeRefC const shape = impl_->static_shape(collider, world.scale);
            if (shape == nullptr)
            {
                ++failed;
                continue;
            }

            // In a ship's frame, relative to the ship's WorldTransform from
            // the same update, which is where the ship's body may not be.
            auto const add = [&](Space& space) {
                f64vec3 position = world.position;
                f64quat rotation = world.rotation;
                if (!space.world())
                {
                    WorldTransform const& ship = registry.get<WorldTransform>(space.ship);
                    f64quat const         undo = glm::conjugate(ship.rotation);
                    position                   = undo * (world.position - ship.position);
                    rotation                   = undo * world.rotation;
                }
                JPH::BodyCreationSettings settings{shape, to_jolt(position), to_jolt(rotation),
                                                   JPH::EMotionType::Static, kStatic};
                settings.mUserData = kStaticProp;
                space.system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
                ++bodies;
            };

            if (collider.world)
            {
                add(impl_->world());
            }
            if (collider.space != entt::null)
            {
                if (Space* const space = impl_->find(collider.space))
                {
                    add(*space);
                }
                else
                {
                    log::warn("physics: a collider names a ship with no ShipSpace");
                }
            }
        }

        for (entt::entity const entity : made)
        {
            registry.emplace<StaticBody>(entity);
        }
        if (!made.empty())
        {
            log::info("physics: %zu static colliders as %u bodies, %u of them without a shape", made.size(), bodies,
                      failed);
        }

        impl_->take_shapes(registry);
    }

    void PhysicsWorld::update(entt::registry& registry, WorkerPool& pool, f64 seconds, bool fixed_frame)
    {
        if (!impl_)
        {
            return;
        }

        impl_->sync(registry, static_cast<f64>(impl_->last_steps) / config::kPhysicsHz);
        impl_->add_built();

        // The ground every held or moving body in the world will touch is
        // built; asleep, a body stays where it is, over ground already
        // built. Every body, asleep or not, keeps the ground around it, out
        // to twice the margin it is built to, so a body resting near a
        // chunk's edge does not make the chunks past it come and go. Aboard
        // a ship there is no terrain to build.
        vector<Ground> const all     = grounds(registry);
        JPH::BodyInterface&  bodies  = impl_->world().system->GetBodyInterface();
        bool                 waiting = false;
        for (auto const [entity, rigid] : registry.view<RigidBody>().each())
        {
            if (rigid.space != entt::null)
            {
                continue;
            }
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

        // The character's ground, as a moving body's: always, since what it
        // is asked can move it at any step.
        if (impl_->character && impl_->character_space->world())
        {
            f64vec3 const       feet   = impl_->feet();
            f64 const           bound  = config::kCharacterHeight;
            if (Ground const* const ground = impl_->nearest(all, feet))
            {
                waiting |= !impl_->ensure(*ground, feet, bound + config::kCollisionMargin, pool);
                impl_->keep(*ground, feet, bound + 2.0 * config::kCollisionMargin);
            }
        }

        f64 alpha = 1.0;
        u32 taken = 0;
        if (!waiting)
        {
            if (impl_->character && impl_->character_held)
            {
                impl_->character_held     = false;
                impl_->character_feet     = impl_->feet();
                impl_->character_previous = impl_->character_feet;
            }

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
            taken = steps;
            impl_->evict();
        }
        else
        {
            // Time spent waiting for ground is not simulated later.
            impl_->accumulator = 0.0;
        }
        impl_->alpha      = alpha;
        impl_->last_steps = taken;

        // In the frame of the space each body is in, which is its parent's.
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

    void PhysicsWorld::add_character(entt::registry const& registry, f64vec3 const& feet)
    {
        remove_character();
        impl_->sync(registry, nullopt);

        // By its feet, which may stand on a volume's bottom face.
        Space&        space = impl_->containing(feet, config::kCharacterRadius);
        f64vec3 const g     = space.world() ? gravity_at(registry, feet) : space.vector_to_world(space.gravity);
        impl_->make_character(space, space.to_local(feet), space.vector_to_local(up_from(g)));

        impl_->character_held     = true;
        impl_->character_input    = {};
        impl_->character_across   = JPH::Vec3::sZero();
        impl_->character_feet     = feet;
        impl_->character_previous = feet;
    }

    void PhysicsWorld::remove_character()
    {
        impl_->character       = nullptr;
        impl_->character_space = nullptr;
    }

    bool PhysicsWorld::has_character() const
    {
        return impl_ && impl_->character;
    }

    void PhysicsWorld::teleport_character(f64vec3 const& feet)
    {
        if (!impl_->character)
        {
            return;
        }
        Space& space = impl_->containing(feet, config::kCharacterRadius);
        if (&space != impl_->character_space)
        {
            f64vec3 const up = impl_->character_space->vector_to_world(impl_->character_up);
            impl_->make_character(space, space.to_local(feet), space.vector_to_local(up));
        }
        else
        {
            impl_->character->SetPosition(to_jolt(space.to_local(feet)));
        }
        impl_->character->SetLinearVelocity(JPH::Vec3::sZero());
        impl_->character_across   = JPH::Vec3::sZero();
        impl_->character_feet     = feet;
        impl_->character_previous = feet;
    }

    void PhysicsWorld::set_character_input(CharacterInput const& input)
    {
        impl_->character_input = input;
    }

    optional<CharacterState> PhysicsWorld::character() const
    {
        if (!impl_ || !impl_->character)
        {
            return nullopt;
        }
        JPH::CharacterVirtual const& character = *impl_->character;
        Space const&                 space     = *impl_->character_space;
        f64vec3 const                feet      = glm::mix(impl_->character_previous, impl_->character_feet, impl_->alpha);
        return CharacterState{
            .feet      = feet,
            .up        = space.vector_to_world(impl_->character_up),
            .velocity  = space.vector_to_world(from_jolt_vector(character.GetLinearVelocity())) + space.carried(feet),
            .on_ground = character.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround,
            .held      = impl_->character_held,
        };
    }

    f64vec3 PhysicsWorld::up_at(entt::registry const& registry, f64vec3 const& point) const
    {
        Space const& space = impl_->containing(point, config::kCharacterRadius);
        return up_from(space.world() ? gravity_at(registry, point) : space.vector_to_world(space.gravity));
    }

    bool PhysicsWorld::idle() const
    {
        if (!impl_)
        {
            return true;
        }

        // Standing: on the ground, asked nothing, and no longer sliding
        // along it. Its velocity across is what eases, so that is what
        // settles; the fall into the ground each step is not motion.
        bool standing = true;
        if (impl_->character)
        {
            f32 const across = impl_->character_across.Length();
            standing = !impl_->character_held &&
                       impl_->character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround &&
                       glm::length(impl_->character_input.move) == 0.0 && !impl_->character_input.jump &&
                       across < 1.0e-3f;
        }

        return standing && impl_->held == 0 && impl_->building == 0 && impl_->awake_bodies() == 0;
    }

    u32 PhysicsWorld::bodies() const
    {
        return impl_ ? impl_->dynamic + impl_->held : 0;
    }

    u64 PhysicsWorld::steps() const
    {
        return impl_ ? impl_->steps : 0;
    }

    u32 PhysicsWorld::collision_chunks() const
    {
        return impl_ ? static_cast<u32>(impl_->chunks.size()) : 0;
    }

    u32 PhysicsWorld::ship_spaces() const
    {
        return impl_ ? static_cast<u32>(impl_->spaces.size() - 1) : 0;
    }

    bool PhysicsWorld::character_aboard() const
    {
        return impl_ && impl_->character && !impl_->character_space->world();
    }

    void PhysicsWorld::collision_triangles(
        function<void(ShapeKind kind, span<f64vec3 const> triangles)> const& visit) const
    {
        if (!impl_)
        {
            return;
        }

        vector<f64vec3> points;
        for (std::unique_ptr<Space> const& space : impl_->spaces)
        {
            JPH::BodyIDVector ids;
            space->system->GetBodies(ids);

            JPH::BodyLockInterfaceNoLock const& locks = space->system->GetBodyLockInterfaceNoLock();
            for (JPH::BodyID const id : ids)
            {
                JPH::BodyLockRead lock{locks, id};
                if (!lock.Succeeded())
                {
                    continue;
                }
                JPH::Body const& body = lock.GetBody();

                ShapeKind const kind = body.IsStatic()           ? (body.GetUserData() == kStaticProp ? ShapeKind::Static
                                                                                                      : ShapeKind::Ground)
                                       : !body.IsInBroadPhase() ? ShapeKind::Held
                                       : body.IsActive()        ? ShapeKind::Awake
                                                                : ShapeKind::Asleep;

                // Triangulated relative to the centre of mass, in f32, then
                // placed in f64: a chunk's corner is millions of metres out,
                // and a ship's frame is carried into the world. Only leaf
                // shapes triangulate, so a compound, such as the character's
                // offset inner capsule, is taken apart first.
                JPH::RVec3 const centre = body.GetCenterOfMassPosition();
                points.clear();

                LeafShapes leaves;
                body.GetTransformedShape().CollectTransformedShapes(JPH::AABox::sBiggest(), leaves);
                for (JPH::TransformedShape const& leaf : leaves.shapes)
                {
                    JPH::Shape::GetTrianglesContext context;
                    leaf.GetTrianglesStart(context, JPH::AABox::sBiggest(), centre);
                    constexpr int                  kBatch = 256;
                    array<JPH::Float3, 3 * kBatch> batch{};
                    for (;;)
                    {
                        int const count = leaf.GetTrianglesNext(context, kBatch, batch.data());
                        if (count <= 0)
                        {
                            break;
                        }
                        for (size_t index = 0; index < static_cast<size_t>(3 * count); ++index)
                        {
                            f64vec3 const local = from_jolt(centre) + f64vec3{batch[index].x, batch[index].y,
                                                                              batch[index].z};
                            points.push_back(space->to_world(local));
                        }
                    }
                }
                visit(kind, points);
            }
        }
    }

    u32 PhysicsWorld::awake() const
    {
        return impl_ ? impl_->awake_bodies() : 0;
    }
}
