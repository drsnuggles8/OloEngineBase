#pragma once

#include "Physics3DTypes.h"
#include "JoltUtils.h"
#include "JoltLayerInterface.h"
#include "JoltContactListener.h"
#include "JoltBody.h"
#include "JoltCharacterController.h"
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

namespace OloEngine {

	class Scene; // Forward declaration

	class JoltScene
	{
	public:
		JoltScene(Scene* scene);
		~JoltScene();

		// Initialization
		void Initialize();
		void Shutdown();
		bool IsInitialized() const { return m_Initialized; }

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

		// Character controller management
		Ref<JoltCharacterController> CreateCharacterController(Entity entity, const ContactCallbackFn& contactCallback = nullptr);
		void DestroyCharacterController(Entity entity);
		Ref<JoltCharacterController> GetCharacterController(Entity entity);
		Ref<JoltCharacterController> GetCharacterControllerByEntityID(UUID entityID);

		// Layer interface access for character controllers
		JPH::PhysicsSystem* GetPhysicsSystem() const { return m_JoltSystem.get(); }

		// Scene lifecycle
		void OnRuntimeStart();
		void OnRuntimeStop();
		void OnSimulationStart();
		void OnSimulationStop();

		// Scene queries
		bool CastRay(const RayCastInfo& rayInfo, SceneQueryHit& outHit);
		bool CastShape(const ShapeCastInfo& shapeInfo, SceneQueryHit& outHit);
		i32 OverlapShape(const ShapeOverlapInfo& overlapInfo, SceneQueryHit* outHits, i32 maxHits);

		// Radial impulse
		void AddRadialImpulse(const glm::vec3& origin, f32 radius, f32 strength, EFalloffMode falloff, bool velocityChange);

		// Entity teleportation
		void Teleport(Entity entity, const glm::vec3& targetPosition, const glm::quat& targetRotation, bool force = false);

		// Transform synchronization
		void SynchronizeTransforms();

		// Contact events
		void OnContactEvent(ContactType type, UUID entityA, UUID entityB);

		// Jolt system access
		JPH::BodyInterface& GetBodyInterface() { return m_JoltSystem->GetBodyInterface(); }
		const JPH::BodyInterface& GetBodyInterface() const { return m_JoltSystem->GetBodyInterface(); }
		const JPH::BodyLockInterface& GetBodyLockInterface() const { return m_JoltSystem->GetBodyLockInterface(); }
		JPH::PhysicsSystem& GetJoltSystem() { return *m_JoltSystem; }
		const JPH::PhysicsSystem& GetJoltSystem() const { return *m_JoltSystem; }

		// Debug info
		u32 GetBodyCount() const { return m_JoltSystem ? m_JoltSystem->GetNumBodies() : 0; }
		u32 GetActiveBodyCount() const { return m_JoltSystem ? m_JoltSystem->GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0; }

	private:
		void CreateRigidBodies();
		void SynchronizeBody(Ref<JoltBody> body);

		// Internal Jolt setup
		void InitializeJolt();
		void ShutdownJolt();

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
		static constexpr u32 MaxBodies = 65536;
		static constexpr u32 NumBodyMutexes = 0; // Autodetect
		static constexpr u32 MaxBodyPairs = 65536;
		static constexpr u32 MaxContactConstraints = 10240;
		static constexpr u32 TempAllocatorSize = 10 * 1024 * 1024; // 10 MB
		static constexpr u32 JobSystemMaxJobs = 2048;
		static constexpr u32 JobSystemMaxBarriers = 8;
	};

}