#pragma once

#include "Physics3DTypes.h"
#include "SceneQueries.h"
#include "JoltUtils.h"
#include "JoltLayerInterface.h"
#include "JoltContactListener.h"
#include "JoltBody.h"
#include "JoltCharacterController.h"
#include "EntityExclusionUtils.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Scene/Entity.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/Reference.h>
#include "JoltJobSystemAdapter.h"
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyType.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace JPH
{
    class VehicleConstraint;      // Forward declaration — full type only needed in JoltScene.cpp
    class SoftBodySharedSettings; // Cloth soft body — full type only needed in JoltScene.cpp
} // namespace JPH

namespace OloEngine
{

    class Scene; // Forward declaration

    class JoltScene : public SceneQueries
    {
      public:
        JoltScene(Scene* scene);
        ~JoltScene();

        // Initialization
        void Initialize();
        void Shutdown();
        bool IsInitialized() const
        {
            return m_Initialized;
        }

        // Simulation
        void Simulate(f32 deltaTime);
        void Step(f32 fixedTimeStep);

        // Gravity
        glm::vec3 GetGravity() const;
        void SetGravity(const glm::vec3& gravity);

        // Body management
        Ref<JoltBody> CreateBody(Entity entity);
        void DestroyBody(Entity entity);
        Ref<JoltBody> GetBody(Entity entity);
        Ref<JoltBody> GetBodyByEntityID(UUID entityID);

        // Entity lookup by body ID (for character controller integration)
        Entity GetEntityByBodyID(const JPH::BodyID& bodyID);

        // Terrain collision (static height-field bodies). A terrain entity carries no
        // Rigidbody3DComponent / JoltBody wrapper, so its collision is a raw static JPH
        // body tracked separately, keyed by the owning TerrainComponent entity. The
        // caller (Scene) builds the JPH::HeightFieldShape from the terrain's CPU heights
        // and the entity transform, then hands it here. The body's UserData is the entity
        // UUID and it is registered in the BodyID→entity reverse map, so raycasts and
        // contacts resolve back to the terrain entity. CreateTerrainBody is idempotent —
        // it replaces any existing terrain body for the entity. Returns the new BodyID
        // (invalid on failure). DestroyTerrainBody removes it by entity UUID.
        JPH::BodyID CreateTerrainBody(Entity entity, const JPH::Ref<JPH::Shape>& shape,
                                      const glm::vec3& position, const glm::quat& rotation);
        void DestroyTerrainBody(UUID entityID);
        bool HasTerrainBody(UUID entityID) const
        {
            return m_TerrainBodies.contains(entityID);
        }

        // Cloth / soft-body management (issue #460). Like terrain bodies, a cloth entity
        // carries no Rigidbody3DComponent / JoltBody wrapper — its Jolt soft body is a raw
        // JPH body tracked separately, keyed by the owning ClothComponent entity. The caller
        // (Scene) builds the JPH::SoftBodySharedSettings from the cloth grid + entity
        // transform (JoltShapes::CreateClothSharedSettings), then hands it here. The body's
        // UserData is the entity UUID and it is registered in the BodyID→entity reverse map.
        // CreateClothBody is idempotent — it replaces any existing cloth body for the entity.
        // Returns the new BodyID (invalid on failure). The soft body is created on the MOVING
        // layer so it falls under gravity and collides with static/rigid geometry.
        JPH::BodyID CreateClothBody(Entity entity, const JPH::Ref<JPH::SoftBodySharedSettings>& settings,
                                    u32 iterations, f32 linearDamping, f32 pressure);
        void DestroyClothBody(UUID entityID);
        bool HasClothBody(UUID entityID) const
        {
            return m_Cloths.contains(entityID);
        }
        [[nodiscard("cloth count query result must be used")]] u32 GetClothCount() const
        {
            return static_cast<u32>(m_Cloths.size());
        }
        // Read the current world-space particle positions of a cloth soft body into
        // outWorldPositions (row-major, same order as the generating grid). GPU-free — runs
        // headless / on the dedicated server. Returns false if there is no cloth body for the
        // entity or the body lock fails; on success outWorldPositions has one entry per
        // particle. Called each frame by Scene to drive the deforming render mesh and by the
        // functional tests to assert the cloth drapes / rests on the floor.
        [[nodiscard("cloth vertex readback result must be used")]] bool GetClothVertices(UUID entityID, std::vector<glm::vec3>& outWorldPositions) const;

