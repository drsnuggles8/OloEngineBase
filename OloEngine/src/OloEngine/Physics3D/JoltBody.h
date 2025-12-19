#pragma once

#include "Physics3DTypes.h"
#include "JoltUtils.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Scene/Entity.h"

#include <concepts>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>

// Concept to constrain template types to be derived from JPH::Shape
// This enables compile-time type safety for shape management APIs
// while preserving backward compatibility with existing code
template<typename T>
concept JoltShape = std::derived_from<T, JPH::Shape>;

namespace OloEngine {

	class JoltScene; // Forward declaration

	class JoltBody : public RefCounted
	{
	public:
		JoltBody(Entity entity, JoltScene* scene);
		~JoltBody();

		// Entity access
		Entity GetEntity() const { return m_Entity; }
		UUID GetEntityID() const { return m_Entity.GetUUID(); }

		// Jolt body access
		JPH::BodyID GetBodyID() const { return m_BodyID; }
		[[nodiscard]] bool IsValid() const { return !m_BodyID.IsInvalid(); }

		// Body type
		bool IsStatic() const;
		bool IsDynamic() const;
		bool IsKinematic() const;
		void SetBodyType(EBodyType bodyType);
		EBodyType GetBodyType() const;

		// Collision properties
		void SetCollisionLayer(u32 layerID);
		u32 GetCollisionLayer() const;

		void SetTrigger(bool isTrigger);
		bool IsTrigger() const;

		// Transform
		glm::vec3 GetPosition() const;
		void SetPosition(const glm::vec3& position);
		
		glm::quat GetRotation() const;
		void SetRotation(const glm::quat& rotation);

		void SetTransform(const glm::vec3& position, const glm::quat& rotation);

		// For kinematic bodies
		void MoveKinematic(const glm::vec3& targetPosition, const glm::quat& targetRotation, f32 deltaTime);
		void Rotate(const glm::vec3& rotationTimesDeltaTime);

		// Mass properties (for dynamic bodies)
		f32 GetMass() const;
		void SetMass(f32 mass);

		// Drag properties
		void SetLinearDrag(f32 linearDrag);
		f32 GetLinearDrag() const;
		
		void SetAngularDrag(f32 angularDrag);
		f32 GetAngularDrag() const;

		// Velocity (for dynamic and kinematic bodies)
		glm::vec3 GetLinearVelocity() const;
		void SetLinearVelocity(const glm::vec3& velocity);

		glm::vec3 GetAngularVelocity() const;
		void SetAngularVelocity(const glm::vec3& velocity);

		// Velocity limits
		f32 GetMaxLinearVelocity() const;
		void SetMaxLinearVelocity(f32 maxVelocity);

		f32 GetMaxAngularVelocity() const;
		void SetMaxAngularVelocity(f32 maxVelocity);

		// Gravity (for dynamic bodies)
		bool GetGravityEnabled() const;
		void SetGravityEnabled(bool enabled);

		// Forces and impulses (for dynamic bodies)
		void AddForce(const glm::vec3& force, EForceMode forceMode = EForceMode::Force, bool forceWake = true);
		void AddForce(const glm::vec3& force, const glm::vec3& location, EForceMode forceMode = EForceMode::Force, bool forceWake = true);
		void AddTorque(const glm::vec3& torque, bool forceWake = true);
		void AddRadialImpulse(const glm::vec3& origin, f32 radius, f32 strength, EFalloffMode falloff, bool velocityChange);

		// Sleep state
		bool IsSleeping() const;
		void SetSleepState(bool sleep);

		// Collision detection
		void SetCollisionDetectionMode(ECollisionDetectionType collisionDetection);
		ECollisionDetectionType GetCollisionDetectionMode() const;

		// Axis locking
		void SetAxisLock(EActorAxis axis, bool locked, bool forceWake = true);
		bool IsAxisLocked(EActorAxis axis) const;
		EActorAxis GetLockedAxes() const;

		// Shape management
		/// @brief Set the shape for this body using a type-safe template
		/// @tparam T Shape type derived from JPH::Shape (constrained by JoltShape concept)
		/// @param shape The shape to set for this body
		/// @example body->SetShape(JPH::RefConst<JPH::BoxShape>(new JPH::BoxShape(halfExtents)));
		template<JoltShape T>
		void SetShape(JPH::RefConst<T> shape);
		
		/// @brief Set the shape for this body (backward compatibility)
		/// @param shape The base shape to set for this body
		void SetShape(JPH::RefConst<JPH::Shape> shape);
		
		/// @brief Get the shape from this body with compile-time type safety
		/// @tparam T Shape type derived from JPH::Shape (constrained by JoltShape concept)
		/// @return Typed shape reference, or nullptr if shape doesn't match type T
		/// @example auto boxShape = body->GetShape<JPH::BoxShape>();
		template<JoltShape T>
		JPH::RefConst<T> GetShape() const;
		
		/// @brief Get the shape from this body (backward compatibility)
		/// @return Base shape reference
		JPH::RefConst<JPH::Shape> GetShape() const;

		// Activation
		void Activate();
		void Deactivate();
		bool IsActive() const;

	private:
		// Internal Jolt body management
		void CreateJoltBody();
		void DestroyJoltBody();
		void UpdateBodyFromComponents();
		
		// Helper methods
		JPH::BodyInterface& GetBodyInterface();
		const JPH::BodyInterface& GetBodyInterface() const;
		const JPH::BodyLockInterface& GetBodyLockInterface() const;

		// Body creation helpers
		JPH::Ref<JPH::Shape> CreateShapeForBody() const;
		JPH::BodyCreationSettings CreateBodySettings(const TransformComponent& transformComponent, const Rigidbody3DComponent& rigidBodyComponent, JPH::Ref<JPH::Shape> shape) const;
		void SetupCreatedBody(JPH::BodyID bodyID, const Rigidbody3DComponent& rigidBodyComponent);

		// Material application
		void ApplyMaterialProperties(JPH::BodyCreationSettings& bodySettings) const;

		// Axis locking constraint management
		void CreateAxisLockConstraint(JPH::Body& body);
		void DestroyAxisLockConstraint();
		void OnAxisLockUpdated(bool forceWake);

	private:
		Entity m_Entity;
		JoltScene* m_Scene;
		JPH::BodyID m_BodyID;
		JPH::Ref<JPH::SixDOFConstraint> m_AxisLockConstraint = nullptr;
		
		// Cached properties
		EActorAxis m_LockedAxes = EActorAxis::None;
		bool m_GravityEnabled = true;
	};

	// Template implementations for concept-constrained shape methods
	template<JoltShape T>
	inline void JoltBody::SetShape(JPH::RefConst<T> shape)
	{
		// Convert to base JPH::Shape for implementation
		SetShape(JPH::RefConst<JPH::Shape>(shape.GetPtr()));
	}

	template<JoltShape T>
	inline JPH::RefConst<T> JoltBody::GetShape() const
	{
		// Get the base shape and cast to the requested type
		JPH::RefConst<JPH::Shape> baseShape = GetShape();
		if (!baseShape)
			return nullptr;
		
		// Dynamic cast to ensure type safety at runtime
		const T* derivedShape = dynamic_cast<const T*>(baseShape.GetPtr());
		if (!derivedShape)
			return nullptr;
		
		return JPH::RefConst<T>(derivedShape);
	}

}
