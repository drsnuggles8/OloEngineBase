#include "OloEnginePCH.h"
#include "JoltScene.h"
#include "EntityExclusionBodyFilter.h"
#include "SceneQueries.h"
#include "JoltShapes.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>

#include <glm/gtc/constants.hpp>

namespace OloEngine
{
    namespace
    {
        // JPH::Factory and the RTTI registered by JPH::RegisterTypes() are
        // process-wide singletons. Each JoltScene owns its own PhysicsSystem
        // but they all share these globals, so re-creating them on every
        // JoltScene::Initialize() leaked the previous Factory plus its
        // string_view/u32 RTTI hash tables whenever two JoltScenes coexisted
        // (caught by LSan via MultipleScenesCoexistTest). Ref-count instead
        // so global state is allocated on the first scene and torn down
        // exactly once when the last scene shuts down.
        std::atomic<i32> s_JoltGlobalRefCount{ 0 };
    } // namespace

    JoltScene::JoltScene(Scene* scene)
        : m_Scene(scene)
    {
        OLO_CORE_ASSERT(scene, "JoltScene requires a valid Scene");
    }

    JoltScene::~JoltScene()
    {
        Shutdown();
    }

    void JoltScene::Initialize()
    {
        if (m_Initialized)
            return;

        OLO_CORE_INFO("Initializing JoltScene");

        InitializeJolt();
        JoltShapes::Initialize();

        m_Initialized = true;
        OLO_CORE_INFO("JoltScene initialized successfully");
    }

    void JoltScene::Shutdown()
    {
        if (!m_Initialized)
            return;

        OLO_CORE_INFO("Shutting down JoltScene");

        // Remove constraints before the bodies they reference are destroyed.
        DestroyAllConstraints();

        // Destroy all bodies
        for (const auto& [entityID, body] : m_Bodies)
        {
            // Body destructor will handle Jolt cleanup
        }
        m_Bodies.clear();
        m_BodyIDToEntity.clear(); // Clear reverse lookup map
        m_BodiesToSync.clear();

        // Destroy all character controllers
        for (const auto& [entityID, characterController] : m_CharacterControllers)
        {
            // Character controller destructor will handle Jolt cleanup
        }
        m_CharacterControllers.clear();
        m_CharacterControllersToUpdate.clear();

        JoltShapes::Shutdown();
        ShutdownJolt();

        m_Initialized = false;
        OLO_CORE_INFO("JoltScene shut down successfully");
    }

    void JoltScene::Simulate(f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized || !m_JoltSystem)
            return;

        // Process contact events
        if (m_ContactListener)
        {
            m_ContactListener->ProcessContactEvents();
        }

        // Fixed timestep simulation with accumulator
        m_Accumulator += deltaTime;

        // Prevent "spiral of death" by capping simulation steps per frame
        u32 stepsExecuted = 0;

        // If accumulator exceeds maximum, clamp it and log the skip
        if (f32 maxAccumulator = static_cast<f32>(s_MaxStepsPerFrame) * m_FixedTimeStep; m_Accumulator > maxAccumulator)
        {
            f32 skippedTime = m_Accumulator - maxAccumulator;
            m_Accumulator = maxAccumulator;
            OLO_CORE_WARN("Physics timestep accumulator clamped! Skipped {0} seconds to prevent spiral of death", skippedTime);
        }

        while (m_Accumulator >= m_FixedTimeStep && stepsExecuted < s_MaxStepsPerFrame)
        {
            Step(m_FixedTimeStep);
            m_Accumulator -= m_FixedTimeStep;
            ++stepsExecuted;
        }

