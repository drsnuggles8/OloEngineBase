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

namespace OloEngine {

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

		// Destroy all bodies
		for (auto& [entityID, body] : m_Bodies)
		{
			// Body destructor will handle Jolt cleanup
		}
		m_Bodies.clear();
		m_BodiesToSync.clear();

		// Destroy all character controllers
		for (auto& [entityID, characterController] : m_CharacterControllers)
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
		if (!m_Initialized || !m_JoltSystem)
			return;

		// Process contact events
		if (m_ContactListener)
		{
			m_ContactListener->ProcessContactEvents();
		}

		// Fixed timestep simulation with accumulator
		m_Accumulator += deltaTime;

		while (m_Accumulator >= m_FixedTimeStep)
		{
			Step(m_FixedTimeStep);
			m_Accumulator -= m_FixedTimeStep;
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
			m_JobSystem.get()
		);
		
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
		if (!entity || !entity.HasComponent<RigidBody3DComponent>())
		{
			OLO_CORE_ERROR("Cannot create physics body for entity without RigidBody3DComponent");
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
			// Remove from sync list
			auto syncIt = std::find(m_BodiesToSync.begin(), m_BodiesToSync.end(), it->second);
			if (syncIt != m_BodiesToSync.end())
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
		// Search through all bodies to find the one with matching BodyID
		for (const auto& [entityID, body] : m_Bodies)
		{
			if (body && body->GetBodyID() == bodyID)
			{
				return body->GetEntity();
			}
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
		auto it = m_CharacterControllers.find(entityID);
		if (it != m_CharacterControllers.end())
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
			auto updateIt = std::find(m_CharacterControllersToUpdate.begin(), m_CharacterControllersToUpdate.end(), it->second);
			if (updateIt != m_CharacterControllersToUpdate.end())
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
	}

	void JoltScene::OnRuntimeStop()
	{
		OLO_CORE_INFO("JoltScene stopping runtime");

		// Destroy all bodies
		for (auto& [entityID, body] : m_Bodies)
		{
			// Body destructor will handle cleanup
		}
		m_Bodies.clear();
		m_BodiesToSync.clear();
	}

	void JoltScene::OnSimulationStart()
	{
		OLO_CORE_INFO("JoltScene starting simulation");
		// Additional simulation-specific setup can go here
	}

	void JoltScene::OnSimulationStop()
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
				const BoxCastInfo& boxInfo = static_cast<const BoxCastInfo&>(shapeCastInfo);
				return CastBox(boxInfo, outHit);
			}
			case ShapeCastType::Sphere:
			{
				const SphereCastInfo& sphereInfo = static_cast<const SphereCastInfo&>(shapeCastInfo);
				return CastSphere(sphereInfo, outHit);
			}
			case ShapeCastType::Capsule:
			{
				const CapsuleCastInfo& capsuleInfo = static_cast<const CapsuleCastInfo&>(shapeCastInfo);
				return CastCapsule(capsuleInfo, outHit);
			}
			default:
				OLO_CORE_ERROR("Unsupported shape cast type");
				return false;
		}
	}

	bool JoltScene::CastBox(const BoxCastInfo& boxCastInfo, SceneQueryHit& outHit)
	{
		if (!m_JoltSystem)
			return false;

		JPH::Ref<JPH::Shape> boxShape = new JPH::BoxShape(JoltUtils::ToJoltVector(boxCastInfo.m_HalfExtent));
		return PerformShapeCast(boxShape, boxCastInfo.m_Origin, boxCastInfo.m_Direction, 
			boxCastInfo.m_MaxDistance, boxCastInfo.m_LayerMask, boxCastInfo.m_ExcludedEntities, outHit);
	}

	bool JoltScene::CastSphere(const SphereCastInfo& sphereCastInfo, SceneQueryHit& outHit)
	{
		if (!m_JoltSystem)
			return false;

		JPH::Ref<JPH::Shape> sphereShape = new JPH::SphereShape(sphereCastInfo.m_Radius);
		return PerformShapeCast(sphereShape, sphereCastInfo.m_Origin, sphereCastInfo.m_Direction,
			sphereCastInfo.m_MaxDistance, sphereCastInfo.m_LayerMask, sphereCastInfo.m_ExcludedEntities, outHit);
	}

	bool JoltScene::CastCapsule(const CapsuleCastInfo& capsuleCastInfo, SceneQueryHit& outHit)
	{
		if (!m_JoltSystem)
			return false;

		JPH::Ref<JPH::Shape> capsuleShape = new JPH::CapsuleShape(capsuleCastInfo.m_HalfHeight, capsuleCastInfo.m_Radius);
		return PerformShapeCast(capsuleShape, capsuleCastInfo.m_Origin, capsuleCastInfo.m_Direction,
			capsuleCastInfo.m_MaxDistance, capsuleCastInfo.m_LayerMask, capsuleCastInfo.m_ExcludedEntities, outHit);
	}

	i32 JoltScene::CastBoxMultiple(const BoxCastInfo& boxCastInfo, SceneQueryHit* outHits, i32 maxHits)
	{
		if (!m_JoltSystem)
			return 0;

		JPH::Ref<JPH::Shape> boxShape = new JPH::BoxShape(JoltUtils::ToJoltVector(boxCastInfo.m_HalfExtent));
		return PerformShapeCastMultiple(boxShape, boxCastInfo.m_Origin, boxCastInfo.m_Direction,
			boxCastInfo.m_MaxDistance, boxCastInfo.m_LayerMask, boxCastInfo.m_ExcludedEntities, outHits, maxHits);
	}

	i32 JoltScene::CastSphereMultiple(const SphereCastInfo& sphereCastInfo, SceneQueryHit* outHits, i32 maxHits)
	{
		if (!m_JoltSystem)
			return 0;

		JPH::Ref<JPH::Shape> sphereShape = new JPH::SphereShape(sphereCastInfo.m_Radius);
		return PerformShapeCastMultiple(sphereShape, sphereCastInfo.m_Origin, sphereCastInfo.m_Direction,
			sphereCastInfo.m_MaxDistance, sphereCastInfo.m_LayerMask, sphereCastInfo.m_ExcludedEntities, outHits, maxHits);
	}

	i32 JoltScene::CastCapsuleMultiple(const CapsuleCastInfo& capsuleCastInfo, SceneQueryHit* outHits, i32 maxHits)
	{
		if (!m_JoltSystem)
			return 0;

		JPH::Ref<JPH::Shape> capsuleShape = new JPH::CapsuleShape(capsuleCastInfo.m_HalfHeight, capsuleCastInfo.m_Radius);
		return PerformShapeCastMultiple(capsuleShape, capsuleCastInfo.m_Origin, capsuleCastInfo.m_Direction,
			capsuleCastInfo.m_MaxDistance, capsuleCastInfo.m_LayerMask, capsuleCastInfo.m_ExcludedEntities, outHits, maxHits);
	}

	i32 JoltScene::OverlapShape(const ShapeOverlapInfo& overlapInfo, SceneQueryHit* outHits, i32 maxHits)
	{
		switch (overlapInfo.GetCastType())
		{
			case ShapeCastType::Box:
			{
				const BoxOverlapInfo& boxInfo = static_cast<const BoxOverlapInfo&>(overlapInfo);
				return OverlapBox(boxInfo, outHits, maxHits);
			}
			case ShapeCastType::Sphere:
			{
				const SphereOverlapInfo& sphereInfo = static_cast<const SphereOverlapInfo&>(overlapInfo);
				return OverlapSphere(sphereInfo, outHits, maxHits);
			}
			case ShapeCastType::Capsule:
			{
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
		return PerformShapeOverlap(boxShape, boxOverlapInfo.m_Origin, boxOverlapInfo.m_Rotation,
			boxOverlapInfo.m_LayerMask, boxOverlapInfo.m_ExcludedEntities, outHits, maxHits);
	}

	i32 JoltScene::OverlapSphere(const SphereOverlapInfo& sphereOverlapInfo, SceneQueryHit* outHits, i32 maxHits)
	{
		if (!m_JoltSystem)
			return 0;

		JPH::Ref<JPH::Shape> sphereShape = new JPH::SphereShape(sphereOverlapInfo.m_Radius);
		return PerformShapeOverlap(sphereShape, sphereOverlapInfo.m_Origin, sphereOverlapInfo.m_Rotation,
			sphereOverlapInfo.m_LayerMask, sphereOverlapInfo.m_ExcludedEntities, outHits, maxHits);
	}

	i32 JoltScene::OverlapCapsule(const CapsuleOverlapInfo& capsuleOverlapInfo, SceneQueryHit* outHits, i32 maxHits)
	{
		if (!m_JoltSystem)
			return 0;

		JPH::Ref<JPH::Shape> capsuleShape = new JPH::CapsuleShape(capsuleOverlapInfo.m_HalfHeight, capsuleOverlapInfo.m_Radius);
		return PerformShapeOverlap(capsuleShape, capsuleOverlapInfo.m_Origin, capsuleOverlapInfo.m_Rotation,
			capsuleOverlapInfo.m_LayerMask, capsuleOverlapInfo.m_ExcludedEntities, outHits, maxHits);
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
			hitCount++;
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
		for (auto& body : m_BodiesToSync)
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

	void JoltScene::OnContactEvent(ContactType type, UUID entityA, UUID entityB)
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

		// Create physics bodies for all entities with RigidBody3DComponent
		auto view = m_Scene->GetAllEntitiesWith<RigidBody3DComponent>();
		for (auto entityID : view)
		{
			Entity entity{ entityID, m_Scene };
			CreateBody(entity);
		}

		OLO_CORE_INFO("Created {0} physics bodies", m_Bodies.size());
	}

	void JoltScene::SynchronizeBody(Ref<JoltBody> body)
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
		transform.Rotation = glm::eulerAngles(rotation);
	}

	void JoltScene::InitializeJolt()
	{
		// Register all Jolt physics types
		JPH::RegisterDefaultAllocator();
		JPH::Trace = nullptr; // Disable Jolt's trace output
		JPH::Factory::sInstance = new JPH::Factory();
		JPH::RegisterTypes();

		// Create temp allocator
		m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(s_TempAllocatorSize);

		// Create job system
		m_JobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::thread::hardware_concurrency() - 1);

		// Create layer interfaces
		m_BroadPhaseLayerInterface = std::make_unique<BroadPhaseLayerInterface>();
		m_ObjectVsBroadPhaseLayerFilter = std::make_unique<ObjectVsBroadPhaseLayerFilter>();
		m_ObjectLayerPairFilter = std::make_unique<ObjectLayerPairFilter>();

		// Create physics system
		m_JoltSystem = std::make_unique<JPH::PhysicsSystem>();
		m_JoltSystem->Init(s_MaxBodies, s_NumBodyMutexes, s_MaxBodyPairs, s_MaxContactConstraints, 
			*m_BroadPhaseLayerInterface, *m_ObjectVsBroadPhaseLayerFilter, *m_ObjectLayerPairFilter);

		// Create contact listener
		m_ContactListener = std::make_unique<JoltContactListener>(this);
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

		// Cleanup Jolt
		delete JPH::Factory::sInstance;
		JPH::Factory::sInstance = nullptr;
		JPH::UnregisterTypes();

		OLO_CORE_INFO("Jolt Physics shut down");
	}

	// Scene query helper methods
	bool JoltScene::PerformShapeCast(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction, 
		f32 maxDistance, u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit& outHit)
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
			castDirection
		);

		// Perform shape cast
		JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> hitCollector;
		JPH::ShapeCastSettings shapeCastSettings;
		
		// Create filters
		JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*m_ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayer(layerMask));
		JPH::DefaultObjectLayerFilter objectLayerFilter(*m_ObjectLayerPairFilter, JPH::ObjectLayer(layerMask));
		EntityExclusionBodyFilter bodyFilter(excludedEntities);

		m_JoltSystem->GetNarrowPhaseQuery().CastShape(shapeCast, shapeCastSettings, startPos, hitCollector, broadPhaseFilter, objectLayerFilter, bodyFilter);
		
		if (!hitCollector.HadHit())
			return false;

		// Fill hit information
		FillHitInfo(hitCollector.mHit, shapeCast, outHit);
		return true;
	}

	i32 JoltScene::PerformShapeCastMultiple(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
		f32 maxDistance, u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit* outHits, i32 maxHits)
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
			castDirection
		);

		// Perform shape cast with multiple hit collector
		JPH::AllHitCollisionCollector<JPH::CastShapeCollector> hitCollector;
		JPH::ShapeCastSettings shapeCastSettings;
		
		// Create filters
		JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*m_ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayer(layerMask));
		JPH::DefaultObjectLayerFilter objectLayerFilter(*m_ObjectLayerPairFilter, JPH::ObjectLayer(layerMask));
		EntityExclusionBodyFilter bodyFilter(excludedEntities);

		m_JoltSystem->GetNarrowPhaseQuery().CastShape(shapeCast, shapeCastSettings, startPos, hitCollector, broadPhaseFilter, objectLayerFilter, bodyFilter);

		// Fill hit results
		i32 hitCount = 0;
		for (const auto& hit : hitCollector.mHits)
		{
			if (hitCount >= maxHits)
				break;

			FillHitInfo(hit, shapeCast, outHits[hitCount]);
			hitCount++;
		}

		return hitCount;
	}

	i32 JoltScene::PerformShapeOverlap(JPH::Ref<JPH::Shape> shape, const glm::vec3& position, const glm::quat& rotation,
		u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit* outHits, i32 maxHits)
	{
		if (!m_JoltSystem || maxHits <= 0)
			return 0;

		// Create transform for the overlap shape
		JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(
			JoltUtils::ToJoltQuat(rotation),
			JoltUtils::ToJoltVector(position)
		);

		// Perform overlap query
		JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hitCollector;
		JPH::CollideShapeSettings overlapSettings;
		
		// Create filters
		JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*m_ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayer(layerMask));
		JPH::DefaultObjectLayerFilter objectLayerFilter(*m_ObjectLayerPairFilter, JPH::ObjectLayer(layerMask));
		EntityExclusionBodyFilter bodyFilter(excludedEntities);

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
				hitInfo.HitEntity = static_cast<UUID>(body.GetUserData());
				hitInfo.Position = JoltUtils::FromJoltVector(body.GetPosition());
				
				// Get body from our map for reference
				auto it = m_Bodies.find(hitInfo.HitEntity);
				if (it != m_Bodies.end())
				{
					hitInfo.HitBody = it->second;
				}
				
				hitCount++;
			}
		}

		return hitCount;
	}

	bool JoltScene::IsEntityExcluded(UUID entityID, const std::vector<UUID>& excludedEntities)
	{
		return EntityExclusionUtils::IsEntityExcluded(excludedEntities, entityID);
	}

	// New O(1) ExcludedEntitySet overloads
	bool JoltScene::PerformShapeCast(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction, 
		f32 maxDistance, u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit& outHit)
	{
		// Convert to vector and delegate to existing implementation for now
		// TODO: Optimize to use ExcludedEntitySet directly in body filter
		std::vector<UUID> excludedEntities = excludedEntitySet.ToVector();
		return PerformShapeCast(shape, start, direction, maxDistance, layerMask, excludedEntities, outHit);
	}

	i32 JoltScene::PerformShapeCastMultiple(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
		f32 maxDistance, u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit* outHits, i32 maxHits)
	{
		// Convert to vector and delegate to existing implementation for now
		// TODO: Optimize to use ExcludedEntitySet directly in body filter
		std::vector<UUID> excludedEntities = excludedEntitySet.ToVector();
		return PerformShapeCastMultiple(shape, start, direction, maxDistance, layerMask, excludedEntities, outHits, maxHits);
	}

	i32 JoltScene::PerformShapeOverlap(JPH::Ref<JPH::Shape> shape, const glm::vec3& position, const glm::quat& rotation,
		u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit* outHits, i32 maxHits)
	{
		// Convert to vector and delegate to existing implementation for now
		// TODO: Optimize to use ExcludedEntitySet directly in body filter
		std::vector<UUID> excludedEntities = excludedEntitySet.ToVector();
		return PerformShapeOverlap(shape, position, rotation, layerMask, excludedEntities, outHits, maxHits);
	}

	bool JoltScene::IsEntityExcluded(UUID entityID, const ExcludedEntitySet& excludedEntitySet)
	{
		return EntityExclusionUtils::IsEntityExcluded(excludedEntitySet, entityID);
	}

	void JoltScene::FillHitInfo(const JPH::RayCastResult& hit, const JPH::RRayCast& ray, SceneQueryHit& outHit)
	{
		outHit.Clear();
		
		JPH::Vec3 hitPosition = ray.GetPointOnRay(hit.mFraction);
		outHit.Position = JoltUtils::FromJoltVector(hitPosition);
		outHit.Distance = hit.mFraction * ray.mDirection.Length();

		// Lock the body to get additional information
		JPH::BodyLockRead bodyLock(m_JoltSystem->GetBodyLockInterface(), hit.mBodyID);
		if (bodyLock.Succeeded())
		{
			const JPH::Body& body = bodyLock.GetBody();
			outHit.HitEntity = static_cast<UUID>(body.GetUserData());
			outHit.Normal = JoltUtils::FromJoltVector(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPosition));
			
			// Get body reference from our map
			auto it = m_Bodies.find(outHit.HitEntity);
			if (it != m_Bodies.end())
			{
				outHit.HitBody = it->second;
			}
		}
	}

	void JoltScene::FillHitInfo(const JPH::ShapeCastResult& hit, const JPH::RShapeCast& shapeCast, SceneQueryHit& outHit)
	{
		outHit.Clear();
		
		JPH::Vec3 hitPosition = shapeCast.GetPointOnRay(hit.mFraction);
		outHit.Position = JoltUtils::FromJoltVector(hitPosition);
		outHit.Distance = hit.mFraction * shapeCast.mDirection.Length();
		outHit.Normal = JoltUtils::FromJoltVector(hit.mPenetrationAxis.Normalized());

		// Lock the body to get additional information
		JPH::BodyLockRead bodyLock(m_JoltSystem->GetBodyLockInterface(), hit.mBodyID2);
		if (bodyLock.Succeeded())
		{
			const JPH::Body& body = bodyLock.GetBody();
			outHit.HitEntity = static_cast<UUID>(body.GetUserData());
			
			// Get body reference from our map
			auto it = m_Bodies.find(outHit.HitEntity);
			if (it != m_Bodies.end())
			{
				outHit.HitBody = it->second;
			}
		}
	}

}