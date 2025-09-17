#include "OloEnginePCH.h"
#include "JoltScene.h"
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

		JPH::RRayCast ray;
		ray.mOrigin = JoltUtils::ToJoltVector(rayInfo.Origin);
		ray.mDirection = JoltUtils::ToJoltVector(rayInfo.Direction * rayInfo.MaxDistance);

		JPH::RayCastResult hit;
		auto& layerInterface = *m_BroadPhaseLayerInterface;
		JPH::DefaultBroadPhaseLayerFilter broadPhaseLayerFilter(*m_ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayer(0));
		JPH::DefaultObjectLayerFilter objectLayerFilter(*m_ObjectLayerPairFilter, JPH::ObjectLayer(0));
		JPH::BodyFilter bodyFilter{};
		
		if (m_JoltSystem->GetNarrowPhaseQuery().CastRay(ray, hit, broadPhaseLayerFilter, objectLayerFilter, bodyFilter))
		{
			outHit.HasHit = true;
			outHit.Distance = hit.mFraction * rayInfo.MaxDistance;
			outHit.Position = rayInfo.Origin + rayInfo.Direction * outHit.Distance;
			
			// Get body to find entity ID
			JPH::BodyID bodyID = hit.mBodyID;
			auto& bodyInterface = m_JoltSystem->GetBodyInterface();
			u64 userData = bodyInterface.GetUserData(bodyID);
			outHit.EntityID = static_cast<UUID>(userData);

			// Calculate normal (this is a simplified approach)
			outHit.Normal = glm::vec3(0.0f, 1.0f, 0.0f); // TODO: Get actual surface normal

			return true;
		}

		outHit.HasHit = false;
		return false;
	}

	bool JoltScene::CastShape(const ShapeCastInfo& shapeInfo, SceneQueryHit& outHit)
	{
		if (!m_JoltSystem)
			return false;

		// Create shape based on shape info
		JPH::Ref<JPH::Shape> shape;
		switch (shapeInfo.Shape)
		{
			case ShapeType::Box:
				shape = new JPH::BoxShape(JoltUtils::ToJoltVector(shapeInfo.HalfExtents));
				break;
			case ShapeType::Sphere:
				shape = new JPH::SphereShape(shapeInfo.Radius);
				break;
			case ShapeType::Capsule:
				shape = new JPH::CapsuleShape(shapeInfo.HalfHeight, shapeInfo.Radius);
				break;
			default:
				OLO_CORE_ERROR("Unsupported shape type for shape cast: {0}", static_cast<i32>(shapeInfo.Shape));
				return false;
		}

		JPH::Vec3 startPos = JoltUtils::ToJoltVector(shapeInfo.Origin);
		JPH::Vec3 direction = JoltUtils::ToJoltVector(shapeInfo.Direction * shapeInfo.MaxDistance);
		JPH::RShapeCast shapeCast(shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(startPos), direction);

		JPH::ShapeCastResult hit;
		JPH::DefaultBroadPhaseLayerFilter broadPhaseLayerFilter(*m_ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayer(0));
		JPH::DefaultObjectLayerFilter objectLayerFilter(*m_ObjectLayerPairFilter, JPH::ObjectLayer(0));
		JPH::BodyFilter bodyFilter{};
		JPH::ShapeCastSettings settings{};
		
		// Use the simplified CastShape API
		class ShapeCastCollector : public JPH::CastShapeCollector
		{
		public:
			void AddHit(const JPH::ShapeCastResult& inResult) override
			{
				if (inResult.mFraction < mResult.mFraction)
					mResult = inResult;
			}
			JPH::ShapeCastResult mResult;
		};
		
		ShapeCastCollector collector;
		m_JoltSystem->GetNarrowPhaseQuery().CastShape(shapeCast, settings, startPos, collector, broadPhaseLayerFilter, objectLayerFilter, bodyFilter);
		
		if (collector.mResult.mFraction < 1.0f)
		{
			outHit.HasHit = true;
			outHit.Distance = collector.mResult.mFraction * shapeInfo.MaxDistance;
			outHit.Position = shapeInfo.Origin + shapeInfo.Direction * outHit.Distance;

			// Get entity ID from body
			u64 userData = m_JoltSystem->GetBodyInterface().GetUserData(collector.mResult.mBodyID2);
			outHit.EntityID = static_cast<UUID>(userData);

			outHit.Normal = glm::vec3(0.0f, 1.0f, 0.0f); // TODO: Get actual surface normal

			return true;
		}

		outHit.HasHit = false;
		return false;
	}

	i32 JoltScene::OverlapShape(const ShapeOverlapInfo& overlapInfo, SceneQueryHit* outHits, i32 maxHits)
	{
		if (!m_JoltSystem || !outHits || maxHits <= 0)
			return 0;

		// Create shape based on overlap info
		JPH::Ref<JPH::Shape> shape;
		switch (overlapInfo.Shape)
		{
			case ShapeType::Box:
				shape = new JPH::BoxShape(JoltUtils::ToJoltVector(overlapInfo.HalfExtents));
				break;
			case ShapeType::Sphere:
				shape = new JPH::SphereShape(overlapInfo.Radius);
				break;
			case ShapeType::Capsule:
				shape = new JPH::CapsuleShape(overlapInfo.HalfHeight, overlapInfo.Radius);
				break;
			default:
				OLO_CORE_ERROR("Unsupported shape type for overlap query: {0}", static_cast<i32>(overlapInfo.Shape));
				return 0;
		}

		// TODO: Implement overlap query
		// This would require using Jolt's CollideShape functionality
		OLO_CORE_WARN("OverlapShape not yet implemented");
		return 0;
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
		m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(TempAllocatorSize);

		// Create job system
		m_JobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::thread::hardware_concurrency() - 1);

		// Create layer interfaces
		m_BroadPhaseLayerInterface = std::make_unique<BroadPhaseLayerInterface>();
		m_ObjectVsBroadPhaseLayerFilter = std::make_unique<ObjectVsBroadPhaseLayerFilter>();
		m_ObjectLayerPairFilter = std::make_unique<ObjectLayerPairFilter>();

		// Create physics system
		m_JoltSystem = std::make_unique<JPH::PhysicsSystem>();
		m_JoltSystem->Init(MaxBodies, NumBodyMutexes, MaxBodyPairs, MaxContactConstraints, 
			*m_BroadPhaseLayerInterface, *m_ObjectVsBroadPhaseLayerFilter, *m_ObjectLayerPairFilter);

		// Create contact listener
		m_ContactListener = std::make_unique<JoltContactListener>(this);
		m_JoltSystem->SetContactListener(m_ContactListener.get());

		// Set default gravity
		m_JoltSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

		OLO_CORE_INFO("Jolt Physics initialized - MaxBodies: {0}, MaxBodyPairs: {1}, MaxContactConstraints: {2}", 
			MaxBodies, MaxBodyPairs, MaxContactConstraints);
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

}