        // Two-body constraint (joint) management. CreateConstraint builds the
        // Jolt constraint for the PhysicsJoint3DComponent on `entity` (both
        // endpoint bodies must already exist) and sets the component's
        // m_RuntimeConstraintToken on success; it is called from the runtime-start
        // second pass and from the runtime-add hook. DestroyConstraint releases it.
        bool CreateConstraint(Entity entity);
        void DestroyConstraint(Entity entity);
        [[nodiscard("constraint count query result must be used")]] u32 GetConstraintCount() const
        {
            return static_cast<u32>(m_Constraints.size());
        }

        // (Re)build the collision filtering for joints that opted out of letting
        // their two connected bodies collide (PhysicsJoint3DComponent::
        // m_CollideConnected == false). Idempotent: each call resets bodies a
        // previous call grouped, then assigns a single shared collision group +
        // a fresh GroupFilterTable that disables exactly the authored no-collide
        // pairs. Call it after the constraint pass at runtime start and after a
        // joint is added/removed at runtime. Safe to call when no joint opts out
        // (it just clears any prior filtering).
        void ApplyJointCollisionFilters();

        // Vehicle (wheeled) management. CreateVehicle builds the Jolt
        // VehicleConstraint + WheeledVehicleController for the VehicleComponent on
        // `entity` (the chassis body must already exist), registers it as both a
        // constraint and a step listener, and sets the component's
        // m_RuntimeVehicleToken on success; it is called from the runtime-start
        // pass and the runtime-add hook. DestroyVehicle removes the step listener,
        // releases the constraint, and clears the token. Both are idempotent.
        bool CreateVehicle(Entity entity);
        void DestroyVehicle(Entity entity);
        [[nodiscard("vehicle count query result must be used")]] u32 GetVehicleCount() const
        {
            return static_cast<u32>(m_Vehicles.size());
        }

        // Ragdoll (skeleton-driven SwingTwist chain) management, issue #308 item 5.
        // CreateRagdoll expands the RagdollComponent on `entity` into a chain of
        // Rigidbody3D + collider + SwingTwist-joint components authored onto the
        // skeleton's bone entities; it does NOT touch Jolt itself, so it must run
        // before the body/constraint creation passes (see Scene::OnPhysics3DStart).
        // DestroyRagdoll removes exactly the components it generated, restoring the
        // authored scene. Both are idempotent. CreateRagdolls builds every enabled
        // ragdoll in the scene; DestroyAllRagdolls tears them all down. The plural
        // forms are driven by Scene::OnPhysics3DStart / OnPhysics3DStop.
        bool CreateRagdoll(Entity entity);
        void DestroyRagdoll(Entity entity);
        // Authoring pass (no Jolt objects created here): expand every enabled
        // RagdollComponent into per-bone Rigidbody3D + collider + SwingTwist-joint
        // components. MUST run BEFORE Initialize / the body + constraint passes.
        void CreateRagdolls();
        // Remove every generated ragdoll component (restoring the authored scene).
        void DestroyAllRagdolls();
        [[nodiscard("ragdoll count query result must be used")]] u32 GetRagdollCount() const
        {
            return static_cast<u32>(m_Ragdolls.size());
        }

        // Character controller management
        Ref<JoltCharacterController> CreateCharacterController(Entity entity, const ContactCallbackFn& contactCallback = nullptr);
        void DestroyCharacterController(Entity entity);
        Ref<JoltCharacterController> GetCharacterController(Entity entity);
        Ref<JoltCharacterController> GetCharacterControllerByEntityID(UUID entityID);

        // Layer interface access for character controllers
        // DEPRECATED: Use GetJoltSystem() instead. This method may return nullptr if not initialized.
        JPH::PhysicsSystem* GetPhysicsSystem() const
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing PhysicsSystem");
            return m_JoltSystem.get();
        }

        // Scene lifecycle
        void OnRuntimeStart();
        void OnRuntimeStop();
        void OnSimulationStart() const;
        void OnSimulationStop() const;