        // Synchronize transforms after simulation
        SynchronizeTransforms();
    }

    void JoltScene::Step(f32 fixedTimeStep)
    {
        if (!m_JoltSystem)
            return;

        // Pre-simulate character controllers
        for (auto& characterController : m_CharacterControllersToUpdate)
        {
            characterController->PreSimulate(fixedTimeStep);
        }

        // Simulate character controllers
        for (auto& characterController : m_CharacterControllersToUpdate)
        {
            characterController->Simulate(fixedTimeStep);
        }

        // Step the physics simulation
        JPH::EPhysicsUpdateError error = m_JoltSystem->Update(
            fixedTimeStep,
            m_CollisionSteps,
            m_TempAllocator.get(),
            m_JobSystem.get());

        // Drain any scheduler workers still finishing Job::Release()→FreeJob() before we
        // touch physics state or start the next Step (whose CreateJob reuses the same job
        // free-list). Jolt's barrier only waited on Job::Execute() returning, not on our
        // lambda's trailing Release(). Without this the worker can corrupt the free-list
        // concurrently — the #281 SEH 0xc0000005 / use-after-free. Normally a no-op.
        if (m_JobSystem)
            m_JobSystem->WaitForOutstandingTasks();

        if (error != JPH::EPhysicsUpdateError::None)
        {
            OLO_CORE_ERROR("Jolt physics update error: {0}", static_cast<i32>(error));
        }

        // Post-simulate character controllers
        for (auto& characterController : m_CharacterControllersToUpdate)
        {
            characterController->PostSimulate();
        }
    }

    glm::vec3 JoltScene::GetGravity() const
    {
        if (!m_JoltSystem)
            return glm::vec3(0.0f, -9.81f, 0.0f);

        return JoltUtils::FromJoltVector(m_JoltSystem->GetGravity());
    }

    void JoltScene::SetGravity(const glm::vec3& gravity)
    {
        if (!m_JoltSystem)
            return;

        m_JoltSystem->SetGravity(JoltUtils::ToJoltVector(gravity));
    }

    Ref<JoltBody> JoltScene::CreateBody(Entity entity)
    {
        if (!entity || !entity.HasComponent<Rigidbody3DComponent>())
        {
            OLO_CORE_ERROR("Cannot create physics body for entity without Rigidbody3DComponent");
            return nullptr;
        }

        UUID entityID = entity.GetUUID();

        // Check if body already exists
        if (m_Bodies.find(entityID) != m_Bodies.end())
        {
            OLO_CORE_WARN("Physics body already exists for entity {0}", (u64)entityID);
            return m_Bodies[entityID];
        }

        // Create new body
        Ref<JoltBody> body = Ref<JoltBody>(new JoltBody(entity, this));
        if (!body->IsValid())
        {
            OLO_CORE_ERROR("Failed to create Jolt body for entity {0}", (u64)entityID);
            return nullptr;
        }

        m_Bodies[entityID] = body;

        // Add to reverse lookup map for efficient GetEntityByBodyID
        m_BodyIDToEntity[body->GetBodyID()] = entityID;

        OLO_CORE_TRACE("Created physics body for entity {0}", (u64)entityID);
        return body;
    }

    void JoltScene::DestroyBody(Entity entity)
    {
        if (!entity)
            return;

        UUID entityID = entity.GetUUID();
        auto it = m_Bodies.find(entityID);
        if (it != m_Bodies.end())
        {
            // Remove from reverse lookup map
            if (it->second && it->second->IsValid())
            {
                m_BodyIDToEntity.erase(it->second->GetBodyID());
            }

            // Remove from sync list
            if (auto syncIt = std::ranges::find(m_BodiesToSync, it->second); syncIt != m_BodiesToSync.end())
            {
                m_BodiesToSync.erase(syncIt);
            }

            m_Bodies.erase(it);
            OLO_CORE_TRACE("Destroyed physics body for entity {0}", (u64)entityID);
        }
    }

    Ref<JoltBody> JoltScene::GetBody(Entity entity)
    {
        if (!entity)
            return nullptr;

        return GetBodyByEntityID(entity.GetUUID());
    }

    Ref<JoltBody> JoltScene::GetBodyByEntityID(UUID entityID)
    {
        auto it = m_Bodies.find(entityID);
        return (it != m_Bodies.end()) ? it->second : nullptr;
    }

    Entity JoltScene::GetEntityByBodyID(const JPH::BodyID& bodyID)
    {
        // Use efficient reverse lookup instead of O(n) linear search
        if (auto it = m_BodyIDToEntity.find(bodyID); it != m_BodyIDToEntity.end())
        {
            // Found the entity UUID, get Entity from scene
            return m_Scene->GetEntityByUUID(it->second);
        }

        // Return invalid entity if not found
        return Entity{};
    }

    Ref<JoltCharacterController> JoltScene::CreateCharacterController(Entity entity, const ContactCallbackFn& contactCallback)
    {
        if (!entity || !m_Initialized)
            return nullptr;

        UUID entityID = entity.GetUUID();

        // Check if character controller already exists
        if (auto it = m_CharacterControllers.find(entityID); it != m_CharacterControllers.end())
        {
            OLO_CORE_WARN("Character controller already exists for entity {0}", (u64)entityID);
            return it->second;
        }

        // Create character controller
        auto characterController = Ref<JoltCharacterController>(new JoltCharacterController(entity, this, contactCallback));

        // Store it
        m_CharacterControllers[entityID] = characterController;
        m_CharacterControllersToUpdate.push_back(characterController);

        OLO_CORE_TRACE("Created character controller for entity {0}", (u64)entityID);
        return characterController;
    }

    void JoltScene::DestroyCharacterController(Entity entity)
    {
        if (!entity)
            return;

        UUID entityID = entity.GetUUID();
        auto it = m_CharacterControllers.find(entityID);
        if (it != m_CharacterControllers.end())
        {
            // Remove from update list
            if (auto updateIt = std::ranges::find(m_CharacterControllersToUpdate, it->second); updateIt != m_CharacterControllersToUpdate.end())
            {
                m_CharacterControllersToUpdate.erase(updateIt);
            }

            m_CharacterControllers.erase(it);
            OLO_CORE_TRACE("Destroyed character controller for entity {0}", (u64)entityID);
        }
    }

    Ref<JoltCharacterController> JoltScene::GetCharacterController(Entity entity)
    {
        if (!entity)
            return nullptr;

        return GetCharacterControllerByEntityID(entity.GetUUID());
    }

    Ref<JoltCharacterController> JoltScene::GetCharacterControllerByEntityID(UUID entityID)
    {
        auto it = m_CharacterControllers.find(entityID);
        return (it != m_CharacterControllers.end()) ? it->second : nullptr;
    }

    void JoltScene::OnRuntimeStart()
    {
        OLO_CORE_INFO("JoltScene starting runtime");

        if (!m_Initialized)
        {
            Initialize();
        }

        CreateRigidBodies();
        // Second pass: all bodies exist now, so joints can resolve both endpoints.
        CreateConstraints();
    }

    void JoltScene::OnRuntimeStop()
    {
        OLO_CORE_INFO("JoltScene stopping runtime");

        // Remove constraints before the bodies they reference are destroyed.
        DestroyAllConstraints();

        // Destroy all bodies
        for (const auto& [entityID, body] : m_Bodies)
        {
            // Body destructor will handle cleanup
        }
        m_Bodies.clear();
        m_BodyIDToEntity.clear(); // Clear reverse lookup map
        m_BodiesToSync.clear();
    }

    void JoltScene::OnSimulationStart() const
    {
        OLO_CORE_INFO("JoltScene starting simulation");
        // Additional simulation-specific setup can go here
    }

    void JoltScene::OnSimulationStop() const
    {
        OLO_CORE_INFO("JoltScene stopping simulation");
        // Additional simulation-specific cleanup can go here
    }

    bool JoltScene::CastRay(const RayCastInfo& rayInfo, SceneQueryHit& outHit)
    {
        if (!m_JoltSystem)
            return false;

        // Clear the output hit info
        outHit.Clear();

        // Create ray
        JPH::RRayCast ray;
        ray.mOrigin = JoltUtils::ToJoltVector(rayInfo.m_Origin);
        ray.mDirection = JoltUtils::ToJoltVector(glm::normalize(rayInfo.m_Direction)) * rayInfo.m_MaxDistance;

        // Perform ray cast
        JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> hitCollector;
        JPH::RayCastSettings rayCastSettings;

        // Create filters
        JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*m_ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayer(rayInfo.m_LayerMask));
        JPH::DefaultObjectLayerFilter objectLayerFilter(*m_ObjectLayerPairFilter, JPH::ObjectLayer(rayInfo.m_LayerMask));
        EntityExclusionBodyFilter bodyFilter(rayInfo.m_ExcludedEntities);

        m_JoltSystem->GetNarrowPhaseQuery().CastRay(ray, rayCastSettings, hitCollector, broadPhaseFilter, objectLayerFilter, bodyFilter);

        if (!hitCollector.HadHit())
            return false;

        // Fill hit information
        FillHitInfo(hitCollector.mHit, ray, outHit);
        return true;
    }

    bool JoltScene::CastShape(const ShapeCastInfo& shapeCastInfo, SceneQueryHit& outHit)
    {
        switch (shapeCastInfo.GetCastType())
        {
            case ShapeCastType::Box:
            {
#ifdef OLO_DEBUG
                // Debug-only runtime type verification to catch mismatches early
                OLO_CORE_ASSERT(shapeCastInfo.GetCastType() == ShapeCastType::Box,
                                "ShapeCastInfo type mismatch: expected Box but got different type");
#endif
                const BoxCastInfo& boxInfo = static_cast<const BoxCastInfo&>(shapeCastInfo);
                return CastBox(boxInfo, outHit);
            }
            case ShapeCastType::Sphere:
            {
#ifdef OLO_DEBUG
                // Debug-only runtime type verification to catch mismatches early
                OLO_CORE_ASSERT(shapeCastInfo.GetCastType() == ShapeCastType::Sphere,
                                "ShapeCastInfo type mismatch: expected Sphere but got different type");
#endif
                const SphereCastInfo& sphereInfo = static_cast<const SphereCastInfo&>(shapeCastInfo);
                return CastSphere(sphereInfo, outHit);
            }
            case ShapeCastType::Capsule:
            {
#ifdef OLO_DEBUG
                // Debug-only runtime type verification to catch mismatches early
                OLO_CORE_ASSERT(shapeCastInfo.GetCastType() == ShapeCastType::Capsule,
                                "ShapeCastInfo type mismatch: expected Capsule but got different type");
#endif
                const CapsuleCastInfo& capsuleInfo = static_cast<const CapsuleCastInfo&>(shapeCastInfo);
                return CastCapsule(capsuleInfo, outHit);
            }
            default:
                OLO_CORE_ERROR("Unsupported shape cast type: {0}", static_cast<int>(shapeCastInfo.GetCastType()));
                return false;
        }
    }

    bool JoltScene::CastBox(const BoxCastInfo& boxCastInfo, SceneQueryHit& outHit)
    {
        if (!m_JoltSystem)
            return false;

        JPH::Ref<JPH::Shape> boxShape = new JPH::BoxShape(JoltUtils::ToJoltVector(boxCastInfo.m_HalfExtent));
        ExcludedEntitySet exclusionSet(boxCastInfo.m_ExcludedEntities);
        return PerformShapeCast(boxShape, boxCastInfo.m_Origin, boxCastInfo.m_Direction,
                                boxCastInfo.m_MaxDistance, boxCastInfo.m_LayerMask, exclusionSet, outHit);
    }

    bool JoltScene::CastSphere(const SphereCastInfo& sphereCastInfo, SceneQueryHit& outHit)
    {
        if (!m_JoltSystem)
            return false;

        JPH::Ref<JPH::Shape> sphereShape = new JPH::SphereShape(sphereCastInfo.m_Radius);
        ExcludedEntitySet exclusionSet(sphereCastInfo.m_ExcludedEntities);
        return PerformShapeCast(sphereShape, sphereCastInfo.m_Origin, sphereCastInfo.m_Direction,
                                sphereCastInfo.m_MaxDistance, sphereCastInfo.m_LayerMask, exclusionSet, outHit);
    }

    bool JoltScene::CastCapsule(const CapsuleCastInfo& capsuleCastInfo, SceneQueryHit& outHit)
    {
        if (!m_JoltSystem)
            return false;

        JPH::Ref<JPH::Shape> capsuleShape = new JPH::CapsuleShape(capsuleCastInfo.m_HalfHeight, capsuleCastInfo.m_Radius);
        ExcludedEntitySet exclusionSet(capsuleCastInfo.m_ExcludedEntities);
        return PerformShapeCast(capsuleShape, capsuleCastInfo.m_Origin, capsuleCastInfo.m_Direction,
                                capsuleCastInfo.m_MaxDistance, capsuleCastInfo.m_LayerMask, exclusionSet, outHit);
    }

    i32 JoltScene::CastBoxMultiple(const BoxCastInfo& boxCastInfo, SceneQueryHit* outHits, i32 maxHits)
    {
        if (!m_JoltSystem)
            return 0;

        JPH::Ref<JPH::Shape> boxShape = new JPH::BoxShape(JoltUtils::ToJoltVector(boxCastInfo.m_HalfExtent));
        ExcludedEntitySet exclusionSet(boxCastInfo.m_ExcludedEntities);
        return PerformShapeCastMultiple(boxShape, boxCastInfo.m_Origin, boxCastInfo.m_Direction,
                                        boxCastInfo.m_MaxDistance, boxCastInfo.m_LayerMask, exclusionSet, outHits, maxHits);
    }

    i32 JoltScene::CastSphereMultiple(const SphereCastInfo& sphereCastInfo, SceneQueryHit* outHits, i32 maxHits)
    {
        if (!m_JoltSystem)
            return 0;

        JPH::Ref<JPH::Shape> sphereShape = new JPH::SphereShape(sphereCastInfo.m_Radius);
        ExcludedEntitySet exclusionSet(sphereCastInfo.m_ExcludedEntities);
        return PerformShapeCastMultiple(sphereShape, sphereCastInfo.m_Origin, sphereCastInfo.m_Direction,
                                        sphereCastInfo.m_MaxDistance, sphereCastInfo.m_LayerMask, exclusionSet, outHits, maxHits);
    }

    i32 JoltScene::CastCapsuleMultiple(const CapsuleCastInfo& capsuleCastInfo, SceneQueryHit* outHits, i32 maxHits)
    {
        if (!m_JoltSystem)
            return 0;

        JPH::Ref<JPH::Shape> capsuleShape = new JPH::CapsuleShape(capsuleCastInfo.m_HalfHeight, capsuleCastInfo.m_Radius);
        ExcludedEntitySet exclusionSet(capsuleCastInfo.m_ExcludedEntities);
        return PerformShapeCastMultiple(capsuleShape, capsuleCastInfo.m_Origin, capsuleCastInfo.m_Direction,
                                        capsuleCastInfo.m_MaxDistance, capsuleCastInfo.m_LayerMask, exclusionSet, outHits, maxHits);
    }

    i32 JoltScene::OverlapShape(const ShapeOverlapInfo& overlapInfo, SceneQueryHit* outHits, i32 maxHits)
    {
        switch (overlapInfo.GetCastType())
        {
            case ShapeCastType::Box:
            {
#ifdef OLO_DEBUG
                // Debug-only runtime type verification to catch mismatches early
                OLO_CORE_ASSERT(overlapInfo.GetCastType() == ShapeCastType::Box,
                                "ShapeOverlapInfo type mismatch: expected Box but got different type");
#endif
                const BoxOverlapInfo& boxInfo = static_cast<const BoxOverlapInfo&>(overlapInfo);
                return OverlapBox(boxInfo, outHits, maxHits);
            }
            case ShapeCastType::Sphere:
            {
#ifdef OLO_DEBUG
                // Debug-only runtime type verification to catch mismatches early
                OLO_CORE_ASSERT(overlapInfo.GetCastType() == ShapeCastType::Sphere,
                                "ShapeOverlapInfo type mismatch: expected Sphere but got different type");
#endif
                const SphereOverlapInfo& sphereInfo = static_cast<const SphereOverlapInfo&>(overlapInfo);
                return OverlapSphere(sphereInfo, outHits, maxHits);
            }
            case ShapeCastType::Capsule:
            {
#ifdef OLO_DEBUG
                // Debug-only runtime type verification to catch mismatches early
                OLO_CORE_ASSERT(overlapInfo.GetCastType() == ShapeCastType::Capsule,
                                "ShapeOverlapInfo type mismatch: expected Capsule but got different type");
#endif
                const CapsuleOverlapInfo& capsuleInfo = static_cast<const CapsuleOverlapInfo&>(overlapInfo);
                return OverlapCapsule(capsuleInfo, outHits, maxHits);
            }
            default:
                OLO_CORE_ERROR("Unsupported shape overlap type");
                return 0;
        }
    }

    i32 JoltScene::OverlapBox(const BoxOverlapInfo& boxOverlapInfo, SceneQueryHit* outHits, i32 maxHits)
    {
        if (!m_JoltSystem)
            return 0;

        JPH::Ref<JPH::Shape> boxShape = new JPH::BoxShape(JoltUtils::ToJoltVector(boxOverlapInfo.m_HalfExtent));
        ExcludedEntitySet exclusionSet(boxOverlapInfo.m_ExcludedEntities);
        return PerformShapeOverlap(boxShape, boxOverlapInfo.m_Origin, boxOverlapInfo.m_Rotation,
                                   boxOverlapInfo.m_LayerMask, exclusionSet, outHits, maxHits);
    }

    i32 JoltScene::OverlapSphere(const SphereOverlapInfo& sphereOverlapInfo, SceneQueryHit* outHits, i32 maxHits)
    {
        if (!m_JoltSystem)
            return 0;

        JPH::Ref<JPH::Shape> sphereShape = new JPH::SphereShape(sphereOverlapInfo.m_Radius);
        ExcludedEntitySet exclusionSet(sphereOverlapInfo.m_ExcludedEntities);
        return PerformShapeOverlap(sphereShape, sphereOverlapInfo.m_Origin, sphereOverlapInfo.m_Rotation,
                                   sphereOverlapInfo.m_LayerMask, exclusionSet, outHits, maxHits);
    }

    i32 JoltScene::OverlapCapsule(const CapsuleOverlapInfo& capsuleOverlapInfo, SceneQueryHit* outHits, i32 maxHits)
    {
        if (!m_JoltSystem)
            return 0;

        JPH::Ref<JPH::Shape> capsuleShape = new JPH::CapsuleShape(capsuleOverlapInfo.m_HalfHeight, capsuleOverlapInfo.m_Radius);
        ExcludedEntitySet exclusionSet(capsuleOverlapInfo.m_ExcludedEntities);
        return PerformShapeOverlap(capsuleShape, capsuleOverlapInfo.m_Origin, capsuleOverlapInfo.m_Rotation,
                                   capsuleOverlapInfo.m_LayerMask, exclusionSet, outHits, maxHits);
    }

    i32 JoltScene::CastRayMultiple(const RayCastInfo& rayInfo, SceneQueryHit* outHits, i32 maxHits)
    {
        if (!m_JoltSystem || maxHits <= 0)
            return 0;

        // Create ray
        JPH::RRayCast ray;
        ray.mOrigin = JoltUtils::ToJoltVector(rayInfo.m_Origin);
        ray.mDirection = JoltUtils::ToJoltVector(glm::normalize(rayInfo.m_Direction)) * rayInfo.m_MaxDistance;

        // Perform ray cast with multiple hit collector
        JPH::AllHitCollisionCollector<JPH::CastRayCollector> hitCollector;
        JPH::RayCastSettings rayCastSettings;

        // Create filters
        JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*m_ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayer(rayInfo.m_LayerMask));
        JPH::DefaultObjectLayerFilter objectLayerFilter(*m_ObjectLayerPairFilter, JPH::ObjectLayer(rayInfo.m_LayerMask));
        EntityExclusionBodyFilter bodyFilter(rayInfo.m_ExcludedEntities);

        m_JoltSystem->GetNarrowPhaseQuery().CastRay(ray, rayCastSettings, hitCollector, broadPhaseFilter, objectLayerFilter, bodyFilter);

        // Fill hit results
        i32 hitCount = 0;
        for (const auto& hit : hitCollector.mHits)
        {
            if (hitCount >= maxHits)
                break;

            FillHitInfo(hit, ray, outHits[hitCount]);
            ++hitCount;
        }

        return hitCount;
    }

    i32 JoltScene::CastShapeMultiple(const ShapeCastInfo& shapeCastInfo, SceneQueryHit* outHits, i32 maxHits)
    {
        if (maxHits <= 0)
            return 0;

        switch (shapeCastInfo.GetCastType())
        {
            case ShapeCastType::Box:
            {
                const BoxCastInfo& boxInfo = static_cast<const BoxCastInfo&>(shapeCastInfo);
                return CastBoxMultiple(boxInfo, outHits, maxHits);
            }
            case ShapeCastType::Sphere:
            {
                const SphereCastInfo& sphereInfo = static_cast<const SphereCastInfo&>(shapeCastInfo);
                return CastSphereMultiple(sphereInfo, outHits, maxHits);
            }
            case ShapeCastType::Capsule:
            {
                const CapsuleCastInfo& capsuleInfo = static_cast<const CapsuleCastInfo&>(shapeCastInfo);
                return CastCapsuleMultiple(capsuleInfo, outHits, maxHits);
            }
            default:
                OLO_CORE_ERROR("Unsupported shape cast type");
                return 0;
        }
    }

    void JoltScene::AddRadialImpulse(const glm::vec3& origin, f32 radius, f32 strength, EFalloffMode falloff, bool velocityChange)
    {
        // Apply radial impulse to all dynamic bodies within radius
        for (auto& [entityID, body] : m_Bodies)
        {
            if (body && body->IsDynamic())
            {
                body->AddRadialImpulse(origin, radius, strength, falloff, velocityChange);
            }
        }
    }

    void JoltScene::Teleport(Entity entity, const glm::vec3& targetPosition, const glm::quat& targetRotation, bool force)
    {
        auto body = GetBody(entity);
        if (body)
        {
            body->SetTransform(targetPosition, targetRotation);
            if (force)
            {
                body->Activate();
            }
        }
    }

    void JoltScene::SynchronizeTransforms()
    {
        // Synchronize transforms for all bodies that need it
        for (const auto& body : m_BodiesToSync)
        {
            if (body)
            {
                SynchronizeBody(body);
            }
        }
        m_BodiesToSync.clear();

        // Also synchronize all dynamic and kinematic bodies
        for (auto& [entityID, body] : m_Bodies)
        {
            if (body && (body->IsDynamic() || body->IsKinematic()) && body->IsActive())
            {
                SynchronizeBody(body);
            }
        }
    }

    void JoltScene::OnContactEvent(ContactType type, UUID entityA, UUID entityB) const
    {
        // Forward contact events to the scene or specific entities
        // This can be extended to trigger script callbacks, audio events, etc.

        switch (type)
        {
            case ContactType::ContactAdded:
                OLO_CORE_TRACE("Contact added between entities {0} and {1}", (u64)entityA, (u64)entityB);
                break;
            case ContactType::ContactPersisted:
                // Usually too verbose to log
                break;
            case ContactType::ContactRemoved:
                OLO_CORE_TRACE("Contact removed between entities {0} and {1}", (u64)entityA, (u64)entityB);
                break;
        }
    }

    void JoltScene::CreateRigidBodies()
    {
        if (!m_Scene)
            return;

        // Create physics bodies for all entities with Rigidbody3DComponent
        auto view = m_Scene->GetAllEntitiesWith<Rigidbody3DComponent>();
        for (auto entityID : view)
        {
            Entity entity{ entityID, m_Scene };
            CreateBody(entity);
        }

        OLO_CORE_INFO("Created {0} physics bodies", m_Bodies.size());
    }

    namespace
    {
        // Any unit vector perpendicular to `v` (assumed normalized). Used to
        // derive the hinge/slider "normal" axis from the single authored primary
        // axis, since both reference frames can share it in world space.
        glm::vec3 PerpendicularTo(const glm::vec3& v)
        {
            const glm::vec3 ref = (glm::abs(v.x) < 0.9f) ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            return glm::normalize(glm::cross(v, ref));
        }
    } // namespace

    void JoltScene::CreateConstraints()
    {
        if (!m_Scene)
            return;

        // Second pass: every JoltBody now exists, so two-body constraints can
        // resolve both endpoints.
        auto view = m_Scene->GetAllEntitiesWith<PhysicsJoint3DComponent>();
        for (auto entityID : view)
        {
            Entity entity{ entityID, m_Scene };
            CreateConstraint(entity);
        }

        OLO_CORE_INFO("Created {0} physics constraints", m_Constraints.size());
    }

    bool JoltScene::CreateConstraint(Entity entity)
    {
        if (!m_JoltSystem || !entity || !entity.HasComponent<PhysicsJoint3DComponent>())
            return false;

        auto& joint = entity.GetComponent<PhysicsJoint3DComponent>();
        UUID entityID = entity.GetUUID();

        // Idempotent — already built for this entity.
        if (m_Constraints.find(entityID) != m_Constraints.end())
            return true;

        // The joint owner must have a physics body to be a constraint endpoint.
        Ref<JoltBody> bodyA = GetBodyByEntityID(entityID);
        if (!bodyA || !bodyA->IsValid() || !entity.HasComponent<TransformComponent>())
        {
            OLO_CORE_WARN("PhysicsJoint3D on entity {0} has no valid rigidbody; skipping constraint", (u64)entityID);
            return false;
        }

        const bool connectToWorld = (static_cast<u64>(joint.m_ConnectedEntity) == 0);

        Entity connectedEntity{};
        Ref<JoltBody> bodyB = nullptr;
        if (!connectToWorld)
        {
            if (joint.m_ConnectedEntity == entityID)
            {
                OLO_CORE_WARN("PhysicsJoint3D on entity {0} connects to itself; skipping constraint", (u64)entityID);
                return false;
            }

            connectedEntity = m_Scene->GetEntityByUUID(joint.m_ConnectedEntity);
            bodyB = GetBodyByEntityID(joint.m_ConnectedEntity);
            if (!bodyB || !bodyB->IsValid() || !connectedEntity || !connectedEntity.HasComponent<TransformComponent>())
            {
                OLO_CORE_WARN("PhysicsJoint3D on entity {0}: connected entity {1} has no valid rigidbody; skipping constraint",
                              (u64)entityID, (u64)joint.m_ConnectedEntity);
                return false;
            }
        }

        // World-space anchors/axis, computed from the same authored transforms the
        // bodies were created from (JoltBody places bodies at TransformComponent's
        // local translation/rotation). With mSpace == WorldSpace, Jolt converts
        // these world points to each body's COM frame internally.
        const auto& tcA = entity.GetComponent<TransformComponent>();
        const glm::quat rotA = tcA.GetRotation();
        const glm::vec3 worldA = tcA.Translation + rotA * joint.m_LocalAnchorA;

        glm::vec3 axis = rotA * joint.m_Axis;
        axis = (glm::dot(axis, axis) < 1e-12f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::normalize(axis);
        const glm::vec3 normal = PerpendicularTo(axis);

        glm::vec3 worldB;
        if (connectToWorld)
        {
            // Pin body A's anchor at its current world location.
            worldB = worldA;
        }
        else
        {
            const auto& tcB = connectedEntity.GetComponent<TransformComponent>();
            worldB = tcB.Translation + tcB.GetRotation() * joint.m_LocalAnchorB;
        }

        // Build the constraint with body1 = connected/world, body2 = this body.
        // Placing the (possibly infinite-mass) connected/world body first keeps
        // the slider's "body 1 should be the heaviest body" guidance satisfied for
        // the common static-anchor case.
        auto buildConstraint = [&](JPH::Body& body1, JPH::Body& body2) -> JPH::TwoBodyConstraint*
        {
            const JPH::RVec3 p1 = JoltUtils::ToJoltRVec3(worldB);
            const JPH::RVec3 p2 = JoltUtils::ToJoltRVec3(worldA);
            const JPH::Vec3 jAxis = JoltUtils::ToJoltVector(axis);
            const JPH::Vec3 jNormal = JoltUtils::ToJoltVector(normal);

            switch (joint.m_Type)
            {
                case JointType3D::Fixed:
                {
                    JPH::FixedConstraintSettings s;
                    // Body-to-body: auto-detect the point so the current relative
                    // pose becomes the locked rest pose. World-fixed: pin explicitly.
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mAutoDetectPoint = !connectToWorld;
                    if (connectToWorld)
                    {
                        s.mPoint1 = p1;
                        s.mPoint2 = p2;
                    }
                    return s.Create(body1, body2);
                }
                case JointType3D::Point:
                {
                    JPH::PointConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mPoint1 = p1;
                    s.mPoint2 = p2;
                    return s.Create(body1, body2);
                }
                case JointType3D::Distance:
                {
                    JPH::DistanceConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mPoint1 = p1;
                    s.mPoint2 = p2;
                    s.mMinDistance = joint.m_MinDistance;
                    s.mMaxDistance = joint.m_MaxDistance;
                    return s.Create(body1, body2);
                }
                case JointType3D::Hinge:
                {
                    JPH::HingeConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mPoint1 = p1;
                    s.mPoint2 = p2;
                    s.mHingeAxis1 = s.mHingeAxis2 = jAxis;
                    s.mNormalAxis1 = s.mNormalAxis2 = jNormal;
                    s.mLimitsMin = std::clamp(JoltUtils::DegreesToRadians(joint.m_HingeMinAngleDeg), -glm::pi<f32>(), 0.0f);
                    s.mLimitsMax = std::clamp(JoltUtils::DegreesToRadians(joint.m_HingeMaxAngleDeg), 0.0f, glm::pi<f32>());
                    return s.Create(body1, body2);
                }
                case JointType3D::Slider:
                {
                    JPH::SliderConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mAutoDetectPoint = false;
                    s.mPoint1 = p1;
                    s.mPoint2 = p2;
                    s.mSliderAxis1 = s.mSliderAxis2 = jAxis;
                    s.mNormalAxis1 = s.mNormalAxis2 = jNormal;
                    s.mLimitsMin = joint.m_SliderMinLimit;
                    s.mLimitsMax = joint.m_SliderMaxLimit;
                    return s.Create(body1, body2);
                }
                case JointType3D::Cone:
                {
                    JPH::ConeConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mPoint1 = p1;
                    s.mPoint2 = p2;
                    s.mTwistAxis1 = s.mTwistAxis2 = jAxis;
                    s.mHalfConeAngle = std::clamp(JoltUtils::DegreesToRadians(joint.m_ConeHalfAngleDeg), 0.0f, glm::pi<f32>());
                    return s.Create(body1, body2);
                }
            }
            return nullptr;
        };

        // Lock the endpoint bodies to obtain JPH::Body& for settings.Create().
        // No simulation is running during the runtime-start second pass, but the
        // runtime-add hook can call this mid-frame, so lock defensively.
        JPH::TwoBodyConstraint* constraint = nullptr;
        const JPH::BodyLockInterface& lockInterface = m_JoltSystem->GetBodyLockInterface();

        if (connectToWorld)
        {
            JPH::BodyLockWrite lock(lockInterface, bodyA->GetBodyID());
            if (!lock.Succeeded())
            {
                OLO_CORE_WARN("PhysicsJoint3D: failed to lock body for entity {0}", (u64)entityID);
                return false;
            }
            constraint = buildConstraint(JPH::Body::sFixedToWorld, lock.GetBody());
        }
        else
        {
            const JPH::BodyID ids[2] = { bodyB->GetBodyID(), bodyA->GetBodyID() };
            JPH::BodyLockMultiWrite lock(lockInterface, ids, 2);
            JPH::Body* b1 = lock.GetBody(0); // connected body (B)
            JPH::Body* b2 = lock.GetBody(1); // this body (A)
            if (!b1 || !b2)
            {
                OLO_CORE_WARN("PhysicsJoint3D: failed to lock bodies for entity {0}", (u64)entityID);
                return false;
            }
            constraint = buildConstraint(*b1, *b2);
        }

        if (!constraint)
        {
            OLO_CORE_ERROR("PhysicsJoint3D: failed to create constraint for entity {0}", (u64)entityID);
            return false;
        }

        // AddConstraint and m_Constraints each hold a JPH::Ref, balancing the
        // RemoveConstraint + map-erase pair in DestroyConstraint/DestroyAllConstraints.
        m_JoltSystem->AddConstraint(constraint);
        m_Constraints[entityID] = constraint;
        joint.m_RuntimeConstraintToken = static_cast<u64>(entityID);

        OLO_CORE_TRACE("Created physics constraint for entity {0}", (u64)entityID);
        return true;
    }

    void JoltScene::DestroyConstraint(Entity entity)
    {
        if (!entity)
            return;

        UUID entityID = entity.GetUUID();
        if (auto it = m_Constraints.find(entityID); it != m_Constraints.end())
        {
            if (m_JoltSystem && it->second != nullptr)
                m_JoltSystem->RemoveConstraint(it->second);
            m_Constraints.erase(it); // releases the JPH::Ref
            OLO_CORE_TRACE("Destroyed physics constraint for entity {0}", (u64)entityID);
        }
    }

    void JoltScene::DestroyAllConstraints()
    {
        // Constraints reference bodies, so they must be removed before the bodies
        // they connect are destroyed.
        if (m_JoltSystem)
        {
            for (auto& [entityID, constraint] : m_Constraints)
            {
                if (constraint != nullptr)
                    m_JoltSystem->RemoveConstraint(constraint);
            }
        }
        m_Constraints.clear();
    }

    void JoltScene::SynchronizeBody(Ref<JoltBody> body) const
    {
        if (!body || !body->IsValid())
            return;

        Entity entity = body->GetEntity();
        if (!entity.HasComponent<TransformComponent>())
            return;

        // Get physics transform
        glm::vec3 position = body->GetPosition();
        glm::quat rotation = body->GetRotation();

        // Update entity transform
        auto& transform = entity.GetComponent<TransformComponent>();
        transform.Translation = position;
        transform.SetRotation(rotation);
    }

    void JoltScene::InitializeJolt()
    {
        // Allocate process-global Jolt state only when this is the first
        // active JoltScene. Subsequent JoltScene::Initialize() calls share
        // the existing Factory and registered RTTI — see comment on
        // s_JoltGlobalRefCount above for the leak this prevents.
        if (s_JoltGlobalRefCount.fetch_add(1, std::memory_order_acq_rel) == 0)
        {
            JPH::RegisterDefaultAllocator();
            JPH::Trace = nullptr; // Disable Jolt's trace output
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }

        // Create temp allocator
        m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(s_TempAllocatorSize);

        // Create job system adapter that wraps FScheduler
        m_JobSystem = std::make_unique<JoltJobSystemAdapter>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);

        // Create layer interfaces
        m_BroadPhaseLayerInterface = std::make_unique<BroadPhaseLayerInterface>();
        m_ObjectVsBroadPhaseLayerFilter = std::make_unique<ObjectVsBroadPhaseLayerFilter>();
        m_ObjectLayerPairFilter = std::make_unique<ObjectLayerPairFilter>();

        // Create physics system
        m_JoltSystem = std::make_unique<JPH::PhysicsSystem>();
        m_JoltSystem->Init(s_MaxBodies, s_NumBodyMutexes, s_MaxBodyPairs, s_MaxContactConstraints,
                           *m_BroadPhaseLayerInterface, *m_ObjectVsBroadPhaseLayerFilter, *m_ObjectLayerPairFilter);

        // Create contact listener
        m_ContactListener = std::make_unique<JoltContactListener>(*this);
        m_JoltSystem->SetContactListener(m_ContactListener.get());

        // Set default gravity
        m_JoltSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

        OLO_CORE_INFO("Jolt Physics initialized - MaxBodies: {0}, MaxBodyPairs: {1}, MaxContactConstraints: {2}",
                      s_MaxBodies, s_MaxBodyPairs, s_MaxContactConstraints);
    }

    void JoltScene::ShutdownJolt()
    {
        if (m_JoltSystem)
        {
            // Remove contact listener
            m_JoltSystem->SetContactListener(nullptr);
        }

        // Destroy in reverse order
        m_ContactListener.reset();
        m_JoltSystem.reset();
        m_ObjectLayerPairFilter.reset();
        m_ObjectVsBroadPhaseLayerFilter.reset();
        m_BroadPhaseLayerInterface.reset();
        m_JobSystem.reset();
        m_TempAllocator.reset();

        // Free process-global Jolt state only when this is the last active
        // JoltScene. Otherwise other scenes still rely on the Factory and
        // registered RTTI.
        if (s_JoltGlobalRefCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }

        OLO_CORE_INFO("Jolt Physics shut down");
    }

    // Scene query helper methods - Legacy vector-based interface (now forwards to ExcludedEntitySet)
    bool JoltScene::PerformShapeCast(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                                     f32 maxDistance, u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit& outHit)
    {
        // Create temporary ExcludedEntitySet for O(1) lookups during the query
        ExcludedEntitySet tempExclusionSet(excludedEntities);
        return PerformShapeCast(shape, start, direction, maxDistance, layerMask, tempExclusionSet, outHit);
    }

    i32 JoltScene::PerformShapeCastMultiple(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                                            f32 maxDistance, u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit* outHits, i32 maxHits)
    {
        // Create temporary ExcludedEntitySet for O(1) lookups during the query
        ExcludedEntitySet tempExclusionSet(excludedEntities);
        return PerformShapeCastMultiple(shape, start, direction, maxDistance, layerMask, tempExclusionSet, outHits, maxHits);
    }

    i32 JoltScene::PerformShapeOverlap(JPH::Ref<JPH::Shape> shape, const glm::vec3& position, const glm::quat& rotation,
                                       u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit* outHits, i32 maxHits)
    {
        // Create temporary ExcludedEntitySet for O(1) lookups during the query
        ExcludedEntitySet tempExclusionSet(excludedEntities);
        return PerformShapeOverlap(shape, position, rotation, layerMask, tempExclusionSet, outHits, maxHits);
    }

    bool JoltScene::IsEntityExcluded(UUID entityID, const std::vector<UUID>& excludedEntities)
    {
        return EntityExclusionUtils::IsEntityExcluded(excludedEntities, entityID);
    }

    // Optimized ExcludedEntitySet-based implementations
    bool JoltScene::PerformShapeCast(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                                     f32 maxDistance, u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit& outHit) const
    {
        if (!m_JoltSystem)
            return false;

        outHit.Clear();

        // Create shape cast
        JPH::Vec3 startPos = JoltUtils::ToJoltVector(start);
        JPH::Vec3 castDirection = JoltUtils::ToJoltVector(glm::normalize(direction)) * maxDistance;

        JPH::RShapeCast shapeCast = JPH::RShapeCast::sFromWorldTransform(
            shape,
            JPH::Vec3::sReplicate(1.0f),
            JPH::RMat44::sTranslation(startPos),
            castDirection);

        // Perform shape cast using optimized ExcludedEntitySet body filter
        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> hitCollector;
        JPH::ShapeCastSettings shapeCastSettings;

        // Create filters
        JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*m_ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayer(layerMask));
        JPH::DefaultObjectLayerFilter objectLayerFilter(*m_ObjectLayerPairFilter, JPH::ObjectLayer(layerMask));
        EntityExclusionBodyFilter bodyFilter(excludedEntitySet); // Use ExcludedEntitySet directly

        m_JoltSystem->GetNarrowPhaseQuery().CastShape(shapeCast, shapeCastSettings, startPos, hitCollector, broadPhaseFilter, objectLayerFilter, bodyFilter);

        if (!hitCollector.HadHit())
            return false;

        // Fill hit information
        FillHitInfo(hitCollector.mHit, shapeCast, outHit);
        return true;
    }

    i32 JoltScene::PerformShapeCastMultiple(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                                            f32 maxDistance, u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit* outHits, i32 maxHits)
    {
        if (!m_JoltSystem || maxHits <= 0)
            return 0;

        // Create shape cast
        JPH::Vec3 startPos = JoltUtils::ToJoltVector(start);
        JPH::Vec3 castDirection = JoltUtils::ToJoltVector(glm::normalize(direction)) * maxDistance;

        JPH::RShapeCast shapeCast = JPH::RShapeCast::sFromWorldTransform(
            shape,
            JPH::Vec3::sReplicate(1.0f),
            JPH::RMat44::sTranslation(startPos),
            castDirection);

        // Perform shape cast with multiple hit collector using optimized ExcludedEntitySet body filter
        JPH::AllHitCollisionCollector<JPH::CastShapeCollector> hitCollector;
        JPH::ShapeCastSettings shapeCastSettings;

        // Create filters
        JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*m_ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayer(layerMask));
        JPH::DefaultObjectLayerFilter objectLayerFilter(*m_ObjectLayerPairFilter, JPH::ObjectLayer(layerMask));
        EntityExclusionBodyFilter bodyFilter(excludedEntitySet); // Use ExcludedEntitySet directly

        m_JoltSystem->GetNarrowPhaseQuery().CastShape(shapeCast, shapeCastSettings, startPos, hitCollector, broadPhaseFilter, objectLayerFilter, bodyFilter);

        // Fill hit results
        i32 hitCount = 0;
        for (const auto& hit : hitCollector.mHits)
        {
            if (hitCount >= maxHits)
                break;

            FillHitInfo(hit, shapeCast, outHits[hitCount]);
            ++hitCount;
        }

        return hitCount;
    }

    i32 JoltScene::PerformShapeOverlap(JPH::Ref<JPH::Shape> shape, const glm::vec3& position, const glm::quat& rotation,
                                       u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit* outHits, i32 maxHits)
    {
        if (!m_JoltSystem || maxHits <= 0)
            return 0;

        // Create transform for the overlap shape
        JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(
            JoltUtils::ToJoltQuat(rotation),
            JoltUtils::ToJoltVector(position));

        // Perform overlap query using optimized ExcludedEntitySet body filter
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hitCollector;
        JPH::CollideShapeSettings overlapSettings;

        // Create filters
        JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*m_ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayer(layerMask));
        JPH::DefaultObjectLayerFilter objectLayerFilter(*m_ObjectLayerPairFilter, JPH::ObjectLayer(layerMask));
        EntityExclusionBodyFilter bodyFilter(excludedEntitySet); // Use ExcludedEntitySet directly

        m_JoltSystem->GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sReplicate(1.0f), transform, overlapSettings,
                                                         JPH::Vec3::sZero(), hitCollector, broadPhaseFilter, objectLayerFilter, bodyFilter);

        // Fill hit results
        i32 hitCount = 0;
        for (const auto& hit : hitCollector.mHits)
        {
            if (hitCount >= maxHits)
                break;

            // Fill basic hit info for overlap (no distance or penetration info available from CollideShape)
            SceneQueryHit& hitInfo = outHits[hitCount];
            hitInfo.Clear();

            // Lock body to get entity information
            JPH::BodyLockRead bodyLock(m_JoltSystem->GetBodyLockInterface(), hit.mBodyID2);
            if (bodyLock.Succeeded())
            {
                const JPH::Body& body = bodyLock.GetBody();
                hitInfo.m_HitEntity = static_cast<UUID>(body.GetUserData());
                hitInfo.m_Position = JoltUtils::FromJoltVector(body.GetPosition());

                // Get body from our map for reference
                if (auto it = m_Bodies.find(hitInfo.m_HitEntity); it != m_Bodies.end())
                {
                    hitInfo.m_HitBody = it->second;
                }

                ++hitCount;
            }
        }

        return hitCount;
    }

    bool JoltScene::IsEntityExcluded(UUID entityID, const ExcludedEntitySet& excludedEntitySet)
    {
        return EntityExclusionUtils::IsEntityExcluded(excludedEntitySet, entityID);
    }

    void JoltScene::FillHitInfo(const JPH::RayCastResult& hit, const JPH::RRayCast& ray, SceneQueryHit& outHit) const
    {
        outHit.Clear();

        JPH::Vec3 hitPosition = ray.GetPointOnRay(hit.mFraction);
        outHit.m_Position = JoltUtils::FromJoltVector(hitPosition);
        outHit.m_Distance = hit.mFraction * ray.mDirection.Length();

        // Lock the body to get additional information
        JPH::BodyLockRead bodyLock(m_JoltSystem->GetBodyLockInterface(), hit.mBodyID);
        if (bodyLock.Succeeded())
        {
            const JPH::Body& body = bodyLock.GetBody();
            outHit.m_HitEntity = static_cast<UUID>(body.GetUserData());
            outHit.m_Normal = JoltUtils::FromJoltVector(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPosition));

            // Get body reference from our map
            auto it = m_Bodies.find(outHit.m_HitEntity);
            if (it != m_Bodies.end())
            {
                outHit.m_HitBody = it->second;
            }
        }
    }

    void JoltScene::FillHitInfo(const JPH::ShapeCastResult& hit, const JPH::RShapeCast& shapeCast, SceneQueryHit& outHit) const
    {
        outHit.Clear();

        JPH::Vec3 hitPosition = shapeCast.GetPointOnRay(hit.mFraction);
        outHit.m_Position = JoltUtils::FromJoltVector(hitPosition);
        outHit.m_Distance = hit.mFraction * shapeCast.mDirection.Length();
        outHit.m_Normal = JoltUtils::FromJoltVector(hit.mPenetrationAxis.Normalized());

        // Lock the body to get additional information
        JPH::BodyLockRead bodyLock(m_JoltSystem->GetBodyLockInterface(), hit.mBodyID2);
        if (bodyLock.Succeeded())
        {
            const JPH::Body& body = bodyLock.GetBody();
            outHit.m_HitEntity = static_cast<UUID>(body.GetUserData());

            // Get body reference from our map
            auto it = m_Bodies.find(outHit.m_HitEntity);
            if (it != m_Bodies.end())
            {
                outHit.m_HitBody = it->second;
            }
        }
    }

} // namespace OloEngine
