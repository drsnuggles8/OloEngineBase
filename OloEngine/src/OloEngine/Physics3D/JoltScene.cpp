#include "OloEnginePCH.h"
#include "JoltScene.h"
#include "EntityExclusionBodyFilter.h"
#include "SceneQueries.h"
#include "JoltShapes.h"
#include "PhysicsEvents.h"
#include "Physics3DSystem.h" // Physics3DSystem::GetSettings() - single source of truth for PhysicsSettings (issue #523)
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h" // SkeletonComponent (ragdoll skeleton resolution)
#include "OloEngine/Animation/BoneEntityUtils.h"        // FindBoneEntityIds (ragdoll bone -> entity mapping)

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
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/PulleyConstraint.h>
#include <Jolt/Physics/Constraints/GearConstraint.h>
#include <Jolt/Physics/Constraints/RackAndPinionConstraint.h>
#include <Jolt/Physics/Constraints/PathConstraint.h>
#include <Jolt/Physics/Constraints/PathConstraintPathHermite.h>
#include <Jolt/Physics/Constraints/SpringSettings.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp> // glm::inverse(quat) for ragdoll bone-local anchors

#include <cmath>
#include <vector>

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

        // Shared cloth soft-body access (issue #460): lock the body, validate it is a soft
        // body carrying motion properties, then invoke fn(body, motion). Returns false
        // (fn NOT called) if the lock fails, the body isn't a soft body, or it has no
        // motion properties — the exact early-return chain GetClothVertices /
        // GetClothPinnedVertexIndices / DriveClothAttachment all share. The lock lives for
        // the duration of fn, so callers must read/write everything they need inside it.
        // Read and write variants differ only in the lock type and the const-ness handed
        // to fn (GetClothWorldPosition deliberately does NOT use these — it never touches
        // motion properties, so it keeps its own leaner lock).
        template<typename Fn>
        bool WithClothSoftBodyRead(const JPH::BodyLockInterface& lockInterface, JPH::BodyID bodyID, Fn&& fn)
        {
            JPH::BodyLockRead lock(lockInterface, bodyID);
            if (!lock.Succeeded())
                return false;

            const JPH::Body& body = lock.GetBody();
            if (!body.IsSoftBody())
                return false;

            const auto* motion = static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
            if (!motion)
                return false;

            fn(body, *motion);
            return true;
        }

        template<typename Fn>
        bool WithClothSoftBodyWrite(const JPH::BodyLockInterface& lockInterface, JPH::BodyID bodyID, Fn&& fn)
        {
            JPH::BodyLockWrite lock(lockInterface, bodyID);
            if (!lock.Succeeded())
                return false;

            JPH::Body& body = lock.GetBody();
            if (!body.IsSoftBody())
                return false;

            auto* motion = static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
            if (!motion)
                return false;

            fn(body, *motion);
            return true;
        }
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

        // Remove static terrain height-field bodies while m_JoltSystem is still alive.
        DestroyAllTerrainBodies();

        // Remove cloth soft bodies while m_JoltSystem is still alive.
        DestroyAllClothBodies();

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

        PreSimulate();

        const u32 steps = BeginSteps(deltaTime);
        for (u32 stepIndex = 0; stepIndex < steps; ++stepIndex)
        {
            Step(m_FixedTimeStep);
        }

        // Synchronize transforms after simulation
        SynchronizeTransforms();
    }

    void JoltScene::PreSimulate()
    {
        // Process contact events queued by the previous frame's world update.
        // GAME THREAD: handlers route through Scene::OnContactEvent, which may
        // grow script/registry access.
        if (m_ContactListener)
        {
            m_ContactListener->ProcessContactEvents();
        }
    }

    u32 JoltScene::BeginSteps(f32 deltaTime)
    {
        if (!m_Initialized || !m_JoltSystem)
            return 0;

        // Fixed timestep simulation with accumulator
        m_Accumulator += deltaTime;

        // Prevent "spiral of death" by capping simulation steps per frame:
        // if the accumulator exceeds the maximum, clamp it and log the skip.
        if (f32 maxAccumulator = static_cast<f32>(s_MaxStepsPerFrame) * m_FixedTimeStep; m_Accumulator > maxAccumulator)
        {
            f32 skippedTime = m_Accumulator - maxAccumulator;
            m_Accumulator = maxAccumulator;
            OLO_CORE_WARN("Physics timestep accumulator clamped! Skipped {0} seconds to prevent spiral of death", skippedTime);
        }

        // Authorize and CONSUME the fixed steps this frame's delta affords —
        // the caller runs exactly this many Step()s (or phase triplets).
        u32 steps = 0;
        while (m_Accumulator >= m_FixedTimeStep && steps < s_MaxStepsPerFrame)
        {
            m_Accumulator -= m_FixedTimeStep;
            ++steps;
        }
        return steps;
    }

    void JoltScene::Step(f32 fixedTimeStep)
    {
        StepCharacterAndVehiclePhase(fixedTimeStep);
        StepWorldPhase(fixedTimeStep);
        StepJointBreakPhase(fixedTimeStep);
    }

    void JoltScene::StepCharacterAndVehiclePhase(f32 fixedTimeStep)
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

        // Push authored driver input into each vehicle controller before the
        // world update, so the VehicleConstraint step listener (which runs inside
        // StepWorldPhase's Update) acts on this frame's throttle / steering / brake.
        UpdateVehicleControllers();
    }

    void JoltScene::StepWorldPhase(f32 fixedTimeStep)
    {
        if (!m_JoltSystem)
            return;

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
            // Throttle: an overflow (e.g. ContactConstraintsFull) otherwise re-logs every
            // fixed step for as long as it persists. Log immediately on the first hit / on
            // a transition to a different error, then only every kPhysicsUpdateErrorLogInterval
            // steps while it repeats (issue #523).
            const bool isNewError = error != m_LastPhysicsUpdateError;
            if (isNewError)
                m_PhysicsUpdateErrorRepeatCount = 0;
            if (isNewError || m_PhysicsUpdateErrorRepeatCount % kPhysicsUpdateErrorLogInterval == 0)
            {
                OLO_CORE_ERROR("Jolt physics update error: {0} - raise the matching PhysicsSettings limit "
                               "(m_MaxContactConstraints / m_MaxBodyPairs / m_MaxBodies)",
                               static_cast<i32>(error));
            }
            ++m_PhysicsUpdateErrorRepeatCount;
            m_LastPhysicsUpdateError = error;
        }
        else
        {
            m_LastPhysicsUpdateError = JPH::EPhysicsUpdateError::None;
            m_PhysicsUpdateErrorRepeatCount = 0;
        }

        // Post-simulate character controllers — pure Jolt-internal state
        // (displacement/rotation reset + angular-velocity derivation), so it is
        // part of the worker-safe phase.
        for (auto& characterController : m_CharacterControllersToUpdate)
        {
            characterController->PostSimulate();
        }
    }

    void JoltScene::StepJointBreakPhase(f32 fixedTimeStep)
    {
        if (!m_JoltSystem)
            return;

        // Break any joint whose accumulated constraint impulse this step exceeded
        // its authored threshold. Per fixed step because the lambdas are per-step
        // state that the next world update overwrites — the async split runs it
        // on the game thread at the fence, before any next kick.
        BreakOverstressedJoints(fixedTimeStep);
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

    JPH::BodyID JoltScene::CreateTerrainBody(Entity entity, const JPH::Ref<JPH::Shape>& shape,
                                             const glm::vec3& position, const glm::quat& rotation)
    {
        if (!m_Initialized || !m_JoltSystem)
        {
            OLO_CORE_ERROR("CreateTerrainBody: physics not initialized");
            return JPH::BodyID();
        }
        if (!entity || !shape)
        {
            OLO_CORE_ERROR("CreateTerrainBody: invalid entity or null shape");
            return JPH::BodyID();
        }

        const UUID entityID = entity.GetUUID();

        // Idempotent: drop any prior terrain body for this entity (rebuild / regenerate).
        DestroyTerrainBody(entityID);

        // Static world geometry → NON_MOVING object layer (collides with moving bodies
        // and characters; never with other static geometry). DontActivate: a static
        // body never simulates, it just sits in the broadphase for queries/contacts.
        JPH::BodyCreationSettings settings(
            shape,
            JoltUtils::ToJoltVector(position),
            JoltUtils::ToJoltQuat(rotation),
            JPH::EMotionType::Static,
            ObjectLayers::NON_MOVING);
        settings.mUserData = static_cast<u64>(entityID);
        // Grippy ground so characters/vehicles don't slide on slopes; no bounce.
        settings.mFriction = 0.8f;
        settings.mRestitution = 0.0f;

        JPH::BodyID bodyID = m_JoltSystem->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (bodyID.IsInvalid())
        {
            OLO_CORE_ERROR("CreateTerrainBody: Jolt failed to create body for terrain entity {0}", (u64)entityID);
            return JPH::BodyID();
        }

        m_TerrainBodies[entityID] = bodyID;
        m_BodyIDToEntity[bodyID] = entityID;

        OLO_CORE_TRACE("Created terrain collision body for entity {0}, BodyID: {1}", (u64)entityID, bodyID.GetIndex());
        return bodyID;
    }

    void JoltScene::DestroyTerrainBody(UUID entityID)
    {
        auto it = m_TerrainBodies.find(entityID);
        if (it == m_TerrainBodies.end())
            return;

        const JPH::BodyID bodyID = it->second;
        m_BodyIDToEntity.erase(bodyID);
        if (m_JoltSystem)
        {
            auto& bodyInterface = m_JoltSystem->GetBodyInterface();
            bodyInterface.RemoveBody(bodyID);
            bodyInterface.DestroyBody(bodyID);
        }
        m_TerrainBodies.erase(it);
        OLO_CORE_TRACE("Destroyed terrain collision body for entity {0}", (u64)entityID);
    }

    void JoltScene::DestroyAllTerrainBodies()
    {
        if (m_JoltSystem)
        {
            auto& bodyInterface = m_JoltSystem->GetBodyInterface();
            for (const auto& [entityID, bodyID] : m_TerrainBodies)
            {
                m_BodyIDToEntity.erase(bodyID);
                bodyInterface.RemoveBody(bodyID);
                bodyInterface.DestroyBody(bodyID);
            }
        }
        m_TerrainBodies.clear();
    }

    JPH::BodyID JoltScene::CreateClothBody(Entity entity, const JPH::Ref<JPH::SoftBodySharedSettings>& settings,
                                           u32 iterations, f32 linearDamping, f32 pressure)
    {
        if (!m_Initialized || !m_JoltSystem)
        {
            OLO_CORE_ERROR("CreateClothBody: physics not initialized");
            return JPH::BodyID();
        }
        if (!entity || !settings || settings->mVertices.empty())
        {
            OLO_CORE_ERROR("CreateClothBody: invalid entity or empty soft-body settings");
            return JPH::BodyID();
        }

        const UUID entityID = entity.GetUUID();

        // Idempotent: drop any prior cloth body for this entity (rebuild / regenerate).
        DestroyClothBody(entityID);

        // The particles already carry their world-space rest positions (JoltShapes baked the
        // entity transform in), so the body is created at the origin with identity rotation
        // on the MOVING layer — it falls under gravity and collides with static/rigid bodies.
        JPH::SoftBodyCreationSettings creation(settings.GetPtr(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), ObjectLayers::MOVING);
        creation.mUserData = static_cast<u64>(entityID);
        creation.mNumIterations = std::clamp<JPH::uint32>(iterations, 1, 32);
        creation.mLinearDamping = std::isfinite(linearDamping) ? std::max(0.0f, linearDamping) : 0.1f;
        creation.mPressure = std::isfinite(pressure) ? std::max(0.0f, pressure) : 0.0f;
        // A small particle radius keeps the cloth a hair off surfaces it rests on, avoiding
        // z-fighting and reducing tunnelling through thin colliders.
        creation.mVertexRadius = 0.01f;

        JPH::BodyID bodyID = m_JoltSystem->GetBodyInterface().CreateAndAddSoftBody(creation, JPH::EActivation::Activate);
        if (bodyID.IsInvalid())
        {
            OLO_CORE_ERROR("CreateClothBody: Jolt failed to create soft body for entity {0}", (u64)entityID);
            return JPH::BodyID();
        }

        m_Cloths[entityID] = ClothRuntime{ bodyID, settings };
        m_BodyIDToEntity[bodyID] = entityID;

        OLO_CORE_TRACE("Created cloth soft body for entity {0}, BodyID: {1}, particles: {2}",
                       (u64)entityID, bodyID.GetIndex(), settings->mVertices.size());
        return bodyID;
    }

    void JoltScene::DestroyClothBody(UUID entityID)
    {
        auto it = m_Cloths.find(entityID);
        if (it == m_Cloths.end())
            return;

        const JPH::BodyID bodyID = it->second.m_BodyID;
        m_BodyIDToEntity.erase(bodyID);
        if (m_JoltSystem && !bodyID.IsInvalid())
        {
            auto& bodyInterface = m_JoltSystem->GetBodyInterface();
            bodyInterface.RemoveBody(bodyID);
            bodyInterface.DestroyBody(bodyID);
        }
        m_Cloths.erase(it);
        OLO_CORE_TRACE("Destroyed cloth soft body for entity {0}", (u64)entityID);
    }

    void JoltScene::DestroyAllClothBodies()
    {
        if (m_JoltSystem)
        {
            auto& bodyInterface = m_JoltSystem->GetBodyInterface();
            for (const auto& [entityID, cloth] : m_Cloths)
            {
                m_BodyIDToEntity.erase(cloth.m_BodyID);
                if (!cloth.m_BodyID.IsInvalid())
                {
                    bodyInterface.RemoveBody(cloth.m_BodyID);
                    bodyInterface.DestroyBody(cloth.m_BodyID);
                }
            }
        }
        m_Cloths.clear();
    }

    bool JoltScene::GetClothVertices(UUID entityID, std::vector<glm::vec3>& outWorldPositions) const
    {
        outWorldPositions.clear();

        auto it = m_Cloths.find(entityID);
        if (it == m_Cloths.end() || !m_JoltSystem)
            return false;

        // Lock the body read-only and pull the current particle positions out of its
        // SoftBodyMotionProperties. Each particle position is stored relative to the body's
        // centre of mass, so transform it by the centre-of-mass transform to get world space.
        return WithClothSoftBodyRead(m_JoltSystem->GetBodyLockInterface(), it->second.m_BodyID,
                                     [&](const JPH::Body& body, const JPH::SoftBodyMotionProperties& motion)
                                     {
            const JPH::RMat44 comTransform = body.GetCenterOfMassTransform();
            const auto& vertices = motion.GetVertices();
            outWorldPositions.reserve(vertices.size());
            for (const auto& vertex : vertices)
            {
                const JPH::RVec3 world = comTransform * vertex.mPosition;
                outWorldPositions.emplace_back(JoltUtils::FromJoltRVec3(world));
            } });
    }

    bool JoltScene::GetClothWorldPosition(UUID entityID, glm::vec3& outPosition) const
    {
        auto it = m_Cloths.find(entityID);
        if (it == m_Cloths.end() || !m_JoltSystem)
            return false;

        JPH::BodyLockRead lock(m_JoltSystem->GetBodyLockInterface(), it->second.m_BodyID);
        if (!lock.Succeeded())
            return false;

        outPosition = JoltUtils::FromJoltRVec3(lock.GetBody().GetCenterOfMassPosition());
        return true;
    }

    void JoltScene::ApplyClothWindForce(UUID entityID, const glm::vec3& force)
    {
        if (!m_JoltSystem)
            return;
        if (!std::isfinite(force.x) || !std::isfinite(force.y) || !std::isfinite(force.z))
            return;

        auto it = m_Cloths.find(entityID);
        if (it == m_Cloths.end() || it->second.m_BodyID.IsInvalid())
            return;

        m_JoltSystem->GetBodyInterface().AddForce(it->second.m_BodyID, JPH::Vec3(force.x, force.y, force.z));
    }

    bool JoltScene::GetClothPinnedVertexIndices(UUID entityID, std::vector<u32>& outIndices) const
    {
        outIndices.clear();

        auto it = m_Cloths.find(entityID);
        if (it == m_Cloths.end() || !m_JoltSystem)
            return false;

        return WithClothSoftBodyRead(m_JoltSystem->GetBodyLockInterface(), it->second.m_BodyID,
                                     [&](const JPH::Body&, const JPH::SoftBodyMotionProperties& motion)
                                     {
            const auto& vertices = motion.GetVertices();
            for (u32 i = 0; i < static_cast<u32>(vertices.size()); ++i)
            {
                // A pinned particle carries zero inverse mass (JoltShapes::CreateClothSharedSettings).
                if (vertices[i].mInvMass == 0.0f)
                    outIndices.push_back(i);
            } });
    }

    void JoltScene::DriveClothAttachment(UUID entityID, const std::vector<u32>& vertexIndices,
                                         const std::vector<glm::vec3>& targetWorldPositions, f32 dt)
    {
        if (!m_JoltSystem || !std::isfinite(dt) || dt <= 0.0f)
            return;
        if (vertexIndices.empty() || vertexIndices.size() != targetWorldPositions.size())
            return;

        auto it = m_Cloths.find(entityID);
        if (it == m_Cloths.end() || it->second.m_BodyID.IsInvalid())
            return;

        bool droveAny = false;
        WithClothSoftBodyWrite(m_JoltSystem->GetBodyLockInterface(), it->second.m_BodyID,
                               [&](JPH::Body& body, JPH::SoftBodyMotionProperties& motion)
                               {
            // Particle positions/velocities are stored relative to the body's centre of
            // mass. For a cloth the COM frame carries no rotation (the body is created
            // with identity rotation and soft bodies only translate their COM), but we
            // convert through the transform anyway so the drive stays correct if that
            // ever changes: world position via comTransform, world->COM velocity via the
            // transposed (== inverse, orthonormal) rotation.
            const JPH::RMat44 comTransform = body.GetCenterOfMassTransform();
            const f32 invDt = 1.0f / dt;
            auto& vertices = motion.GetVertices();
            const u32 vertexCount = static_cast<u32>(vertices.size());

            for (sizet i = 0; i < vertexIndices.size(); ++i)
            {
                const u32 idx = vertexIndices[i];
                if (idx >= vertexCount)
                    continue;

                JPH::SoftBodyVertex& v = vertices[idx];
                // Only drive kinematic (pinned) particles — never perturb a free one.
                if (v.mInvMass != 0.0f)
                    continue;

                const glm::vec3& target = targetWorldPositions[i];
                if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z))
                    continue;

                const JPH::RVec3 currentWorld = comTransform * v.mPosition;
                const JPH::Vec3 worldVel(
                    (target.x - static_cast<f32>(currentWorld.GetX())) * invDt,
                    (target.y - static_cast<f32>(currentWorld.GetY())) * invDt,
                    (target.z - static_cast<f32>(currentWorld.GetZ())) * invDt);
                v.mVelocity = comTransform.Multiply3x3Transposed(worldVel);
                droveAny = true;
            } });

        // Activate outside the body lock (BodyInterface::ActivateBody takes its own locks):
        // a cloth that settled and slept must wake so the driven velocity is integrated.
        if (droveAny)
            m_JoltSystem->GetBodyInterface().ActivateBody(it->second.m_BodyID);
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
        // Vehicles likewise need their chassis bodies to exist first.
        CreateVehicles();
    }

    void JoltScene::OnRuntimeStop()
    {
        OLO_CORE_INFO("JoltScene stopping runtime");

        // Remove vehicles and constraints before the bodies they reference are
        // destroyed (vehicles also unregister their step listeners here).
        DestroyAllVehicles();
        DestroyAllConstraints();

        // Remove static terrain height-field bodies while m_JoltSystem is still alive.
        DestroyAllTerrainBodies();

        // Remove cloth soft bodies while m_JoltSystem is still alive.
        DestroyAllClothBodies();

        // Destroy all bodies — the JoltBody destructors run as clear() releases
        // each Ref; the reverse-lookup and sync sets are dropped with them.
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

    void JoltScene::ShiftOrigin(const glm::vec3& delta)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_JoltSystem)
        {
            return;
        }

        auto& bodyInterface = m_JoltSystem->GetBodyInterface();
        const JPH::RVec3 joltDelta = JoltUtils::ToJoltRVec3(delta);

        // Rigid bodies — dynamic, kinematic, static (static ground must move with
        // the world or dynamic bodies would fall through the shifted-away floor),
        // and ragdoll bone bodies (which are realised as ordinary Rigidbody3D
        // components, so they live in m_Bodies). BodyInterface::GetPosition and
        // SetPosition both address the body's ORIGIN (unlike JoltBody::GetPosition,
        // which returns the center of mass), so a plain delta shift is exact.
        // DontActivate keeps sleeping bodies asleep and leaves velocity untouched.
        for (auto& [id, body] : m_Bodies)
        {
            if (!body || body->GetBodyID().IsInvalid())
            {
                continue;
            }
            const JPH::BodyID bid = body->GetBodyID();
            bodyInterface.SetPosition(bid, bodyInterface.GetPosition(bid) + joltDelta, JPH::EActivation::DontActivate);
        }

        // Static terrain height-field bodies (raw JPH bodies, no JoltBody wrapper).
        for (auto& [id, bid] : m_TerrainBodies)
        {
            if (bid.IsInvalid())
            {
                continue;
            }
            bodyInterface.SetPosition(bid, bodyInterface.GetPosition(bid) + joltDelta, JPH::EActivation::DontActivate);
        }

        // Cloth soft bodies (issue #613). A JPH soft body IS a JPH::Body whose
        // particle positions are stored RELATIVE to the body's centre of mass
        // (SoftBodyVertex::mPosition is "relative to the center of mass of the
        // soft body"), so moving the body's origin translates every vertex by
        // exactly `delta` — the same SetPosition shift the rigid bodies use is
        // correct and uniform here. GetPosition/SetPosition are exact inverses
        // (both add/remove rotation * shape COM), so this is a clean += delta on
        // the body origin. The COM-relative representation is self-consistent
        // across the next step, so the shift persists. DontActivate leaves a
        // settled/slept cloth asleep and its velocities untouched.
        for (auto& [id, cloth] : m_Cloths)
        {
            if (cloth.m_BodyID.IsInvalid())
            {
                continue;
            }
            bodyInterface.SetPosition(cloth.m_BodyID, bodyInterface.GetPosition(cloth.m_BodyID) + joltDelta,
                                      JPH::EActivation::DontActivate);
        }

        // Character controllers (CharacterVirtual — tracked separately from the
        // rigid-body set). SetTranslation instantly moves the controller.
        for (auto& [id, controller] : m_CharacterControllers)
        {
            if (controller)
            {
                controller->SetTranslation(controller->GetTranslation() + delta);
            }
        }

        // World-anchored constraint anchors (issue #613). Two-body joints between
        // real bodies already translate together (both endpoints moved above), but
        // a constraint anchored to an ABSOLUTE world point — a pulley's fixed
        // pivots, or a single-body joint realised against JPH::Body::sFixedToWorld
        // — holds a world-space anchor that the body shift leaves behind, stretching
        // the joint. Translate those anchors by the same delta so the whole world
        // (including its "fixed" points) stays geometrically consistent. This is
        // what lets Scene::MaybeRebaseOrigin stop deferring the rebase.
        ShiftWorldAnchoredConstraints(delta);
    }

    void JoltScene::ShiftWorldAnchoredConstraints(const glm::vec3& delta)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_JoltSystem || m_Constraints.empty())
        {
            return;
        }

        // sFixedToWorld's body id self-matches whichever side of a world-anchored
        // TwoBodyConstraint is the fixed-world body (NotifyShapeChanged checks
        // body1 then body2 against it), so passing it always targets the world-side
        // local anchor. It is a reserved id no real body shares.
        const JPH::BodyID worldBodyID = JPH::Body::sFixedToWorld.GetID();
        const JPH::Vec3 joltDelta = JoltUtils::ToJoltVector(delta);

        // A pulley's fixed pivots have no runtime setter, so it must be rebuilt.
        // Collect the owners first — the rebuild mutates m_Constraints, so it can't
        // run inside this iteration.
        std::vector<UUID> pulleysToRebuild;

        for (auto& [id, constraint] : m_Constraints)
        {
            if (!constraint)
            {
                continue;
            }

            // Pulley: mFixedPoint1/2 are private absolute world points with no
            // setter, and NotifyShapeChanged only moves the body-attach points, not
            // the pivots. Defer to a destroy+rebuild from shifted settings.
            if (constraint->GetSubType() == JPH::EConstraintSubType::Pulley)
            {
                pulleysToRebuild.push_back(id);
                continue;
            }

            // Every other world-anchored joint (Fixed/Point/Distance/Hinge/Slider/
            // Cone/SwingTwist/SixDOF/Path) stores its world-side anchor as a local-
            // space position on the sFixedToWorld body (COM at origin → local ==
            // absolute). NotifyShapeChanged(worldBody, -delta) shifts exactly that
            // anchor by +delta; the body-side anchor already moved with its body, so
            // the joint's world frame ends up shifted by delta with its rest state,
            // motors and warm-start impulses intact — no solver snap (both anchors
            // moved by the same delta, so the constraint error is unchanged).
            if (constraint->GetType() == JPH::EConstraintType::TwoBodyConstraint)
            {
                const auto& tbc = static_cast<const JPH::TwoBodyConstraint&>(*constraint);
                if (tbc.GetBody1() == &JPH::Body::sFixedToWorld || tbc.GetBody2() == &JPH::Body::sFixedToWorld)
                {
                    constraint->NotifyShapeChanged(worldBodyID, -joltDelta);
                }
            }
        }

        for (UUID id : pulleysToRebuild)
        {
            // Missing-safe lookup: Scene::GetEntityByUUID asserts on a stale UUID,
            // so use the optional variant and skip if the entity is gone (or has
            // since lost its joint) rather than crashing.
            std::optional<Entity> entityOpt = m_Scene ? m_Scene->TryGetEntityWithUUID(id) : std::nullopt;
            if (!entityOpt || !entityOpt->HasComponent<PhysicsJoint3DComponent>())
            {
                continue;
            }
            Entity entity = *entityOpt;
            auto& joint = entity.GetComponent<PhysicsJoint3DComponent>();
            // The authored fixed pivots are absolute world points; shift them so the
            // rebuilt pulley pins to the same relative geometry. The two body-attach
            // points are re-derived from the (already-shifted) TransformComponents
            // inside CreateConstraint, so all four pulley points move by the same
            // delta and the [min,max] rope-length span is preserved.
            joint.m_PulleyFixedPointA += delta;
            joint.m_PulleyFixedPointB += delta;
            DestroyConstraint(entity);
            CreateConstraint(entity);
        }
    }

    bool JoltScene::HasWorldAnchoredConstraints() const
    {
        for (const auto& [id, constraint] : m_Constraints)
        {
            if (!constraint)
            {
                continue;
            }

            // A pulley's two pivot points (mFixedPoint1/2) are absolute world-
            // space points that do not move with either body.
            if (constraint->GetSubType() == JPH::EConstraintSubType::Pulley)
            {
                return true;
            }

            // A single-body joint is realised as a two-body constraint against
            // the shared fixed world body (JPH::Body::sFixedToWorld); its world-
            // side anchor is an absolute point that a body shift would leave
            // behind. Two-body joints between two real bodies are safe.
            if (constraint->GetType() == JPH::EConstraintType::TwoBodyConstraint)
            {
                const auto& tbc = static_cast<const JPH::TwoBodyConstraint&>(*constraint);
                if (tbc.GetBody1() == &JPH::Body::sFixedToWorld || tbc.GetBody2() == &JPH::Body::sFixedToWorld)
                {
                    return true;
                }
            }
        }
        return false;
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

    std::vector<std::pair<UUID, UUID>> JoltScene::GetActiveContactPairs() const
    {
        if (!m_ContactListener)
            return {};
        return m_ContactListener->GetActiveContactPairs();
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

        // Read back the accumulated position (linear) and rotation (angular)
        // constraint impulse magnitudes for the last solved step. The lambda
        // accessors are type-specific, so dispatch on the concrete sub-type. The
        // returned values are impulses (N·s / N·m·s); the caller divides by the
        // step dt to get an average force/torque. Returns false for sub-types we
        // don't monitor (none of ours, but keeps the switch total and safe if a
        // new joint type is added without a break mapping). For each type the
        // position impulse aggregates every locked translational part and the
        // rotation impulse every locked angular part, so the magnitude reflects
        // the full load the joint carries — not just one axis.
        bool ReadConstraintImpulses(const JPH::Constraint& constraint, f32& outLinearImpulse, f32& outAngularImpulse)
        {
            switch (constraint.GetSubType())
            {
                case JPH::EConstraintSubType::Fixed:
                {
                    const auto& c = static_cast<const JPH::FixedConstraint&>(constraint);
                    outLinearImpulse = c.GetTotalLambdaPosition().Length();
                    outAngularImpulse = c.GetTotalLambdaRotation().Length();
                    return true;
                }
                case JPH::EConstraintSubType::Point:
                {
                    const auto& c = static_cast<const JPH::PointConstraint&>(constraint);
                    outLinearImpulse = c.GetTotalLambdaPosition().Length();
                    outAngularImpulse = 0.0f; // rotation is unconstrained
                    return true;
                }
                case JPH::EConstraintSubType::Distance:
                {
                    const auto& c = static_cast<const JPH::DistanceConstraint&>(constraint);
                    outLinearImpulse = std::abs(c.GetTotalLambdaPosition()); // scalar along the axis
                    outAngularImpulse = 0.0f;
                    return true;
                }
                case JPH::EConstraintSubType::Hinge:
                {
                    const auto& c = static_cast<const JPH::HingeConstraint&>(constraint);
                    outLinearImpulse = c.GetTotalLambdaPosition().Length();
                    // Two locked rotational DOF + the angle-limit reaction.
                    outAngularImpulse = c.GetTotalLambdaRotation().Length() + std::abs(c.GetTotalLambdaRotationLimits());
                    return true;
                }
                case JPH::EConstraintSubType::Slider:
                {
                    const auto& c = static_cast<const JPH::SliderConstraint&>(constraint);
                    // Two locked perpendicular axes + the translation-limit reaction.
                    outLinearImpulse = c.GetTotalLambdaPosition().Length() + std::abs(c.GetTotalLambdaPositionLimits());
                    outAngularImpulse = c.GetTotalLambdaRotation().Length();
                    return true;
                }
                case JPH::EConstraintSubType::Cone:
                {
                    const auto& c = static_cast<const JPH::ConeConstraint&>(constraint);
                    outLinearImpulse = c.GetTotalLambdaPosition().Length();
                    outAngularImpulse = std::abs(c.GetTotalLambdaRotation()); // cone half-angle limit
                    return true;
                }
                case JPH::EConstraintSubType::SwingTwist:
                {
                    const auto& c = static_cast<const JPH::SwingTwistConstraint&>(constraint);
                    outLinearImpulse = c.GetTotalLambdaPosition().Length();
                    // Twist + two swing-limit reactions about the constraint axes.
                    const JPH::Vec3 angular(c.GetTotalLambdaTwist(), c.GetTotalLambdaSwingY(), c.GetTotalLambdaSwingZ());
                    outAngularImpulse = angular.Length();
                    return true;
                }
                case JPH::EConstraintSubType::SixDOF:
                {
                    const auto& c = static_cast<const JPH::SixDOFConstraint&>(constraint);
                    outLinearImpulse = c.GetTotalLambdaPosition().Length();
                    outAngularImpulse = c.GetTotalLambdaRotation().Length();
                    return true;
                }
                case JPH::EConstraintSubType::Pulley:
                {
                    const auto& c = static_cast<const JPH::PulleyConstraint&>(constraint);
                    // A single scalar impulse along the rope — a genuine linear load.
                    outLinearImpulse = std::abs(c.GetTotalLambdaPosition());
                    outAngularImpulse = 0.0f;
                    return true;
                }
                case JPH::EConstraintSubType::Gear:
                {
                    const auto& c = static_cast<const JPH::GearConstraint&>(constraint);
                    // The gear-coupling impulse is angular (it relates the two
                    // bodies' rotation rates); map it to the torque threshold.
                    outLinearImpulse = 0.0f;
                    outAngularImpulse = std::abs(c.GetTotalLambda());
                    return true;
                }
                case JPH::EConstraintSubType::RackAndPinion:
                {
                    const auto& c = static_cast<const JPH::RackAndPinionConstraint&>(constraint);
                    // The rack-and-pinion coupling impulse mixes the pinion's
                    // rotation and the rack's translation; surface it on the
                    // torque axis (best-effort — these units are not a pure N·m).
                    outLinearImpulse = 0.0f;
                    outAngularImpulse = std::abs(c.GetTotalLambda());
                    return true;
                }
                case JPH::EConstraintSubType::Path:
                {
                    const auto& c = static_cast<const JPH::PathConstraint&>(constraint);
                    // Linear load: the two perpendicular axes that hold the body on
                    // the path, plus the path-end limit reaction, plus the
                    // motor/friction impulse along the tangent — all genuine forces.
                    outLinearImpulse = c.GetTotalLambdaPosition().Length() + std::abs(c.GetTotalLambdaPositionLimits()) + std::abs(c.GetTotalLambdaMotor());
                    // Angular load: the hinge + rotation parts that constrain the
                    // body's orientation to the path (per the rotation mode).
                    outAngularImpulse = c.GetTotalLambdaRotationHinge().Length() + c.GetTotalLambdaRotation().Length();
                    return true;
                }
                default:
                    return false;
            }
        }

        // Clamp an authored, possibly non-finite motor/friction magnitude (a max
        // torque, max force, or friction limit) to a safe, non-negative range. A
        // negative or non-finite authored value collapses to 0 ("no authority" for
        // a motor limit, "no friction" for a friction limit); the upper clamp keeps
        // a fat-fingered field from feeding an absurd impulse into the solver.
        f32 SanitizeMotorMagnitude(f32 value)
        {
            if (!std::isfinite(value) || value < 0.0f)
                return 0.0f;
            return std::min(value, 1.0e9f);
        }

        // A finite-or-zero pass-through for a motor target (velocity / angle /
        // position). Targets are signed, so we only reject non-finite values.
        f32 SanitizeMotorTarget(f32 value)
        {
            return std::isfinite(value) ? value : 0.0f;
        }

        // Convert an authored joint-limit angle (degrees) to radians clamped to
        // [loRad, hiRad], substituting fallbackDeg for a non-finite authored
        // value. NaN/Inf must never reach a Jolt constraint:
        // SwingTwistConstraintPart::SetLimits asserts on it (Debug) and the
        // solver would otherwise propagate it across the whole island (Release).
        // std::clamp alone does not filter NaN (it compares false both ways), so
        // the finite check is explicit. Authoring boundaries (serializers, Lua)
        // already validate, but a C# script can write a raw field, so this is the
        // last line of defence before Jolt.
        f32 SanitizeJointAngleDeg(f32 degrees, f32 loRad, f32 hiRad, f32 fallbackDeg)
        {
            const f32 radians = JoltUtils::DegreesToRadians(std::isfinite(degrees) ? degrees : fallbackDeg);
            return std::clamp(radians, loRad, hiRad);
        }

        // A pulley length bound. A negative value is Jolt's "auto" sentinel
        // (use the segment length at creation time), so the only clamp is to
        // reject non-finite and absurdly large magnitudes; -1 is preserved.
        f32 SanitizePulleyLength(f32 value, f32 fallback)
        {
            if (!std::isfinite(value))
                return fallback;
            return std::clamp(value, -1.0f, 1.0e9f);
        }

        // A gear / rack-and-pinion / pulley ratio. A non-finite or (for the gear
        // coupling) effectively-zero ratio collapses to 1.0 so the joint still
        // builds and couples; the sign is preserved (a negative gear ratio is a
        // valid reversed coupling). The magnitude is clamped to a sane range.
        f32 SanitizeConstraintRatio(f32 value)
        {
            if (!std::isfinite(value) || std::abs(value) < 1.0e-6f)
                return 1.0f;
            return std::clamp(value, -1.0e9f, 1.0e9f);
        }

        // Build the limit-spring settings for a hinge/slider from the authored
        // frequency (Hz) + damping ratio. A frequency <= 0 (after sanitizing)
        // returns Jolt's default hard-limit spring, so the constraint behaves
        // exactly as before the soft-limit fields existed.
        JPH::SpringSettings MakeLimitSpring(f32 frequency, f32 damping)
        {
            return { JPH::ESpringMode::FrequencyAndDamping,
                     SanitizeMotorMagnitude(frequency),
                     SanitizeMotorMagnitude(damping) };
        }

        // A strictly-positive vehicle dimension (wheel radius/width, suspension
        // length, spring frequency, engine/brake torque). A non-finite or
        // non-positive authored value collapses to the type's sensible default so
        // a fat-fingered field can never feed a degenerate vehicle into Jolt
        // (a zero radius / zero-length suspension asserts in Debug and NaNs the
        // solver in Release). The upper clamp keeps an absurd value bounded.
        f32 SanitizeVehiclePositive(f32 value, f32 fallback)
        {
            if (!std::isfinite(value) || value <= 0.0f)
                return fallback;
            return std::min(value, 1.0e6f);
        }

        // Put a freshly-created hinge constraint into the motor state authored on
        // the component. Velocity drives toward the target angular velocity (the
        // component stores deg/s; Jolt wants rad/s); Position drives toward the
        // target angle (deg → rad, Jolt clamps to the hinge limits internally).
        // Off leaves the motor disabled — any m_HingeMaxFrictionTorque set on the
        // settings still resists rotation. The motor torque limit was already
        // applied to s.mMotorSettings before Create().
        void ConfigureHingeMotor(JPH::HingeConstraint& hinge, const PhysicsJoint3DComponent& joint)
        {
            switch (joint.m_HingeMotorMode)
            {
                case JointMotorMode::Velocity:
                    hinge.SetMotorState(JPH::EMotorState::Velocity);
                    hinge.SetTargetAngularVelocity(JoltUtils::DegreesToRadians(SanitizeMotorTarget(joint.m_HingeMotorTargetVelocityDeg)));
                    break;
                case JointMotorMode::Position:
                    hinge.SetMotorState(JPH::EMotorState::Position);
                    hinge.SetTargetAngle(JoltUtils::DegreesToRadians(SanitizeMotorTarget(joint.m_HingeMotorTargetAngleDeg)));
                    break;
                case JointMotorMode::Off:
                default:
                    break; // motor off; friction (if any) still applies
            }
        }

        // Slider counterpart of ConfigureHingeMotor — drives along the slide axis.
        // Velocity targets m/s, Position targets metres (Jolt clamps to the slide
        // limits internally). The motor force limit was already applied to
        // s.mMotorSettings before Create().
        void ConfigureSliderMotor(JPH::SliderConstraint& slider, const PhysicsJoint3DComponent& joint)
        {
            switch (joint.m_SliderMotorMode)
            {
                case JointMotorMode::Velocity:
                    slider.SetMotorState(JPH::EMotorState::Velocity);
                    slider.SetTargetVelocity(SanitizeMotorTarget(joint.m_SliderMotorTargetVelocity));
                    break;
                case JointMotorMode::Position:
                    slider.SetMotorState(JPH::EMotorState::Position);
                    slider.SetTargetPosition(SanitizeMotorTarget(joint.m_SliderMotorTargetPosition));
                    break;
                case JointMotorMode::Off:
                default:
                    break; // motor off; friction (if any) still applies
            }
        }

        // Map the authored path-rotation mode onto Jolt's enum. The values match
        // 1:1 (JointPathRotationMode mirrors EPathRotationConstraintType); an
        // out-of-range value falls back to Free (rotation unconstrained).
        [[nodiscard("mapped Jolt path-rotation enum must be used")]] JPH::EPathRotationConstraintType ToJoltPathRotation(JointPathRotationMode mode)
        {
            switch (mode)
            {
                case JointPathRotationMode::ConstrainAroundTangent:
                    return JPH::EPathRotationConstraintType::ConstrainAroundTangent;
                case JointPathRotationMode::ConstrainAroundNormal:
                    return JPH::EPathRotationConstraintType::ConstrainAroundNormal;
                case JointPathRotationMode::ConstrainAroundBinormal:
                    return JPH::EPathRotationConstraintType::ConstrainAroundBinormal;
                case JointPathRotationMode::ConstrainToPath:
                    return JPH::EPathRotationConstraintType::ConstrainToPath;
                case JointPathRotationMode::FullyConstrained:
                    return JPH::EPathRotationConstraintType::FullyConstrained;
                case JointPathRotationMode::Free:
                default:
                    return JPH::EPathRotationConstraintType::Free;
            }
        }

        // Build a Jolt Hermite path from the authored control points. Returns
        // nullptr when there are too few usable points (Jolt needs at least one
        // segment) or any point is non-finite — the caller then logs and skips
        // the constraint.
        //
        // Tangents are central differences (a Catmull-Rom spline through the
        // points), one-sided at the ends of a non-looping path and wrapped for a
        // looping one. Each point's normal is derived perpendicular to its
        // tangent so Jolt's binormal = normal x tangent never degenerates (the
        // Free rotation mode ignores the normal, but the ConstrainAround* / ToPath
        // modes rely on it). `looping` is honoured only with >= 3 distinct points;
        // a 2-point loop has a zero central-difference tangent, so it is treated
        // as a straight (non-looping) path.
        JPH::Ref<JPH::PathConstraintPathHermite> BuildHermitePath(const std::vector<glm::vec3>& points, bool looping)
        {
            const sizet n = points.size();
            if (n < 2)
                return nullptr;
            for (const glm::vec3& p : points)
            {
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                    return nullptr;
            }

            // A looping path needs a distinct first/last point (Jolt asserts they
            // differ) and at least one interior point for the wrapped central
            // differences to be non-zero.
            const glm::vec3 firstToLast = points.front() - points.back();
            const bool loop = looping && n >= 3 && glm::dot(firstToLast, firstToLast) > 1.0e-12f;

            JPH::Ref<JPH::PathConstraintPathHermite> path = new JPH::PathConstraintPathHermite();
            path->SetIsLooping(loop);

            for (sizet i = 0; i < n; ++i)
            {
                glm::vec3 prev;
                glm::vec3 next;
                if (loop)
                {
                    prev = points[(i + n - 1) % n];
                    next = points[(i + 1) % n];
                }
                else
                {
                    prev = points[i == 0 ? 0 : i - 1];
                    next = points[i + 1 >= n ? n - 1 : i + 1];
                }

                glm::vec3 tangent = 0.5f * (next - prev);
                // A coincident neighbour pair yields a zero tangent (a degenerate
                // Hermite segment); fall back to the segment direction, then +X.
                if (glm::dot(tangent, tangent) < 1.0e-12f)
                {
                    tangent = next - points[i];
                    if (glm::dot(tangent, tangent) < 1.0e-12f)
                        tangent = points[i] - prev;
                    if (glm::dot(tangent, tangent) < 1.0e-12f)
                        tangent = glm::vec3(1.0f, 0.0f, 0.0f);
                }

                // Derive a normal perpendicular to the tangent. Use world up
                // unless the tangent is near-vertical, then fall back to world +X.
                const glm::vec3 tdir = glm::normalize(tangent);
                const glm::vec3 ref = (std::abs(tdir.y) > 0.99f) ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
                glm::vec3 binormal = glm::cross(ref, tdir);
                if (glm::dot(binormal, binormal) < 1.0e-12f)
                    binormal = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), tdir);
                binormal = glm::normalize(binormal);
                const glm::vec3 normal = glm::normalize(glm::cross(tdir, binormal));

                path->AddPoint(JoltUtils::ToJoltVector(points[i]),
                               JoltUtils::ToJoltVector(tangent),
                               JoltUtils::ToJoltVector(normal));
            }
            return path;
        }

        // Put a freshly-created path constraint into the motor state authored on
        // the component. The position motor drives body 2 ALONG the path:
        // Velocity targets m/s along the path tangent; Position targets a path
        // fraction (clamped to the valid [0, max] domain for a non-looping path,
        // since Jolt asserts on an out-of-range fraction; a looping path accepts
        // any fraction and wraps). Off leaves the motor disabled — any max
        // friction force set on the settings still resists motion. The motor
        // force limit was already applied to s.mPositionMotorSettings before
        // Create().
        void ConfigurePathMotor(JPH::PathConstraint& path, const PhysicsJoint3DComponent& joint)
        {
            switch (joint.m_PathMotorMode)
            {
                case JointMotorMode::Velocity:
                    path.SetPositionMotorState(JPH::EMotorState::Velocity);
                    path.SetTargetVelocity(SanitizeMotorTarget(joint.m_PathMotorTargetVelocity));
                    break;
                case JointMotorMode::Position:
                {
                    path.SetPositionMotorState(JPH::EMotorState::Position);
                    f32 target = SanitizeMotorTarget(joint.m_PathMotorTargetFraction);
                    if (const JPH::PathConstraintPath* p = path.GetPath(); p != nullptr && !p->IsLooping())
                        target = std::clamp(target, 0.0f, p->GetPathMaxFraction());
                    path.SetTargetPathFraction(target);
                    break;
                }
                case JointMotorMode::Off:
                default:
                    break; // motor off; friction (if any) still applies
            }
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

        // Every body and joint exists now, so the no-collide pairs can be filtered.
        ApplyJointCollisionFilters();

        OLO_CORE_INFO("Created {0} physics constraints", m_Constraints.size());
    }

    void JoltScene::ApplyJointCollisionFilters()
    {
        if (!m_JoltSystem || !m_Scene)
            return;

        JPH::BodyInterface& bodyInterface = m_JoltSystem->GetBodyInterface();

        // Reset every body a previous pass placed in the joint collision group
        // back to the default (no group, no filter), so an edited/removed joint
        // stops filtering. A body whose entity is gone is simply skipped. On the
        // first call the set is empty, so this is a no-op.
        for (UUID bodyEntityID : m_JointCollisionBodies)
        {
            if (Ref<JoltBody> body = GetBodyByEntityID(bodyEntityID); body && body->IsValid())
                bodyInterface.SetCollisionGroup(body->GetBodyID(), JPH::CollisionGroup());
        }
        m_JointCollisionBodies.clear();
        m_JointGroupFilter = nullptr;

        // Collect every no-collide joint that connects two real, distinct bodies.
        // World anchors (m_ConnectedEntity == 0) and self-joints have no second
        // body to filter against, so they never contribute a pair.
        struct NoCollidePair
        {
            UUID BodyA;
            UUID BodyB;
        };
        std::vector<NoCollidePair> pairs;
        std::unordered_map<UUID, JPH::CollisionGroup::SubGroupID> subGroupOf;

        auto view = m_Scene->GetAllEntitiesWith<PhysicsJoint3DComponent>();
        for (auto entityID : view)
        {
            Entity entity{ entityID, m_Scene };
            const auto& joint = entity.GetComponent<PhysicsJoint3DComponent>();
            if (joint.m_CollideConnected)
                continue;

            const UUID ownerID = entity.GetUUID();
            const UUID otherID = joint.m_ConnectedEntity;
            if (static_cast<u64>(otherID) == 0 || otherID == ownerID)
                continue; // world anchor / self-joint: no second body to filter

            if (Ref<JoltBody> bodyA = GetBodyByEntityID(ownerID), bodyB = GetBodyByEntityID(otherID);
                !bodyA || !bodyA->IsValid() || !bodyB || !bodyB->IsValid())
                continue; // an endpoint has no live body — nothing to filter

            // Assign each participating body a stable sub-group id the first time
            // it is seen, so a body shared by several joints keeps one sub-group
            // (and the table below encodes all of its no-collide partners). IDs
            // stay contiguous in [0, N) — try_emplace is a no-op when present.
            subGroupOf.try_emplace(ownerID, static_cast<JPH::CollisionGroup::SubGroupID>(subGroupOf.size()));
            subGroupOf.try_emplace(otherID, static_cast<JPH::CollisionGroup::SubGroupID>(subGroupOf.size()));
            pairs.push_back({ ownerID, otherID });
        }

        if (pairs.empty())
            return; // nothing opts out — every body keeps the default collide-all group

        // One shared table sized to the participant count. Its constructor enables
        // every pair; we then disable exactly the authored no-collide pairs, so a
        // body in two joints (chain A-B-C) stops colliding with each direct partner
        // (A-B, B-C) but still collides with the indirect one (A-C) — pairwise, not
        // whole-group, disabling.
        const auto subGroupCount = static_cast<JPH::uint>(subGroupOf.size());
        JPH::Ref<JPH::GroupFilterTable> filter = new JPH::GroupFilterTable(subGroupCount);
        for (const NoCollidePair& pair : pairs)
            filter->DisableCollision(subGroupOf[pair.BodyA], subGroupOf[pair.BodyB]);

        // Place every participant in the shared group with its sub-group id, behind
        // the shared filter, and record it so the next rebuild can reset it.
        for (const auto& [bodyEntityID, subGroupID] : subGroupOf)
        {
            if (Ref<JoltBody> body = GetBodyByEntityID(bodyEntityID); body && body->IsValid())
            {
                bodyInterface.SetCollisionGroup(body->GetBodyID(),
                                                JPH::CollisionGroup(filter.GetPtr(), s_JointCollisionGroupID, subGroupID));
                m_JointCollisionBodies.insert(bodyEntityID);
            }
        }

        m_JointGroupFilter = filter;
    }

    bool JoltScene::CreateConstraint(Entity entity)
    {
        if (!m_JoltSystem || !entity || !entity.HasComponent<PhysicsJoint3DComponent>())
            return false;

        auto& joint = entity.GetComponent<PhysicsJoint3DComponent>();
        UUID entityID = entity.GetUUID();

        // Idempotent — already built for this entity.
        if (m_Constraints.contains(entityID))
            return true;

        // The joint owner must have a physics body to be a constraint endpoint.
        Ref<JoltBody> bodyA = GetBodyByEntityID(entityID);
        if (!bodyA || !bodyA->IsValid() || !entity.HasComponent<TransformComponent>())
        {
            OLO_CORE_WARN("PhysicsJoint3D on entity {0} has no valid rigidbody; skipping constraint", (u64)entityID);
            return false;
        }

        const bool connectToWorld = (static_cast<u64>(joint.m_ConnectedEntity) == 0);

        // Gear / RackAndPinion couple two real bodies' motion rates — a world
        // (infinite-mass) anchor would just lock this body's rate to zero, which
        // is never what a designer means. Require a connected body for them.
        if (connectToWorld && (joint.m_Type == JointType3D::Gear || joint.m_Type == JointType3D::RackAndPinion))
        {
            OLO_CORE_WARN("PhysicsJoint3D on entity {0}: Gear/RackAndPinion needs a connected body, not a world anchor; skipping constraint", (u64)entityID);
            return false;
        }

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
        // The connected body's primary axis in world space (Gear / RackAndPinion).
        // Mirrors how `axis` is m_Axis rotated into world by this body's rotation;
        // for a world anchor (rejected above for those types) the identity rotation
        // leaves it as authored.
        glm::vec3 connectedAxis = joint.m_ConnectedAxis;
        if (connectToWorld)
        {
            // Pin body A's anchor at its current world location.
            worldB = worldA;
        }
        else
        {
            const auto& tcB = connectedEntity.GetComponent<TransformComponent>();
            worldB = tcB.Translation + tcB.GetRotation() * joint.m_LocalAnchorB;
            connectedAxis = tcB.GetRotation() * joint.m_ConnectedAxis;
        }
        connectedAxis = (glm::dot(connectedAxis, connectedAxis) < 1e-12f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::normalize(connectedAxis);

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
                    s.mHingeAxis1 = jAxis;
                    s.mHingeAxis2 = jAxis;
                    s.mNormalAxis1 = jNormal;
                    s.mNormalAxis2 = jNormal;
                    s.mLimitsMin = std::clamp(JoltUtils::DegreesToRadians(joint.m_HingeMinAngleDeg), -glm::pi<f32>(), 0.0f);
                    s.mLimitsMax = std::clamp(JoltUtils::DegreesToRadians(joint.m_HingeMaxAngleDeg), 0.0f, glm::pi<f32>());
                    // Friction torque (used only when the motor is Off) and the
                    // motor torque authority. Configure these on the settings
                    // before Create(); the motor *state* is set on the instance.
                    s.mMaxFrictionTorque = SanitizeMotorMagnitude(joint.m_HingeMaxFrictionTorque);
                    s.mMotorSettings.SetTorqueLimit(SanitizeMotorMagnitude(joint.m_HingeMaxMotorTorque));
                    // Soft (springy) angle limits — a frequency of 0 keeps them hard.
                    s.mLimitsSpringSettings = MakeLimitSpring(joint.m_HingeLimitSpringFrequency, joint.m_HingeLimitSpringDamping);
                    auto* hinge = static_cast<JPH::HingeConstraint*>(s.Create(body1, body2));
                    ConfigureHingeMotor(*hinge, joint);
                    return hinge;
                }
                case JointType3D::Slider:
                {
                    JPH::SliderConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mAutoDetectPoint = false;
                    s.mPoint1 = p1;
                    s.mPoint2 = p2;
                    s.mSliderAxis1 = jAxis;
                    s.mSliderAxis2 = jAxis;
                    s.mNormalAxis1 = jNormal;
                    s.mNormalAxis2 = jNormal;
                    s.mLimitsMin = joint.m_SliderMinLimit;
                    s.mLimitsMax = joint.m_SliderMaxLimit;
                    // Friction force (used only when the motor is Off) and the
                    // motor force authority. A linear motor uses the force limits,
                    // not the torque limits (see Jolt MotorSettings).
                    s.mMaxFrictionForce = SanitizeMotorMagnitude(joint.m_SliderMaxFrictionForce);
                    s.mMotorSettings.SetForceLimit(SanitizeMotorMagnitude(joint.m_SliderMaxMotorForce));
                    // Soft (springy) translation limits — a frequency of 0 keeps them hard.
                    s.mLimitsSpringSettings = MakeLimitSpring(joint.m_SliderLimitSpringFrequency, joint.m_SliderLimitSpringDamping);
                    auto* slider = static_cast<JPH::SliderConstraint*>(s.Create(body1, body2));
                    ConfigureSliderMotor(*slider, joint);
                    return slider;
                }
                case JointType3D::Cone:
                {
                    JPH::ConeConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mPoint1 = p1;
                    s.mPoint2 = p2;
                    s.mTwistAxis1 = jAxis;
                    s.mTwistAxis2 = jAxis;
                    s.mHalfConeAngle = std::clamp(JoltUtils::DegreesToRadians(joint.m_ConeHalfAngleDeg), 0.0f, glm::pi<f32>());
                    return s.Create(body1, body2);
                }
                case JointType3D::SwingTwist:
                {
                    // Ragdoll cone + twist. Twist axis = jAxis, plane axis =
                    // jNormal (perpendicular to jAxis by construction). The two
                    // swing half-cone angles clamp to [0, pi]; the twist range
                    // clamps to [-pi, pi] (Jolt's valid twist domain).
                    JPH::SwingTwistConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mPosition1 = p1;
                    s.mPosition2 = p2;
                    s.mTwistAxis1 = jAxis;
                    s.mTwistAxis2 = jAxis;
                    s.mPlaneAxis1 = jNormal;
                    s.mPlaneAxis2 = jNormal;
                    s.mNormalHalfConeAngle = SanitizeJointAngleDeg(joint.m_SwingNormalHalfAngleDeg, 0.0f, glm::pi<f32>(), 45.0f);
                    s.mPlaneHalfConeAngle = SanitizeJointAngleDeg(joint.m_SwingPlaneHalfAngleDeg, 0.0f, glm::pi<f32>(), 45.0f);
                    f32 twistMin = SanitizeJointAngleDeg(joint.m_TwistMinAngleDeg, -glm::pi<f32>(), glm::pi<f32>(), -45.0f);
                    f32 twistMax = SanitizeJointAngleDeg(joint.m_TwistMaxAngleDeg, -glm::pi<f32>(), glm::pi<f32>(), 45.0f);
                    // Jolt asserts mTwistMinAngle <= mTwistMaxAngle; normalise an
                    // inverted authored range so it stays a valid (non-empty) twist
                    // span (mirrors the SixDOF Limited-axis path below).
                    if (twistMin > twistMax)
                        std::swap(twistMin, twistMax);
                    s.mTwistMinAngle = twistMin;
                    s.mTwistMaxAngle = twistMax;
                    return s.Create(body1, body2);
                }
                case JointType3D::SixDOF:
                {
                    // Fully configurable per-axis constraint. The frame's X axis
                    // is jAxis, Y is jNormal (Z is derived by Jolt). Each DOF maps
                    // to a free / fixed / limited Jolt axis. Pyramid swing lets the
                    // two rotation-limit axes (Y, Z) be asymmetric in [-pi, pi];
                    // the twist (X) rotation limit is asymmetric in [-pi, pi] too.
                    using ESix = JPH::SixDOFConstraintSettings::EAxis;
                    JPH::SixDOFConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mPosition1 = p1;
                    s.mPosition2 = p2;
                    s.mAxisX1 = jAxis;
                    s.mAxisX2 = jAxis;
                    s.mAxisY1 = jNormal;
                    s.mAxisY2 = jNormal;
                    s.mSwingType = JPH::ESwingType::Pyramid;

                    const auto applyAxis = [&s](ESix ax, JointAxisMode mode, f32 lo, f32 hi)
                    {
                        switch (mode)
                        {
                            case JointAxisMode::Free:
                                s.MakeFreeAxis(ax);
                                break;
                            case JointAxisMode::Limited:
                                // NaN/Inf must never reach Jolt; collapse a
                                // non-finite bound to 0 (Jolt then treats the axis
                                // as fixed) rather than feeding garbage in.
                                if (!std::isfinite(lo))
                                    lo = 0.0f;
                                if (!std::isfinite(hi))
                                    hi = 0.0f;
                                // An inverted range would make Jolt fix the axis;
                                // normalise it so a Limited axis stays limited.
                                if (hi < lo)
                                    std::swap(lo, hi);
                                s.SetLimitedAxis(ax, lo, hi);
                                break;
                            case JointAxisMode::Locked:
                            default:
                                s.MakeFixedAxis(ax);
                                break;
                        }
                    };

                    const auto clampAngle = [](f32 radians)
                    {
                        return std::clamp(radians, -glm::pi<f32>(), glm::pi<f32>());
                    };

                    applyAxis(ESix::TranslationX, joint.m_SixDOFTransXMode, joint.m_SixDOFTranslationMin.x, joint.m_SixDOFTranslationMax.x);
                    applyAxis(ESix::TranslationY, joint.m_SixDOFTransYMode, joint.m_SixDOFTranslationMin.y, joint.m_SixDOFTranslationMax.y);
                    applyAxis(ESix::TranslationZ, joint.m_SixDOFTransZMode, joint.m_SixDOFTranslationMin.z, joint.m_SixDOFTranslationMax.z);
                    applyAxis(ESix::RotationX, joint.m_SixDOFRotXMode, clampAngle(JoltUtils::DegreesToRadians(joint.m_SixDOFRotationMinDeg.x)), clampAngle(JoltUtils::DegreesToRadians(joint.m_SixDOFRotationMaxDeg.x)));
                    applyAxis(ESix::RotationY, joint.m_SixDOFRotYMode, clampAngle(JoltUtils::DegreesToRadians(joint.m_SixDOFRotationMinDeg.y)), clampAngle(JoltUtils::DegreesToRadians(joint.m_SixDOFRotationMaxDeg.y)));
                    applyAxis(ESix::RotationZ, joint.m_SixDOFRotZMode, clampAngle(JoltUtils::DegreesToRadians(joint.m_SixDOFRotationMinDeg.z)), clampAngle(JoltUtils::DegreesToRadians(joint.m_SixDOFRotationMaxDeg.z)));
                    return s.Create(body1, body2);
                }
                case JointType3D::Pulley:
                {
                    // Two bodies suspended over two fixed world points. body1 is
                    // the connected/world body (anchor p1 = worldB, fixed point B),
                    // body2 is this body (anchor p2 = worldA, fixed point A). The
                    // fixed points are authored in world space, so pass them through
                    // directly. A negative min/max is Jolt's "auto from current
                    // length" sentinel; if both are positive but inverted, swap so
                    // the [min,max] span stays non-empty.
                    JPH::PulleyConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mBodyPoint1 = p1;
                    s.mBodyPoint2 = p2;
                    s.mFixedPoint1 = JoltUtils::ToJoltRVec3(joint.m_PulleyFixedPointB);
                    s.mFixedPoint2 = JoltUtils::ToJoltRVec3(joint.m_PulleyFixedPointA);
                    // The pulley ratio is a length multiplier, so it must be
                    // non-negative — the shared SanitizeConstraintRatio passes
                    // through the negative values a gear coupling may legitimately
                    // use, so clamp it to [0, 1e9] here.
                    s.mRatio = std::isfinite(joint.m_PulleyRatio) ? std::clamp(joint.m_PulleyRatio, 0.0f, 1.0e9f) : 1.0f;
                    f32 minLen = SanitizePulleyLength(joint.m_PulleyMinLength, 0.0f);
                    f32 maxLen = SanitizePulleyLength(joint.m_PulleyMaxLength, -1.0f);
                    if (minLen >= 0.0f && maxLen >= 0.0f && minLen > maxLen)
                        std::swap(minLen, maxLen);
                    s.mMinLength = minLen;
                    s.mMaxLength = maxLen;
                    return s.Create(body1, body2);
                }
                case JointType3D::Gear:
                {
                    // Couple the two bodies' rotation about their hinge axes.
                    // body1 = connected body (axis = connectedAxis), body2 = this
                    // body (axis = jAxis). Jolt: body1Rot = -ratio * body2Rot.
                    // No SetConstraints() — see the v1 note in Components.h.
                    JPH::GearConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mHingeAxis1 = JoltUtils::ToJoltVector(connectedAxis);
                    s.mHingeAxis2 = jAxis;
                    s.mRatio = SanitizeConstraintRatio(joint.m_GearRatio);
                    return s.Create(body1, body2);
                }
                case JointType3D::RackAndPinion:
                {
                    // body1 = connected body = the pinion (rotates about its hinge
                    // axis connectedAxis); body2 = this body = the rack (slides
                    // along jAxis). Jolt: pinionRotation = ratio * rackTranslation.
                    JPH::RackAndPinionConstraintSettings s;
                    s.mSpace = JPH::EConstraintSpace::WorldSpace;
                    s.mHingeAxis = JoltUtils::ToJoltVector(connectedAxis);
                    s.mSliderAxis = jAxis;
                    s.mRatio = SanitizeConstraintRatio(joint.m_GearRatio);
                    return s.Create(body1, body2);
                }
                case JointType3D::Path:
                {
                    // body1 = connected/world body, body2 = this body. The
                    // Hermite path is authored in body1's local frame (world frame
                    // for a world anchor), so mPathPosition places the path origin
                    // at body1's local anchor (m_LocalAnchorB) with no extra
                    // rotation; Jolt converts to body1's COM space internally.
                    // body2 is pulled onto the path and may be driven along it by
                    // the position motor. Returns nullptr (skips the constraint)
                    // when the authored points can't form a valid path.
                    JPH::Ref<JPH::PathConstraintPathHermite> path = BuildHermitePath(joint.m_PathPoints, joint.m_PathIsLooping);
                    if (path == nullptr)
                    {
                        OLO_CORE_WARN("PhysicsJoint3D on entity {0}: Path joint needs >= 2 finite points; skipping constraint", (u64)entityID);
                        return nullptr;
                    }
                    JPH::PathConstraintSettings s;
                    s.mPath = path;
                    s.mPathPosition = JoltUtils::ToJoltVector(joint.m_LocalAnchorB);
                    s.mPathRotation = JPH::Quat::sIdentity();
                    s.mPathFraction = 0.0f;
                    // Friction force (used only when the motor is Off) and the
                    // along-path motor force authority. Both clamp non-finite /
                    // negative authored values to 0 ("no friction" / "no authority").
                    s.mMaxFrictionForce = SanitizeMotorMagnitude(joint.m_PathMaxFrictionForce);
                    s.mPositionMotorSettings.SetForceLimit(SanitizeMotorMagnitude(joint.m_PathMaxMotorForce));
                    s.mRotationConstraintType = ToJoltPathRotation(joint.m_PathRotationMode);
                    auto* pathConstraint = static_cast<JPH::PathConstraint*>(s.Create(body1, body2));
                    ConfigurePathMotor(*pathConstraint, joint);
                    return pathConstraint;
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
            for (const auto& [entityID, constraint] : m_Constraints)
            {
                if (constraint != nullptr)
                    m_JoltSystem->RemoveConstraint(constraint);
            }
        }
        m_Constraints.clear();

        // The bodies these joints filtered are being torn down too, so drop the
        // shared group filter and the tracked-body set. (The bodies need no group
        // reset — they are about to be destroyed.)
        m_JointGroupFilter = nullptr;
        m_JointCollisionBodies.clear();
    }

    void JoltScene::CreateVehicles()
    {
        if (!m_Scene)
            return;

        // Build every authored vehicle now that the chassis bodies exist.
        auto view = m_Scene->GetAllEntitiesWith<VehicleComponent>();
        for (auto entityID : view)
        {
            Entity entity{ entityID, m_Scene };
            CreateVehicle(entity);
        }

        OLO_CORE_INFO("Created {0} physics vehicles", m_Vehicles.size());
    }

    bool JoltScene::CreateVehicle(Entity entity)
    {
        if (!m_JoltSystem || !entity || !entity.HasComponent<VehicleComponent>())
            return false;

        auto& vehicle = entity.GetComponent<VehicleComponent>();
        const UUID entityID = entity.GetUUID();

        // Idempotent — already built for this entity.
        if (m_Vehicles.contains(entityID))
            return true;

        // The chassis IS this entity's rigidbody; it must exist and be dynamic to
        // be driven by suspension / traction forces.
        Ref<JoltBody> chassis = GetBodyByEntityID(entityID);
        if (!chassis || !chassis->IsValid())
        {
            OLO_CORE_WARN("VehicleComponent on entity {0} has no valid rigidbody; skipping vehicle", (u64)entityID);
            return false;
        }

        // Sanitize all authored geometry — a degenerate wheel (zero radius,
        // inverted suspension travel) asserts in Debug Jolt and NaNs the solver in
        // Release, and a C# script can write a raw field, so this is the last line
        // of defence (mirrors the joint arms).
        const f32 wheelRadius = SanitizeVehiclePositive(vehicle.m_WheelRadius, 0.35f);
        const f32 wheelWidth = SanitizeVehiclePositive(vehicle.m_WheelWidth, 0.25f);
        const f32 attachHeight = std::isfinite(vehicle.m_WheelAttachmentHeight) ? vehicle.m_WheelAttachmentHeight : -0.4f;
        const f32 halfTrack = SanitizeVehiclePositive(vehicle.m_HalfTrackWidth, 0.9f);
        const f32 frontOffset = SanitizeVehiclePositive(vehicle.m_FrontAxleOffset, 1.25f);
        const f32 rearOffset = SanitizeVehiclePositive(vehicle.m_RearAxleOffset, 1.25f);
        // Suspension travel must satisfy 0 <= min <= max (Jolt asserts otherwise).
        // min may legitimately be 0 (wheel can compress fully to the attachment
        // point) — the serializers preserve 0, so don't let SanitizeVehiclePositive
        // (which rejects <= 0) silently rewrite an authored 0 to the default here.
        f32 suspMin = (std::isfinite(vehicle.m_SuspensionMinLength) && vehicle.m_SuspensionMinLength >= 0.0f) ? std::min(vehicle.m_SuspensionMinLength, 1.0e6f) : 0.3f;
        f32 suspMax = SanitizeVehiclePositive(vehicle.m_SuspensionMaxLength, 0.5f);
        if (suspMax < suspMin)
            std::swap(suspMin, suspMax);
        const f32 suspFreq = SanitizeVehiclePositive(vehicle.m_SuspensionFrequency, 1.5f);
        // Damping is a ratio in [0, 1]; a negative / non-finite value collapses to
        // the bouncy-but-stable default, and it is clamped to critically damped.
        const f32 suspDamping = std::clamp(std::isfinite(vehicle.m_SuspensionDamping) ? vehicle.m_SuspensionDamping : 0.5f, 0.0f, 1.0f);
        const f32 maxEngineTorque = SanitizeVehiclePositive(vehicle.m_MaxEngineTorque, 500.0f);
        const f32 maxBrakeTorque = SanitizeMotorMagnitude(vehicle.m_MaxBrakeTorque);
        // Steer angle: a non-finite value disables steering (0) rather than NaNing
        // it — fail safe to wheels-straight on a corrupt/scripted garbage field.
        const f32 maxSteerRad = SanitizeJointAngleDeg(vehicle.m_MaxSteerAngleDeg, 0.0f, glm::pi<f32>(), 0.0f);

        JPH::VehicleConstraintSettings settings;
        settings.mUp = JPH::Vec3(0.0f, 1.0f, 0.0f);
        settings.mForward = JPH::Vec3(0.0f, 0.0f, 1.0f);
        // mMaxPitchRollAngle left at its JPH_PI default (anti-topple off).

        // Standard four-wheel layout: 0=FL, 1=FR (steerable), 2=RL, 3=RR (driven).
        const auto makeWheel = [&](f32 localX, f32 localZ, f32 steerRad)
        {
            auto* wheel = new JPH::WheelSettingsWV();
            wheel->mPosition = JPH::Vec3(localX, attachHeight, localZ);
            wheel->mSuspensionDirection = JPH::Vec3(0.0f, -1.0f, 0.0f);
            wheel->mSuspensionMinLength = suspMin;
            wheel->mSuspensionMaxLength = suspMax;
            wheel->mSuspensionSpring = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, suspFreq, suspDamping);
            wheel->mRadius = wheelRadius;
            wheel->mWidth = wheelWidth;
            wheel->mMaxSteerAngle = steerRad;
            wheel->mMaxBrakeTorque = maxBrakeTorque;
            // Only the rear wheels hand-brake by convention; the MVP doesn't drive
            // the hand brake, so leave mMaxHandBrakeTorque at its default.
            return wheel;
        };

        settings.mWheels.push_back(makeWheel(-halfTrack, frontOffset, maxSteerRad)); // 0 FL
        settings.mWheels.push_back(makeWheel(halfTrack, frontOffset, maxSteerRad));  // 1 FR
        settings.mWheels.push_back(makeWheel(-halfTrack, -rearOffset, 0.0f));        // 2 RL
        settings.mWheels.push_back(makeWheel(halfTrack, -rearOffset, 0.0f));         // 3 RR

        auto* controllerSettings = new JPH::WheeledVehicleControllerSettings();
        controllerSettings->mEngine.mMaxTorque = maxEngineTorque;
        // One differential driving the rear axle (rear-wheel drive for the MVP).
        controllerSettings->mDifferentials.resize(1);
        controllerSettings->mDifferentials[0].mLeftWheel = 2;  // RL
        controllerSettings->mDifferentials[0].mRightWheel = 3; // RR
        controllerSettings->mDifferentials[0].mEngineTorqueRatio = 1.0f;
        settings.mController = controllerSettings;

        // VehicleConstraint takes a Body&; lock the chassis to obtain one. The
        // constraint stores the (stable) Body* so it stays valid after the lock
        // releases — the same pattern as the two-body constraints above.
        JPH::VehicleConstraint* constraint = nullptr;
        {
            JPH::BodyLockWrite lock(m_JoltSystem->GetBodyLockInterface(), chassis->GetBodyID());
            if (!lock.Succeeded())
            {
                OLO_CORE_WARN("VehicleComponent: failed to lock chassis body for entity {0}", (u64)entityID);
                return false;
            }
            constraint = new JPH::VehicleConstraint(lock.GetBody(), settings);
        }

        // The wheels are virtual: a ray cast from each suspension attachment finds
        // the ground. ObjectLayers::MOVING collides with everything (see
        // JoltLayerInterface), so the wheels rest on static ground and dynamic
        // bodies alike; the constraint's default body filter excludes the chassis.
        // SetVehicleCollisionTester stores it in a RefConst, so the tester (and the
        // controller / wheels owned by the constraint) live as long as the
        // constraint Ref we hold below.
        constraint->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterRay(ObjectLayers::MOVING, JPH::Vec3(0.0f, 1.0f, 0.0f)));

        // A VehicleConstraint is BOTH a Constraint and a PhysicsStepListener — the
        // suspension/traction run in OnStep, so it MUST be registered as a step
        // listener too (Jolt's own warning). AddConstraint + m_Vehicles each hold a
        // ref; AddStepListener stores a raw pointer, so DestroyVehicle must
        // RemoveStepListener before the ref drops to zero.
        m_JoltSystem->AddConstraint(constraint);
        m_JoltSystem->AddStepListener(constraint);
        m_Vehicles[entityID] = constraint;
        vehicle.m_RuntimeVehicleToken = static_cast<u64>(entityID);

        OLO_CORE_TRACE("Created physics vehicle for entity {0}", (u64)entityID);
        return true;
    }

    void JoltScene::DestroyVehicle(Entity entity)
    {
        if (!entity)
            return;

        const UUID entityID = entity.GetUUID();
        if (auto it = m_Vehicles.find(entityID); it != m_Vehicles.end())
        {
            if (m_JoltSystem && it->second != nullptr)
            {
                // Unregister the step listener (raw pointer) BEFORE releasing the
                // ref, then remove the constraint.
                m_JoltSystem->RemoveStepListener(it->second);
                m_JoltSystem->RemoveConstraint(it->second);
            }
            m_Vehicles.erase(it); // releases the JPH::Ref
            OLO_CORE_TRACE("Destroyed physics vehicle for entity {0}", (u64)entityID);
        }
    }

    void JoltScene::DestroyAllVehicles()
    {
        // Vehicles reference (and step-listen on) the chassis bodies, so they must
        // be removed before the bodies they drive are destroyed.
        if (m_JoltSystem)
        {
            for (const auto& [entityID, constraint] : m_Vehicles)
            {
                if (constraint != nullptr)
                {
                    m_JoltSystem->RemoveStepListener(constraint);
                    m_JoltSystem->RemoveConstraint(constraint);
                }
            }
        }
        m_Vehicles.clear();
    }

    void JoltScene::CreateRagdolls()
    {
        if (!m_Scene)
            return;

        // Build every authored ragdoll now. This is a pure ECS-authoring pass —
        // see CreateRagdoll — so it runs BEFORE the body/constraint creation passes.
        // Snapshot the ragdoll entities first: CreateRagdoll adds components to the
        // bone entities, so don't mutate the registry while iterating a live view.
        std::vector<UUID> ragdollEntities;
        {
            auto view = m_Scene->GetAllEntitiesWith<RagdollComponent>();
            for (auto entityID : view)
                ragdollEntities.push_back(Entity{ entityID, m_Scene }.GetUUID());
        }
        for (UUID entityID : ragdollEntities)
        {
            if (auto opt = m_Scene->TryGetEntityWithUUID(entityID))
                (void)CreateRagdoll(*opt);
        }

        if (!m_Ragdolls.empty())
            OLO_CORE_INFO("Created {0} physics ragdolls", m_Ragdolls.size());
    }

    bool JoltScene::CreateRagdoll(Entity entity)
    {
        if (!m_Scene || !entity || !entity.HasComponent<RagdollComponent>())
            return false;

        auto& ragdoll = entity.GetComponent<RagdollComponent>();
        const UUID entityID = entity.GetUUID();

        // Idempotent — already built for this entity.
        if (m_Ragdolls.contains(entityID))
            return true;

        if (!ragdoll.m_Enabled)
            return false;

        // Resolve the skeleton-owning entity: m_SkeletonEntity, or this entity for 0.
        Entity skeletonEntity = entity;
        if (ragdoll.m_SkeletonEntity != 0)
        {
            auto opt = m_Scene->TryGetEntityWithUUID(ragdoll.m_SkeletonEntity);
            if (!opt)
            {
                OLO_CORE_WARN("RagdollComponent on entity {0}: skeleton entity {1} not found; skipping ragdoll", (u64)entityID, (u64)ragdoll.m_SkeletonEntity);
                return false;
            }
            skeletonEntity = *opt;
        }

        // The runtime Ref wins; otherwise take the skeleton from a SkeletonComponent.
        Ref<Skeleton> skeleton = ragdoll.m_Skeleton;
        if (!skeleton && skeletonEntity.HasComponent<SkeletonComponent>())
            skeleton = skeletonEntity.GetComponent<SkeletonComponent>().m_Skeleton;
        if (!skeleton)
        {
            OLO_CORE_WARN("RagdollComponent on entity {0}: no skeleton (set m_Skeleton or add a SkeletonComponent on the skeleton entity); skipping ragdoll", (u64)entityID);
            return false;
        }

        // Map each bone (by name == entity tag) to its scene entity under the
        // skeleton entity's hierarchy. One UUID per bone; a null UUID for bones
        // that have no matching entity.
        const std::vector<UUID> boneEntityIds = BoneEntityUtils::FindBoneEntityIds(skeletonEntity, skeleton.Raw(), m_Scene);
        if (boneEntityIds.empty())
        {
            OLO_CORE_WARN("RagdollComponent on entity {0}: no bone entities found under the skeleton hierarchy; skipping ragdoll", (u64)entityID);
            return false;
        }

        // Sanitize the authored per-bone params (a script can write garbage).
        const f32 boneMass = (std::isfinite(ragdoll.m_BoneMass) && ragdoll.m_BoneMass > 0.0f) ? std::min(ragdoll.m_BoneMass, 1.0e6f) : 1.0f;
        const f32 boneRadius = (std::isfinite(ragdoll.m_BoneRadius) && ragdoll.m_BoneRadius > 0.0f) ? std::min(ragdoll.m_BoneRadius, 1.0e3f) : 0.05f;
        const f32 swingLimitDeg = std::clamp(std::isfinite(ragdoll.m_SwingLimitDeg) ? ragdoll.m_SwingLimitDeg : 45.0f, 0.0f, 180.0f);
        const f32 twistLimitDeg = std::clamp(std::isfinite(ragdoll.m_TwistLimitDeg) ? ragdoll.m_TwistLimitDeg : 45.0f, 0.0f, 180.0f);

        const auto& parentIndices = skeleton->m_ParentIndices;
        const sizet boneCount = boneEntityIds.size();

        // Resolve a bone index to its (valid) entity, or a null Entity.
        const auto boneEntity = [this, boneCount, &boneEntityIds](sizet i)
        {
            if (i >= boneCount || static_cast<u64>(boneEntityIds[i]) == 0)
                return Entity{};
            return m_Scene->TryGetEntityWithUUID(boneEntityIds[i]).value_or(Entity{});
        };

        RagdollRuntime runtime;

        // Pass A — ensure a dynamic body (+ sphere collider) on every bone that
        // doesn't already carry one. Add the collider BEFORE the rigidbody so the
        // body resolves its shape (the documented OnComponentAdded convention).
        for (sizet i = 0; i < boneCount; ++i)
        {
            Entity bone = boneEntity(i);
            if (!bone || !bone.HasComponent<TransformComponent>())
                continue;
            if (bone.HasComponent<Rigidbody3DComponent>())
                continue; // pre-authored body (e.g. a Static root anchor) — keep it

            if (!bone.HasComponent<SphereCollider3DComponent>())
            {
                auto& col = bone.AddComponent<SphereCollider3DComponent>();
                col.m_Radius = boneRadius;
                runtime.m_GeneratedColliderEntities.push_back(bone.GetUUID());
            }
            auto& rb = bone.AddComponent<Rigidbody3DComponent>();
            rb.m_Type = BodyType3D::Dynamic;
            rb.m_Mass = boneMass;
            runtime.m_GeneratedBodyEntities.push_back(bone.GetUUID());
        }

        // Pass B — link each child bone to its parent with a SwingTwist joint that
        // pivots about the parent bone's origin (so the child hangs from it like a
        // pendulum link), with CollideConnected = false so the ragdoll can fold.
        for (sizet i = 0; i < boneCount; ++i)
        {
            Entity child = boneEntity(i);
            if (!child || !child.HasComponent<TransformComponent>())
                continue;
            const int parentIdx = (i < parentIndices.size()) ? parentIndices[i] : -1;
            if (parentIdx < 0)
                continue; // root bone — no parent to hang from (author it Static to anchor)
            Entity parent = boneEntity(static_cast<sizet>(parentIdx));
            if (!parent || !parent.HasComponent<TransformComponent>())
                continue;
            if (child.HasComponent<PhysicsJoint3DComponent>())
                continue; // don't clobber a pre-authored joint on this bone

            const auto& childTc = child.GetComponent<TransformComponent>();
            const auto& parentTc = parent.GetComponent<TransformComponent>();
            const glm::vec3 childPos = childTc.Translation;
            const glm::vec3 parentPos = parentTc.Translation;
            const glm::quat invChildRot = glm::inverse(childTc.GetRotation());

            glm::vec3 worldDir = childPos - parentPos;
            worldDir = (glm::dot(worldDir, worldDir) < 1.0e-12f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::normalize(worldDir);

            auto& joint = child.AddComponent<PhysicsJoint3DComponent>();
            joint.m_Type = JointType3D::SwingTwist;
            joint.m_ConnectedEntity = parent.GetUUID();
            // Pivot at the parent bone's origin, expressed in each body's local space.
            // CreateConstraint recomputes the world anchors from the bone transforms;
            // these make worldA == worldB == parentPos and the twist axis run along
            // the bone (CreateConstraint applies childRot to m_Axis).
            joint.m_LocalAnchorA = invChildRot * (parentPos - childPos); // on the child
            joint.m_LocalAnchorB = glm::vec3(0.0f);                      // on the parent (its origin)
            joint.m_Axis = invChildRot * worldDir;
            joint.m_SwingNormalHalfAngleDeg = swingLimitDeg;
            joint.m_SwingPlaneHalfAngleDeg = swingLimitDeg;
            joint.m_TwistMinAngleDeg = -twistLimitDeg;
            joint.m_TwistMaxAngleDeg = twistLimitDeg;
            joint.m_CollideConnected = false;
            runtime.m_GeneratedJointEntities.push_back(child.GetUUID());
        }

        // A ragdoll that generated neither bodies nor joints did nothing useful —
        // don't register it, so GetRagdollCount reflects real ragdolls.
        if (runtime.m_GeneratedBodyEntities.empty() && runtime.m_GeneratedJointEntities.empty())
        {
            OLO_CORE_WARN("RagdollComponent on entity {0}: resolved a skeleton but generated no bodies/joints; skipping ragdoll", (u64)entityID);
            return false;
        }

        const sizet bodyCount = runtime.m_GeneratedBodyEntities.size();
        const sizet jointCount = runtime.m_GeneratedJointEntities.size();
        m_Ragdolls[entityID] = std::move(runtime);
        ragdoll.m_RuntimeRagdollToken = static_cast<u64>(entityID);
        OLO_CORE_TRACE("Created physics ragdoll for entity {0} ({1} generated bodies, {2} joints)", (u64)entityID, bodyCount, jointCount);
        return true;
    }

    void JoltScene::DestroyRagdoll(Entity entity)
    {
        if (!entity || !m_Scene)
            return;

        const UUID entityID = entity.GetUUID();
        auto it = m_Ragdolls.find(entityID);
        if (it == m_Ragdolls.end())
            return;

        // Move the lists out before removing components — RemoveComponent fires the
        // OnComponentRemoved hooks (which release the matching Jolt body/constraint)
        // and could, in principle, re-enter; work off a local copy and erase first.
        const RagdollRuntime runtime = std::move(it->second);
        m_Ragdolls.erase(it);

        // Remove joints FIRST (a constraint references its two bodies), then the
        // generated bodies, then the generated colliders.
        for (UUID boneId : runtime.m_GeneratedJointEntities)
        {
            if (auto opt = m_Scene->TryGetEntityWithUUID(boneId); opt && opt->HasComponent<PhysicsJoint3DComponent>())
                opt->RemoveComponent<PhysicsJoint3DComponent>();
        }
        for (UUID boneId : runtime.m_GeneratedBodyEntities)
        {
            if (auto opt = m_Scene->TryGetEntityWithUUID(boneId); opt && opt->HasComponent<Rigidbody3DComponent>())
                opt->RemoveComponent<Rigidbody3DComponent>();
        }
        for (UUID boneId : runtime.m_GeneratedColliderEntities)
        {
            if (auto opt = m_Scene->TryGetEntityWithUUID(boneId); opt && opt->HasComponent<SphereCollider3DComponent>())
                opt->RemoveComponent<SphereCollider3DComponent>();
        }

        if (entity.HasComponent<RagdollComponent>())
            entity.GetComponent<RagdollComponent>().m_RuntimeRagdollToken = 0;

        OLO_CORE_TRACE("Destroyed physics ragdoll for entity {0}", (u64)entityID);
    }

    void JoltScene::DestroyAllRagdolls()
    {
        if (!m_Scene)
        {
            m_Ragdolls.clear();
            return;
        }

        // Snapshot the keys first: DestroyRagdoll erases from m_Ragdolls (and its
        // RemoveComponent calls fire hooks), so don't iterate the map while mutating it.
        std::vector<UUID> ragdollEntities;
        ragdollEntities.reserve(m_Ragdolls.size());
        for (const auto& [entityID, runtime] : m_Ragdolls)
            ragdollEntities.push_back(entityID);

        for (UUID entityID : ragdollEntities)
        {
            if (auto opt = m_Scene->TryGetEntityWithUUID(entityID))
                DestroyRagdoll(*opt);
            else
                m_Ragdolls.erase(entityID); // entity gone — drop the stale tracking
        }
        m_Ragdolls.clear();
    }

    void JoltScene::UpdateVehicleControllers()
    {
        if (m_Vehicles.empty() || !m_Scene)
            return;

        JPH::BodyInterface& bodyInterface = m_JoltSystem->GetBodyInterface();
        for (const auto& [entityID, constraint] : m_Vehicles)
        {
            if (constraint == nullptr)
                continue;

            // Non-asserting lookup: m_Vehicles can briefly outlive the entity in
            // teardown edge cases, and GetEntityByUUID would assert on a missing
            // UUID — skip cleanly instead of crashing the physics tick.
            auto entityOpt = m_Scene->TryGetEntityWithUUID(entityID);
            if (!entityOpt || !entityOpt->HasComponent<VehicleComponent>())
                continue;
            Entity entity = *entityOpt;

            auto* controller = static_cast<JPH::WheeledVehicleController*>(constraint->GetController());
            if (controller == nullptr)
                continue;

            // Sanitize the live driver input (a script can write any value).
            const auto& vc = entity.GetComponent<VehicleComponent>();
            const f32 forward = std::isfinite(vc.m_ThrottleInput) ? std::clamp(vc.m_ThrottleInput, -1.0f, 1.0f) : 0.0f;
            const f32 right = std::isfinite(vc.m_SteerInput) ? std::clamp(vc.m_SteerInput, -1.0f, 1.0f) : 0.0f;
            const f32 brake = std::isfinite(vc.m_BrakeInput) ? std::clamp(vc.m_BrakeInput, 0.0f, 1.0f) : 0.0f;
            controller->SetDriverInput(forward, right, brake, 0.0f);

            // A settled vehicle sleeps; without this it would ignore throttle /
            // steering / brake input until something else woke it.
            if (const JPH::Body* body = constraint->GetVehicleBody();
                body != nullptr && (std::abs(forward) > 1.0e-4f || std::abs(right) > 1.0e-4f || brake > 1.0e-4f))
            {
                bodyInterface.ActivateBody(body->GetID());
            }
        }
    }

    void JoltScene::BreakOverstressedJoints(f32 stepDeltaTime)
    {
        // Nothing to break, or no scene to publish on / look up components from.
        if (m_Constraints.empty() || !m_Scene || stepDeltaTime <= 0.0f)
            return;

        // Scan first, mutate after: DestroyConstraint() erases from m_Constraints,
        // which would invalidate this iteration, and event handlers may touch the
        // scene. So gather the events, then break + publish outside the loop.
        std::vector<JointBrokeEvent> broken;
        for (const auto& [entityID, constraint] : m_Constraints)
        {
            if (constraint == nullptr)
                continue;

            Entity entity = m_Scene->GetEntityByUUID(entityID);
            if (!entity || !entity.HasComponent<PhysicsJoint3DComponent>())
                continue;

            const auto& joint = entity.GetComponent<PhysicsJoint3DComponent>();
            const bool forceEnabled = joint.m_BreakForce > 0.0f;
            const bool torqueEnabled = joint.m_BreakTorque > 0.0f;
            if (!forceEnabled && !torqueEnabled)
                continue; // unbreakable joint — skip the read entirely

            f32 linearImpulse = 0.0f;
            f32 angularImpulse = 0.0f;
            if (!ReadConstraintImpulses(*constraint, linearImpulse, angularImpulse))
                continue;

            // lambda is an impulse (force·dt / torque·dt); recover the average
            // force/torque the joint carried over the step.
            const f32 force = linearImpulse / stepDeltaTime;
            const f32 torque = angularImpulse / stepDeltaTime;
            const bool brokeByForce = forceEnabled && (force > joint.m_BreakForce);
            const bool brokeByTorque = torqueEnabled && (torque > joint.m_BreakTorque);
            if (!brokeByForce && !brokeByTorque)
                continue;

            broken.push_back(JointBrokeEvent{
                .EntityID = entityID,
                .ConnectedEntityID = joint.m_ConnectedEntity,
                .Type = joint.m_Type,
                .Force = force,
                .Torque = torque,
                .BrokeByForce = brokeByForce,
                .BrokeByTorque = brokeByTorque });
        }

        for (const JointBrokeEvent& event : broken)
        {
            Entity entity = m_Scene->GetEntityByUUID(event.EntityID);
            // Remove the Jolt constraint (erases the m_Constraints entry) and
            // clear the component's runtime token so it reads as "no constraint".
            DestroyConstraint(entity);
            if (entity && entity.HasComponent<PhysicsJoint3DComponent>())
                entity.GetComponent<PhysicsJoint3DComponent>().m_RuntimeConstraintToken = 0;

            OLO_CORE_TRACE("PhysicsJoint3D on entity {0} broke (force={1} N, torque={2} N·m)",
                           (u64)event.EntityID, event.Force, event.Torque);

            // Publish last so a handler that inspects the joint/entity sees the
            // already-broken state.
            m_Scene->GetGameplayEvents().Publish(event);
        }
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

        // Route the system limits through PhysicsSettings (the same source
        // Physics3DSystem::Init reads) instead of hardcoding them, so a project can
        // actually raise them past the old fixed constants (issue #523). Clamp to a
        // sane minimum defensively: Physics3DSystem::SetSettings() has no validating
        // caller other than ProjectSerializer, so a test or script that sets the
        // settings directly could otherwise hand Jolt a zero-sized Init().
        const PhysicsSettings& settings = Physics3DSystem::GetSettings();
        const u32 maxBodies = std::max(settings.m_MaxBodies, 1u);
        const u32 maxBodyPairs = std::max(settings.m_MaxBodyPairs, 1u);

        // Create temp allocator, scaled against kBaselineTempAllocatorSize (see its
        // comment): Jolt's per-step scratch usage scales with maxContactConstraints, and
        // TempAllocatorImpl is a fixed-size ring buffer, so a bigger constraint capacity
        // needs a proportionally bigger scratch buffer or Jolt silently corrupts memory.
        // Cap maxContactConstraints itself to whatever the 512 MB scratch-buffer ceiling
        // can support *before* it reaches PhysicsSystem::Init — Physics3DSystem::SetSettings()
        // has no validating caller other than ProjectSerializer, so a direct caller (test,
        // script) could otherwise hand Jolt a constraint capacity the scratch buffer can't
        // back, corrupting memory instead of just under-allocating.
        constexpr u64 kMaxTempAllocatorSize = 512ull * 1024 * 1024;
        const u64 maxSafeContactConstraints =
            (static_cast<u64>(kBaselineMaxContactConstraints) * kMaxTempAllocatorSize) / kBaselineTempAllocatorSize;
        const u32 maxContactConstraints = static_cast<u32>(
            std::clamp<u64>(std::max(settings.m_MaxContactConstraints, 1u), 1ull, maxSafeContactConstraints));
        m_AppliedMaxBodies = maxBodies;
        m_AppliedMaxBodyPairs = maxBodyPairs;
        m_AppliedMaxContactConstraints = maxContactConstraints;

        const u64 scaledTempAllocatorSize = (static_cast<u64>(kBaselineTempAllocatorSize) * maxContactConstraints) / kBaselineMaxContactConstraints;
        const u32 tempAllocatorSize = static_cast<u32>(
            std::clamp<u64>(scaledTempAllocatorSize, kBaselineTempAllocatorSize, kMaxTempAllocatorSize));
        m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(tempAllocatorSize);

        // Create job system adapter that wraps FScheduler
        m_JobSystem = std::make_unique<JoltJobSystemAdapter>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);

        // Create layer interfaces
        m_BroadPhaseLayerInterface = std::make_unique<BroadPhaseLayerInterface>();
        m_ObjectVsBroadPhaseLayerFilter = std::make_unique<ObjectVsBroadPhaseLayerFilter>();
        m_ObjectLayerPairFilter = std::make_unique<ObjectLayerPairFilter>();

        // Create physics system
        m_JoltSystem = std::make_unique<JPH::PhysicsSystem>();
        m_JoltSystem->Init(maxBodies, s_NumBodyMutexes, maxBodyPairs, maxContactConstraints,
                           *m_BroadPhaseLayerInterface, *m_ObjectVsBroadPhaseLayerFilter, *m_ObjectLayerPairFilter);

        // Create contact listener
        m_ContactListener = std::make_unique<JoltContactListener>(*this);
        m_JoltSystem->SetContactListener(m_ContactListener.get());

        // Apply gravity from PhysicsSettings instead of hardcoding Earth gravity —
        // a project authoring non-Earth gravity (underwater levels, low-g, etc.) had
        // zero effect on the live simulation before issue #540.
        m_JoltSystem->SetGravity(JoltUtils::ToJoltVector(settings.m_Gravity));

        // Mirror Physics3DSystem::UpdatePhysicsSystemSettings's solver/sleep tuning
        // onto the system that's actually stepped every frame (JoltScene::Simulate).
        // Physics3DSystem::m_PhysicsSystem is never stepped — nothing calls
        // Physics3DSystem::Update — so applying this tuning only there (as it was
        // before #540) was a no-op on the live sim; JoltScene::m_JoltSystem is the
        // only system that matters at runtime.
        JPH::PhysicsSettings joltSettings;
        joltSettings.mNumVelocitySteps = settings.m_VelocitySolverIterations;
        joltSettings.mNumPositionSteps = settings.m_PositionSolverIterations;
        joltSettings.mBaumgarte = settings.m_Baumgarte;
        joltSettings.mSpeculativeContactDistance = settings.m_SpeculativeContactDistance;
        joltSettings.mPenetrationSlop = settings.m_PenetrationSlop;
        joltSettings.mLinearCastThreshold = settings.m_LinearCastThreshold;
        joltSettings.mMinVelocityForRestitution = settings.m_MinVelocityForRestitution;
        joltSettings.mTimeBeforeSleep = settings.m_TimeBeforeSleep;
        joltSettings.mPointVelocitySleepThreshold = settings.m_PointVelocitySleepThreshold;
        joltSettings.mDeterministicSimulation = settings.m_DeterministicSimulation;
        joltSettings.mConstraintWarmStart = settings.m_ConstraintWarmStart;
        joltSettings.mUseBodyPairContactCache = settings.m_UseBodyPairContactCache;
        joltSettings.mUseManifoldReduction = settings.m_UseManifoldReduction;
        joltSettings.mUseLargeIslandSplitter = settings.m_UseLargeIslandSplitter;
        joltSettings.mAllowSleeping = settings.m_AllowSleeping;
        m_JoltSystem->SetPhysicsSettings(joltSettings);

        // Seed the fixed timestep from PhysicsSettings instead of leaving the 1/60
        // default initializer in place (issue #540). Guard defensively — same
        // rationale as the maxBodies/maxContactConstraints clamps above:
        // SetSettings() has no validating caller other than ProjectSerializer, so a
        // direct caller (test, script) could hand this a non-positive/non-finite
        // value that would spin Simulate()'s accumulator loop forever.
        if (std::isfinite(settings.m_FixedTimestep) && settings.m_FixedTimestep > 0.0f)
        {
            m_FixedTimeStep = settings.m_FixedTimestep;
        }

        OLO_CORE_INFO("Jolt Physics initialized - MaxBodies: {0}, MaxBodyPairs: {1}, MaxContactConstraints: {2}, "
                      "TempAllocatorSize: {3} bytes, Gravity: ({4}, {5}, {6}), FixedTimeStep: {7}",
                      maxBodies, maxBodyPairs, maxContactConstraints, tempAllocatorSize,
                      settings.m_Gravity.x, settings.m_Gravity.y, settings.m_Gravity.z, m_FixedTimeStep);
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
