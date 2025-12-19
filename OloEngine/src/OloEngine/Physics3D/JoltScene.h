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
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyType.h>

#include <memory>
#include <unordered_map>
#include <vector>

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
        void OnSimulationStart();
        void OnSimulationStop();

        // Scene queries (implementing SceneQueries interface)
        virtual bool CastRay(const RayCastInfo& rayInfo, SceneQueryHit& outHit) override;
        virtual bool CastShape(const ShapeCastInfo& shapeCastInfo, SceneQueryHit& outHit) override;
        virtual bool CastBox(const BoxCastInfo& boxCastInfo, SceneQueryHit& outHit) override;
        virtual bool CastSphere(const SphereCastInfo& sphereCastInfo, SceneQueryHit& outHit) override;
        virtual bool CastCapsule(const CapsuleCastInfo& capsuleCastInfo, SceneQueryHit& outHit) override;
        virtual i32 OverlapShape(const ShapeOverlapInfo& overlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        virtual i32 OverlapBox(const BoxOverlapInfo& boxOverlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        virtual i32 OverlapSphere(const SphereOverlapInfo& sphereOverlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        virtual i32 OverlapCapsule(const CapsuleOverlapInfo& capsuleOverlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        virtual i32 CastRayMultiple(const RayCastInfo& rayInfo, SceneQueryHit* outHits, i32 maxHits) override;
        virtual i32 CastShapeMultiple(const ShapeCastInfo& shapeCastInfo, SceneQueryHit* outHits, i32 maxHits) override;
        virtual i32 CastBoxMultiple(const BoxCastInfo& boxCastInfo, SceneQueryHit* outHits, i32 maxHits) override;
        virtual i32 CastSphereMultiple(const SphereCastInfo& sphereCastInfo, SceneQueryHit* outHits, i32 maxHits) override;
        virtual i32 CastCapsuleMultiple(const CapsuleCastInfo& capsuleCastInfo, SceneQueryHit* outHits, i32 maxHits) override;

        // Radial impulse
        void AddRadialImpulse(const glm::vec3& origin, f32 radius, f32 strength, EFalloffMode falloff, bool velocityChange);

        // Entity teleportation
        void Teleport(Entity entity, const glm::vec3& targetPosition, const glm::quat& targetRotation, bool force = false);

        // Transform synchronization
        void SynchronizeTransforms();

        // Contact events
        void OnContactEvent(ContactType type, UUID entityA, UUID entityB);

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

      private:
        void CreateRigidBodies();
        void SynchronizeBody(Ref<JoltBody> body);

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
                              f32 maxDistance, u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit& outHit);
        i32 PerformShapeCastMultiple(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                                     f32 maxDistance, u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit* outHits, i32 maxHits);
        i32 PerformShapeOverlap(JPH::Ref<JPH::Shape> shape, const glm::vec3& position, const glm::quat& rotation,
                                u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit* outHits, i32 maxHits);
        bool IsEntityExcluded(UUID entityID, const ExcludedEntitySet& excludedEntitySet);

        void FillHitInfo(const JPH::RayCastResult& hit, const JPH::RRayCast& ray, SceneQueryHit& outHit);
        void FillHitInfo(const JPH::ShapeCastResult& hit, const JPH::RShapeCast& shapeCast, SceneQueryHit& outHit);

      private:
        Scene* m_Scene;
        bool m_Initialized = false;

        // Jolt physics system
        std::unique_ptr<JPH::TempAllocator> m_TempAllocator;
        std::unique_ptr<JPH::JobSystemThreadPool> m_JobSystem;
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

        // Character controller management
        std::unordered_map<UUID, Ref<JoltCharacterController>> m_CharacterControllers;
        std::vector<Ref<JoltCharacterController>> m_CharacterControllersToUpdate;

        // Simulation settings
        f32 m_FixedTimeStep = 1.0f / 60.0f;
        f32 m_Accumulator = 0.0f;
        i32 m_CollisionSteps = 1;
        i32 m_IntegrationSubSteps = 1;

        // Constants
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