        // Scene queries (implementing SceneQueries interface)
        bool CastRay(const RayCastInfo& rayInfo, SceneQueryHit& outHit) override;
        bool CastShape(const ShapeCastInfo& shapeCastInfo, SceneQueryHit& outHit) override;
        bool CastBox(const BoxCastInfo& boxCastInfo, SceneQueryHit& outHit) override;
        bool CastSphere(const SphereCastInfo& sphereCastInfo, SceneQueryHit& outHit) override;
        bool CastCapsule(const CapsuleCastInfo& capsuleCastInfo, SceneQueryHit& outHit) override;
        i32 OverlapShape(const ShapeOverlapInfo& overlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 OverlapBox(const BoxOverlapInfo& boxOverlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 OverlapSphere(const SphereOverlapInfo& sphereOverlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 OverlapCapsule(const CapsuleOverlapInfo& capsuleOverlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 CastRayMultiple(const RayCastInfo& rayInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 CastShapeMultiple(const ShapeCastInfo& shapeCastInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 CastBoxMultiple(const BoxCastInfo& boxCastInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 CastSphereMultiple(const SphereCastInfo& sphereCastInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 CastCapsuleMultiple(const CapsuleCastInfo& capsuleCastInfo, SceneQueryHit* outHits, i32 maxHits) override;

        // Radial impulse
        void AddRadialImpulse(const glm::vec3& origin, f32 radius, f32 strength, EFalloffMode falloff, bool velocityChange);

        // Entity teleportation
        void Teleport(Entity entity, const glm::vec3& targetPosition, const glm::quat& targetRotation, bool force = false);

        // Transform synchronization
        void SynchronizeTransforms();

        // Contact events
        void OnContactEvent(ContactType type, UUID entityA, UUID entityB) const;

        // Jolt system access
        // NOTE: These methods require Initialize() to have been called successfully
        JPH::BodyInterface& GetBodyInterface()
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing BodyInterface");
            return m_JoltSystem->GetBodyInterface();
        }
        const JPH::BodyInterface& GetBodyInterface() const
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing BodyInterface");
            return m_JoltSystem->GetBodyInterface();
        }
        const JPH::BodyLockInterface& GetBodyLockInterface() const
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing BodyLockInterface");
            return m_JoltSystem->GetBodyLockInterface();
        }
        JPH::PhysicsSystem& GetJoltSystem()
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing PhysicsSystem");
            return *m_JoltSystem;
        }
        const JPH::PhysicsSystem& GetJoltSystem() const
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing PhysicsSystem");
            return *m_JoltSystem;
        }

        // Pointer version for null-checking (prefer reference versions above when possible)
        JPH::PhysicsSystem* GetJoltSystemPtr() const
        {
            return m_JoltSystem.get();
        }

        // Debug info
        u32 GetBodyCount() const
        {
            return m_JoltSystem ? m_JoltSystem->GetNumBodies() : 0;
        }
        u32 GetActiveBodyCount() const
        {
            return m_JoltSystem ? m_JoltSystem->GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0;
        }

        // Read-only snapshot of the entity pairs whose bodies are currently in
        // contact, deduplicated per entity pair. Empty when the contact listener
        // is absent (physics not initialized). Backs the read-only MCP
        // olo_physics_contacts diagnostics tool. Thread-safe (listener-locked).
        [[nodiscard("contact-pair snapshot must be used")]] std::vector<std::pair<UUID, UUID>> GetActiveContactPairs() const;

      private:
        void CreateRigidBodies();
        // Second pass after CreateRigidBodies(): build every authored joint's
        // Jolt constraint now that all bodies exist.
        void CreateConstraints();
        // Remove and release every tracked constraint. Must run before the
        // bodies they reference are destroyed.
        void DestroyAllConstraints();
        // Remove every tracked static terrain height-field body. Called on runtime
        // stop / shutdown while m_JoltSystem is still alive.
        void DestroyAllTerrainBodies();
        // Remove every tracked cloth soft body. Called on runtime stop / shutdown while
        // m_JoltSystem is still alive (before the bodies' shared settings are released).
        void DestroyAllClothBodies();
        // Second pass after CreateRigidBodies(): build every authored vehicle's
        // Jolt VehicleConstraint now that the chassis bodies exist.
        void CreateVehicles();
        // Remove (and unregister the step listener for) every tracked vehicle.
        // Must run before the chassis bodies they reference are destroyed.
        void DestroyAllVehicles();
        // Before each simulation step, push the authored driver input from every
        // VehicleComponent into its WheeledVehicleController (and wake the chassis
        // when there is input), so the vehicle's OnStep listener acts on it.
        void UpdateVehicleControllers();
        // After a simulation step, read back each breakable constraint's
        // accumulated impulse (force/torque = impulse / dt), and for any that
        // exceeds its authored PhysicsJoint3DComponent break threshold: remove
        // the constraint, clear the component's m_RuntimeConstraintToken, and
        // publish a JointBrokeEvent on the Scene's GameplayEventBus.
        void BreakOverstressedJoints(f32 stepDeltaTime);
        void SynchronizeBody(Ref<JoltBody> body) const;

        // Internal Jolt setup
        void InitializeJolt();
        void ShutdownJolt();

        // Scene query helpers - legacy vector-based interface (O(n) performance)
        // ⚠️  DEPRECATED: These methods have O(n) performance due to linear entity exclusion checks.
        // Migration: Use the ExcludedEntitySet overloads below for O(1) performance with repeated queries.
        // Performance Note: Each query performs std::find() over the excluded entities vector, causing
        // significant performance degradation with large exclusion lists or frequent queries.
        [[deprecated("Use ExcludedEntitySet overloads for O(1) performance - this vector-based API has O(n) lookup cost")]]
        bool PerformShapeCast(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                              f32 maxDistance, u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit& outHit);
        [[deprecated("Use ExcludedEntitySet overloads for O(1) performance - this vector-based API has O(n) lookup cost")]]
        i32 PerformShapeCastMultiple(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                                     f32 maxDistance, u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit* outHits, i32 maxHits);
        [[deprecated("Use ExcludedEntitySet overloads for O(1) performance - this vector-based API has O(n) lookup cost")]]
        i32 PerformShapeOverlap(JPH::Ref<JPH::Shape> shape, const glm::vec3& position, const glm::quat& rotation,
                                u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit* outHits, i32 maxHits);
        [[deprecated("Use ExcludedEntitySet overloads for O(1) performance - this vector-based API has O(n) lookup cost")]]
        bool IsEntityExcluded(UUID entityID, const std::vector<UUID>& excludedEntities);

        // Scene query helpers - optimized O(1) ExcludedEntitySet interface
        // ✅ PREFERRED: These methods provide O(1) entity exclusion checks for optimal performance.
        // Performance Note: Uses std::unordered_set for constant-time entity lookup during queries.
        bool PerformShapeCast(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                              f32 maxDistance, u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit& outHit) const;
        i32 PerformShapeCastMultiple(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                                     f32 maxDistance, u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit* outHits, i32 maxHits);
        i32 PerformShapeOverlap(JPH::Ref<JPH::Shape> shape, const glm::vec3& position, const glm::quat& rotation,
                                u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit* outHits, i32 maxHits);
        bool IsEntityExcluded(UUID entityID, const ExcludedEntitySet& excludedEntitySet);

        void FillHitInfo(const JPH::RayCastResult& hit, const JPH::RRayCast& ray, SceneQueryHit& outHit) const;
        void FillHitInfo(const JPH::ShapeCastResult& hit, const JPH::RShapeCast& shapeCast, SceneQueryHit& outHit) const;

      private:
        Scene* m_Scene;
        bool m_Initialized = false;

        // Jolt physics system
        std::unique_ptr<JPH::TempAllocator> m_TempAllocator;
        std::unique_ptr<JoltJobSystemAdapter> m_JobSystem;
        std::unique_ptr<JPH::PhysicsSystem> m_JoltSystem;

        // Layer interfaces
        std::unique_ptr<BroadPhaseLayerInterface> m_BroadPhaseLayerInterface;
        std::unique_ptr<ObjectVsBroadPhaseLayerFilter> m_ObjectVsBroadPhaseLayerFilter;
        std::unique_ptr<ObjectLayerPairFilter> m_ObjectLayerPairFilter;

        // Contact listener
        std::unique_ptr<JoltContactListener> m_ContactListener;

        // Body management
        std::unordered_map<UUID, Ref<JoltBody>> m_Bodies;
        std::unordered_map<JPH::BodyID, UUID> m_BodyIDToEntity; // Reverse lookup for efficient GetEntityByBodyID
        std::vector<Ref<JoltBody>> m_BodiesToSync;

        // Static terrain height-field collision bodies, keyed by the owning
        // TerrainComponent entity. These are raw JPH bodies (no JoltBody wrapper), so
        // they live outside m_Bodies; they are still entered in m_BodyIDToEntity so
        // queries resolve them. Created/destroyed by Create/DestroyTerrainBody.
        std::unordered_map<UUID, JPH::BodyID> m_TerrainBodies;

        // Cloth soft bodies (issue #460), keyed by the owning ClothComponent entity. Like
        // terrain bodies these are raw JPH bodies (no JoltBody wrapper), so they live
        // outside m_Bodies; they are still entered in m_BodyIDToEntity so queries resolve
        // them. The JPH::Ref keeps the SoftBodySharedSettings alive for the body's lifetime.
        // Created/destroyed by Create/DestroyClothBody.
        struct ClothRuntime
        {
            JPH::BodyID m_BodyID;
            JPH::Ref<JPH::SoftBodySharedSettings> m_Settings;
        };
        std::unordered_map<UUID, ClothRuntime> m_Cloths;

        // Joints (two-body constraints), keyed by the owning entity (the one
        // carrying the PhysicsJoint3DComponent). The JPH::Ref keeps the
        // constraint alive while it is registered with m_JoltSystem.
        std::unordered_map<UUID, JPH::Ref<JPH::Constraint>> m_Constraints;

        // Wheeled vehicles, keyed by the owning entity (the chassis carrying the
        // VehicleComponent). The JPH::Ref keeps the VehicleConstraint alive while
        // it is registered with m_JoltSystem; it owns its WheeledVehicleController
        // and VehicleCollisionTester. The constraint is ALSO registered as a step
        // listener (raw pointer, not ref-counted), so DestroyVehicle must
        // RemoveStepListener before releasing the ref.
        std::unordered_map<UUID, JPH::Ref<JPH::VehicleConstraint>> m_Vehicles;

        // Ragdolls (issue #308 item 5), keyed by the owning entity (the one
        // carrying the RagdollComponent). A ragdoll holds no Jolt object of its
        // own — it authors Rigidbody3D + collider + SwingTwist-joint components
        // onto the skeleton's bone entities, which the normal body/constraint
        // passes then realise. We only track which components we generated on
        // which bone entities so DestroyRagdoll can remove exactly those (and
        // their Jolt body/constraint, via the OnComponentRemoved hooks) on stop,
        // leaving any pre-authored bone bodies/joints untouched.
        struct RagdollRuntime
        {
            std::vector<UUID> m_GeneratedBodyEntities;     // got a generated Rigidbody3DComponent
            std::vector<UUID> m_GeneratedColliderEntities; // got a generated SphereCollider3DComponent
            std::vector<UUID> m_GeneratedJointEntities;    // got a generated PhysicsJoint3DComponent
        };
        std::unordered_map<UUID, RagdollRuntime> m_Ragdolls;

        // Collision filtering for joints whose m_CollideConnected == false (see
        // ApplyJointCollisionFilters). m_JointGroupFilter is the single shared
        // GroupFilterTable referenced by every filtered body's CollisionGroup;
        // holding a Ref here keeps it alive and makes its lifetime explicit.
        // m_JointCollisionBodies is the set of body-owning entities that were
        // assigned that group, so the next rebuild can reset them first.
        JPH::Ref<JPH::GroupFilterTable> m_JointGroupFilter;
        std::unordered_set<UUID> m_JointCollisionBodies;

        // Character controller management
        std::unordered_map<UUID, Ref<JoltCharacterController>> m_CharacterControllers;
        std::vector<Ref<JoltCharacterController>> m_CharacterControllersToUpdate;

        // Simulation settings
        f32 m_FixedTimeStep = 1.0f / 60.0f;
        f32 m_Accumulator = 0.0f;
        i32 m_CollisionSteps = 1;
        i32 m_IntegrationSubSteps = 1;

        // Constants
        // CollisionGroup group id reserved for joint "collide connected" filtering.
        // The engine uses CollisionGroup for nothing else, so every filtered body
        // shares this group id and a unique sub-group id (see ApplyJointCollisionFilters).
        static constexpr u32 s_JointCollisionGroupID = 0;
        static constexpr u32 s_MaxBodies = 65536;
        static constexpr u32 s_NumBodyMutexes = 0; // Autodetect
        static constexpr u32 s_MaxBodyPairs = 65536;
        static constexpr u32 s_MaxStepsPerFrame = 10; // Prevent "spiral of death" in fixed timestep
        static constexpr u32 s_MaxContactConstraints = 10240;
        static constexpr u32 s_TempAllocatorSize = 10 * 1024 * 1024; // 10 MB
        static constexpr u32 s_JobSystemMaxJobs = 2048;
        static constexpr u32 s_JobSystemMaxBarriers = 8;
    };

} // namespace OloEngine
