#include "OloEnginePCH.h"
#include "JoltBody.h"
#include "JoltScene.h"
#include "JoltShapes.h"
#include "JoltLayerInterface.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Scene/Components.h"

#include <cstdint>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>

namespace OloEngine {

	JoltBody::JoltBody(Entity entity, JoltScene* scene)
		: m_Entity(entity), m_Scene(scene), m_BodyID(JPH::BodyID())
	{
		OLO_CORE_ASSERT(entity, "JoltBody requires a valid entity");
		OLO_CORE_ASSERT(scene, "JoltBody requires a valid JoltScene");

		if (entity.HasComponent<RigidBody3DComponent>())
		{
			CreateJoltBody();
		}
	}

	JoltBody::~JoltBody()
	{
		DestroyJoltBody();
	}

	bool JoltBody::IsStatic() const
	{
		if (m_BodyID.IsInvalid()) return true;
		
		auto& bodyInterface = GetBodyInterface();
		return bodyInterface.GetMotionType(m_BodyID) == JPH::EMotionType::Static;
	}

	bool JoltBody::IsDynamic() const
	{
		if (m_BodyID.IsInvalid()) return false;
		
		auto& bodyInterface = GetBodyInterface();
		return bodyInterface.GetMotionType(m_BodyID) == JPH::EMotionType::Dynamic;
	}

	bool JoltBody::IsKinematic() const
	{
		if (m_BodyID.IsInvalid()) return false;
		
		auto& bodyInterface = GetBodyInterface();
		return bodyInterface.GetMotionType(m_BodyID) == JPH::EMotionType::Kinematic;
	}

	void JoltBody::SetBodyType(EBodyType bodyType)
	{
		if (m_BodyID.IsInvalid()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::EMotionType motionType = JoltUtils::ToJoltMotionType(bodyType);
		bodyInterface.SetMotionType(m_BodyID, motionType, JPH::EActivation::Activate);

		// Update component
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			component.Type = static_cast<BodyType3D>(bodyType);
		}
	}

	EBodyType JoltBody::GetBodyType() const
	{
		if (m_BodyID.IsInvalid()) return EBodyType::Static;

		auto& bodyInterface = GetBodyInterface();
		return JoltUtils::FromJoltMotionType(bodyInterface.GetMotionType(m_BodyID));
	}

	void JoltBody::SetCollisionLayer(u32 layerID)
	{
		if (m_BodyID.IsInvalid()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::ObjectLayer objectLayer = JoltLayerInterface::GetObjectLayerForCollider(layerID, GetBodyType(), IsTrigger());
		bodyInterface.SetObjectLayer(m_BodyID, objectLayer);

		// Update component
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			component.LayerID = layerID;
		}
	}

	u32 JoltBody::GetCollisionLayer() const
	{
		if (m_BodyID.IsInvalid()) return 0;

		// Get layer ID from component since it's the authoritative source
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			const auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			return component.LayerID;
		}
		return 0;
	}

	void JoltBody::SetTrigger(bool isTrigger)
	{
		if (m_BodyID.IsInvalid()) return;

		// In Jolt, sensor/trigger behavior is typically controlled through:
		// 1. Object layers during body creation
		// 2. Contact listener callbacks
		// For now, we'll store the state and handle it during recreation if needed
		
		// Update component
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			component.IsTrigger = isTrigger;
		}
	}

	bool JoltBody::IsTrigger() const
	{
		if (m_BodyID.IsInvalid()) return false;

		// Get trigger state from component since Jolt doesn't provide direct access
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			const auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			return component.IsTrigger;
		}
		return false;
	}

	glm::vec3 JoltBody::GetPosition() const
	{
		if (m_BodyID.IsInvalid()) return glm::vec3(0.0f);

		auto& bodyInterface = GetBodyInterface();
		JPH::RVec3 position = bodyInterface.GetCenterOfMassPosition(m_BodyID);
		return JoltUtils::FromJoltVector(position);
	}

	void JoltBody::SetPosition(const glm::vec3& position)
	{
		if (m_BodyID.IsInvalid()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::RVec3 joltPosition = JoltUtils::ToJoltVector(position);
		bodyInterface.SetPosition(m_BodyID, joltPosition, JPH::EActivation::DontActivate);
	}

	glm::quat JoltBody::GetRotation() const
	{
		if (m_BodyID.IsInvalid()) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		auto& bodyInterface = GetBodyInterface();
		JPH::Quat rotation = bodyInterface.GetRotation(m_BodyID);
		return JoltUtils::FromJoltQuat(rotation);
	}

	void JoltBody::SetRotation(const glm::quat& rotation)
	{
		if (m_BodyID.IsInvalid()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::Quat joltRotation = JoltUtils::ToJoltQuat(rotation);
		bodyInterface.SetRotation(m_BodyID, joltRotation, JPH::EActivation::DontActivate);
	}

	void JoltBody::SetTransform(const glm::vec3& position, const glm::quat& rotation)
	{
		if (m_BodyID.IsInvalid()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::RVec3 joltPosition = JoltUtils::ToJoltVector(position);
		JPH::Quat joltRotation = JoltUtils::ToJoltQuat(rotation);
		bodyInterface.SetPositionAndRotation(m_BodyID, joltPosition, joltRotation, JPH::EActivation::DontActivate);
	}

	void JoltBody::MoveKinematic(const glm::vec3& targetPosition, const glm::quat& targetRotation, f32 deltaTime)
	{
		if (m_BodyID.IsInvalid() || !IsKinematic()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::RVec3 joltPosition = JoltUtils::ToJoltVector(targetPosition);
		JPH::Quat joltRotation = JoltUtils::ToJoltQuat(targetRotation);
		bodyInterface.MoveKinematic(m_BodyID, joltPosition, joltRotation, deltaTime);
	}

	void JoltBody::Rotate(const glm::vec3& rotationTimesDeltaTime)
	{
		if (m_BodyID.IsInvalid()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::Vec3 joltRotation = JoltUtils::ToJoltVector(rotationTimesDeltaTime);
		
		// Get current rotation and apply delta
		JPH::Quat currentRotation = bodyInterface.GetRotation(m_BodyID);
		JPH::Quat deltaRotation = JPH::Quat::sRotation(joltRotation.Normalized(), joltRotation.Length());
		JPH::Quat newRotation = deltaRotation * currentRotation;
		
		bodyInterface.SetRotation(m_BodyID, newRotation, JPH::EActivation::Activate);
	}

	f32 JoltBody::GetMass() const
	{
		if (m_BodyID.IsInvalid()) return 0.0f;

		const auto& bodyLockInterface = GetBodyLockInterface();
		JPH::BodyLockRead lock(bodyLockInterface, m_BodyID);
		if (lock.Succeeded())
		{
			const JPH::Body& body = lock.GetBody();
			const JPH::MotionProperties* motionProperties = body.GetMotionProperties();
			if (motionProperties)
			{
				return 1.0f / motionProperties->GetInverseMass();
			}
		}
		return 0.0f;
	}

	void JoltBody::SetMass(f32 mass)
	{
		if (m_BodyID.IsInvalid() || !IsDynamic()) return;

		const auto& bodyLockInterface = GetBodyLockInterface();
		JPH::BodyLockWrite lock(bodyLockInterface, m_BodyID);
		if (lock.Succeeded())
		{
			JPH::Body& body = lock.GetBody();
			JPH::MotionProperties* motionProperties = body.GetMotionProperties();
			if (motionProperties)
			{
				motionProperties->SetInverseMass(1.0f / mass);
			}
		}

		// Update component
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			component.Mass = mass;
		}
	}

	void JoltBody::SetLinearDrag(f32 linearDrag)
	{
		if (m_BodyID.IsInvalid()) return;

		const auto& bodyLockInterface = GetBodyLockInterface();
		JPH::BodyLockWrite lock(bodyLockInterface, m_BodyID);
		if (lock.Succeeded())
		{
			JPH::Body& body = lock.GetBody();
			JPH::MotionProperties* motionProperties = body.GetMotionProperties();
			if (motionProperties)
			{
				motionProperties->SetLinearDamping(linearDrag);
			}
		}

		// Update component
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			component.LinearDrag = linearDrag;
		}
	}

	f32 JoltBody::GetLinearDrag() const
	{
		if (m_BodyID.IsInvalid()) return 0.0f;

		const auto& bodyLockInterface = GetBodyLockInterface();
		JPH::BodyLockRead lock(bodyLockInterface, m_BodyID);
		if (lock.Succeeded())
		{
			const JPH::Body& body = lock.GetBody();
			const JPH::MotionProperties* motionProperties = body.GetMotionProperties();
			if (motionProperties)
			{
				return motionProperties->GetLinearDamping();
			}
		}
		return 0.0f;
	}

	void JoltBody::SetAngularDrag(f32 angularDrag)
	{
		if (m_BodyID.IsInvalid()) return;

		const auto& bodyLockInterface = GetBodyLockInterface();
		JPH::BodyLockWrite lock(bodyLockInterface, m_BodyID);
		if (lock.Succeeded())
		{
			JPH::Body& body = lock.GetBody();
			JPH::MotionProperties* motionProperties = body.GetMotionProperties();
			if (motionProperties)
			{
				motionProperties->SetAngularDamping(angularDrag);
			}
		}

		// Update component
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			component.AngularDrag = angularDrag;
		}
	}

	f32 JoltBody::GetAngularDrag() const
	{
		if (m_BodyID.IsInvalid()) return 0.0f;

		const auto& bodyLockInterface = GetBodyLockInterface();
		JPH::BodyLockRead lock(bodyLockInterface, m_BodyID);
		if (lock.Succeeded())
		{
			const JPH::Body& body = lock.GetBody();
			const JPH::MotionProperties* motionProperties = body.GetMotionProperties();
			if (motionProperties)
			{
				return motionProperties->GetAngularDamping();
			}
		}
		return 0.0f;
	}

	glm::vec3 JoltBody::GetLinearVelocity() const
	{
		if (m_BodyID.IsInvalid()) return glm::vec3(0.0f);

		auto& bodyInterface = GetBodyInterface();
		JPH::Vec3 velocity = bodyInterface.GetLinearVelocity(m_BodyID);
		return JoltUtils::FromJoltVector(velocity);
	}

	void JoltBody::SetLinearVelocity(const glm::vec3& velocity)
	{
		if (m_BodyID.IsInvalid()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::Vec3 joltVelocity = JoltUtils::ToJoltVector(velocity);
		bodyInterface.SetLinearVelocity(m_BodyID, joltVelocity);
	}

	glm::vec3 JoltBody::GetAngularVelocity() const
	{
		if (m_BodyID.IsInvalid()) return glm::vec3(0.0f);

		auto& bodyInterface = GetBodyInterface();
		JPH::Vec3 velocity = bodyInterface.GetAngularVelocity(m_BodyID);
		return JoltUtils::FromJoltVector(velocity);
	}

	void JoltBody::SetAngularVelocity(const glm::vec3& velocity)
	{
		if (m_BodyID.IsInvalid()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::Vec3 joltVelocity = JoltUtils::ToJoltVector(velocity);
		bodyInterface.SetAngularVelocity(m_BodyID, joltVelocity);
	}

	f32 JoltBody::GetMaxLinearVelocity() const
	{
		if (m_BodyID.IsInvalid()) return 0.0f;

		const auto& bodyLockInterface = GetBodyLockInterface();
		JPH::BodyLockRead lock(bodyLockInterface, m_BodyID);
		if (lock.Succeeded())
		{
			const JPH::Body& body = lock.GetBody();
			const JPH::MotionProperties* motionProperties = body.GetMotionProperties();
			if (motionProperties)
			{
				return motionProperties->GetMaxLinearVelocity();
			}
		}
		return 0.0f;
	}

	void JoltBody::SetMaxLinearVelocity(f32 maxVelocity)
	{
		if (m_BodyID.IsInvalid()) return;

		const auto& bodyLockInterface = GetBodyLockInterface();
		JPH::BodyLockWrite lock(bodyLockInterface, m_BodyID);
		if (lock.Succeeded())
		{
			JPH::Body& body = lock.GetBody();
			JPH::MotionProperties* motionProperties = body.GetMotionProperties();
			if (motionProperties)
			{
				motionProperties->SetMaxLinearVelocity(maxVelocity);
			}
		}

		// Update component
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			component.MaxLinearVelocity = maxVelocity;
		}
	}

	f32 JoltBody::GetMaxAngularVelocity() const
	{
		if (m_BodyID.IsInvalid()) return 0.0f;

		const auto& bodyLockInterface = GetBodyLockInterface();
		JPH::BodyLockRead lock(bodyLockInterface, m_BodyID);
		if (lock.Succeeded())
		{
			const JPH::Body& body = lock.GetBody();
			const JPH::MotionProperties* motionProperties = body.GetMotionProperties();
			if (motionProperties)
			{
				return motionProperties->GetMaxAngularVelocity();
			}
		}
		return 0.0f;
	}

	void JoltBody::SetMaxAngularVelocity(f32 maxVelocity)
	{
		if (m_BodyID.IsInvalid()) return;

		const auto& bodyLockInterface = GetBodyLockInterface();
		JPH::BodyLockWrite lock(bodyLockInterface, m_BodyID);
		if (lock.Succeeded())
		{
			JPH::Body& body = lock.GetBody();
			JPH::MotionProperties* motionProperties = body.GetMotionProperties();
			if (motionProperties)
			{
				motionProperties->SetMaxAngularVelocity(maxVelocity);
			}
		}

		// Update component
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			component.MaxAngularVelocity = maxVelocity;
		}
	}

	bool JoltBody::GetGravityEnabled() const
	{
		return m_GravityEnabled;
	}

	void JoltBody::SetGravityEnabled(bool enabled)
	{
		if (m_BodyID.IsInvalid()) return;

		m_GravityEnabled = enabled;
		auto& bodyInterface = GetBodyInterface();
		bodyInterface.SetGravityFactor(m_BodyID, enabled ? 1.0f : 0.0f);

		// Update component
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			component.DisableGravity = !enabled;
		}
	}

	void JoltBody::AddForce(const glm::vec3& force, EForceMode forceMode, bool forceWake)
	{
		if (m_BodyID.IsInvalid() || !IsDynamic()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::Vec3 joltForce = JoltUtils::ToJoltVector(force);

		switch (forceMode)
		{
			case EForceMode::Force:
				bodyInterface.AddForce(m_BodyID, joltForce);
				break;
			case EForceMode::Impulse:
				bodyInterface.AddImpulse(m_BodyID, joltForce);
				break;
			case EForceMode::VelocityChange:
				// For velocity change, we need to convert to impulse
				{
					f32 mass = GetMass();
					bodyInterface.AddImpulse(m_BodyID, joltForce * mass);
				}
				break;
			case EForceMode::Acceleration:
				// For acceleration, we need to convert to force
				{
					f32 mass = GetMass();
					bodyInterface.AddForce(m_BodyID, joltForce * mass);
				}
				break;
		}

		if (forceWake)
		{
			Activate();
		}
	}

	void JoltBody::AddForce(const glm::vec3& force, const glm::vec3& location, EForceMode forceMode, bool forceWake)
	{
		if (m_BodyID.IsInvalid() || !IsDynamic()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::Vec3 joltForce = JoltUtils::ToJoltVector(force);
		JPH::RVec3 joltLocation = JoltUtils::ToJoltVector(location);

		switch (forceMode)
		{
			case EForceMode::Force:
				bodyInterface.AddForce(m_BodyID, joltForce, joltLocation);
				break;
			case EForceMode::Impulse:
				bodyInterface.AddImpulse(m_BodyID, joltForce, joltLocation);
				break;
			case EForceMode::VelocityChange:
				// For velocity change, we need to convert to impulse
				{
					f32 mass = GetMass();
					bodyInterface.AddImpulse(m_BodyID, joltForce * mass, joltLocation);
				}
				break;
			case EForceMode::Acceleration:
				// For acceleration, we need to convert to force
				{
					f32 mass = GetMass();
					bodyInterface.AddForce(m_BodyID, joltForce * mass, joltLocation);
				}
				break;
		}

		if (forceWake)
		{
			Activate();
		}
	}

	void JoltBody::AddTorque(const glm::vec3& torque, bool forceWake)
	{
		if (m_BodyID.IsInvalid() || !IsDynamic()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::Vec3 joltTorque = JoltUtils::ToJoltVector(torque);
		bodyInterface.AddTorque(m_BodyID, joltTorque);

		if (forceWake)
		{
			Activate();
		}
	}

	void JoltBody::AddRadialImpulse(const glm::vec3& origin, f32 radius, f32 strength, EFalloffMode falloff, bool velocityChange)
	{
		if (m_BodyID.IsInvalid() || !IsDynamic()) return;

		glm::vec3 bodyPosition = GetPosition();
		glm::vec3 direction = bodyPosition - origin;
		f32 distance = glm::length(direction);

		if (distance > radius || distance < 0.001f) return;

		direction = glm::normalize(direction);

		f32 impulse = strength;
		if (falloff == EFalloffMode::Linear)
		{
			impulse *= (1.0f - (distance / radius));
		}

		if (velocityChange)
		{
			AddForce(direction * impulse, EForceMode::VelocityChange, true);
		}
		else
		{
			AddForce(direction * impulse, EForceMode::Impulse, true);
		}
	}

	bool JoltBody::IsSleeping() const
	{
		if (m_BodyID.IsInvalid()) return true;

		auto& bodyInterface = GetBodyInterface();
		return !bodyInterface.IsActive(m_BodyID);
	}

	void JoltBody::SetSleepState(bool sleep)
	{
		if (m_BodyID.IsInvalid()) return;

		if (sleep)
		{
			Deactivate();
		}
		else
		{
			Activate();
		}
	}

	void JoltBody::SetCollisionDetectionMode(ECollisionDetectionType collisionDetection)
	{
		if (m_BodyID.IsInvalid()) return;

		auto& bodyInterface = GetBodyInterface();
		JPH::EMotionQuality motionQuality = JoltUtils::ToJoltMotionQuality(collisionDetection);
		bodyInterface.SetMotionQuality(m_BodyID, motionQuality);
	}

	ECollisionDetectionType JoltBody::GetCollisionDetectionMode() const
	{
		if (m_BodyID.IsInvalid()) return ECollisionDetectionType::Discrete;

		auto& bodyInterface = GetBodyInterface();
		JPH::EMotionQuality motionQuality = bodyInterface.GetMotionQuality(m_BodyID);
		
		switch (motionQuality)
		{
			case JPH::EMotionQuality::Discrete:
				return ECollisionDetectionType::Discrete;
			case JPH::EMotionQuality::LinearCast:
				return ECollisionDetectionType::Continuous;
			default:
				return ECollisionDetectionType::Discrete;
		}
	}

	void JoltBody::SetAxisLock(EActorAxis axis, bool locked, bool forceWake)
	{
		if (locked)
		{
			m_LockedAxes = static_cast<EActorAxis>(static_cast<u32>(m_LockedAxes) | static_cast<u32>(axis));
		}
		else
		{
			m_LockedAxes = static_cast<EActorAxis>(static_cast<u32>(m_LockedAxes) & ~static_cast<u32>(axis));
		}

		// Update component
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			component.LockedAxes = m_LockedAxes;
		}

		OnAxisLockUpdated(forceWake);
	}

	bool JoltBody::IsAxisLocked(EActorAxis axis) const
	{
		return (static_cast<u32>(m_LockedAxes) & static_cast<u32>(axis)) != 0;
	}

	EActorAxis JoltBody::GetLockedAxes() const
	{
		return m_LockedAxes;
	}

	void JoltBody::OnAxisLockUpdated(bool forceWake)
	{
		if (m_BodyID.IsInvalid()) return;

		// Recreate the axis lock constraint with new settings
		DestroyAxisLockConstraint();

		if (m_LockedAxes != EActorAxis::None && !IsStatic())
		{
			const auto& bodyLockInterface = GetBodyLockInterface();
			JPH::BodyLockWrite lock(bodyLockInterface, m_BodyID);
			if (lock.Succeeded())
			{
				JPH::Body& body = lock.GetBody();
				CreateAxisLockConstraint(body);
			}
		}

		if (forceWake)
		{
			Activate();
		}
	}

	void JoltBody::CreateAxisLockConstraint(JPH::Body& body)
	{
		if (m_AxisLockConstraint != nullptr) return; // Already created

		JPH::SixDOFConstraintSettings constraintSettings;
		constraintSettings.mPosition1 = constraintSettings.mPosition2 = body.GetCenterOfMassPosition();

		// Lock translation axes
		if ((m_LockedAxes & EActorAxis::TranslationX) != EActorAxis::None)
			constraintSettings.MakeFixedAxis(JPH::SixDOFConstraintSettings::TranslationX);

		if ((m_LockedAxes & EActorAxis::TranslationY) != EActorAxis::None)
			constraintSettings.MakeFixedAxis(JPH::SixDOFConstraintSettings::TranslationY);

		if ((m_LockedAxes & EActorAxis::TranslationZ) != EActorAxis::None)
			constraintSettings.MakeFixedAxis(JPH::SixDOFConstraintSettings::TranslationZ);

		// Lock rotation axes
		if ((m_LockedAxes & EActorAxis::RotationX) != EActorAxis::None)
			constraintSettings.MakeFixedAxis(JPH::SixDOFConstraintSettings::RotationX);

		if ((m_LockedAxes & EActorAxis::RotationY) != EActorAxis::None)
			constraintSettings.MakeFixedAxis(JPH::SixDOFConstraintSettings::RotationY);

		if ((m_LockedAxes & EActorAxis::RotationZ) != EActorAxis::None)
			constraintSettings.MakeFixedAxis(JPH::SixDOFConstraintSettings::RotationZ);

		// Create the constraint between the body and a fixed point in world space
		m_AxisLockConstraint = static_cast<JPH::SixDOFConstraint*>(constraintSettings.Create(JPH::Body::sFixedToWorld, body));

		// Add the constraint to the physics system
		m_Scene->GetJoltSystem().AddConstraint(m_AxisLockConstraint);
	}

	void JoltBody::DestroyAxisLockConstraint()
	{
		if (m_AxisLockConstraint != nullptr)
		{
			m_Scene->GetJoltSystem().RemoveConstraint(m_AxisLockConstraint);
			m_AxisLockConstraint = nullptr;
		}
	}

	void JoltBody::SetShape(JPH::Ref<JPH::Shape> shape)
	{
		if (m_BodyID.IsInvalid() || !shape) return;

		auto& bodyInterface = GetBodyInterface();
		bodyInterface.SetShape(m_BodyID, shape, true, JPH::EActivation::Activate);
	}

	JPH::Ref<JPH::Shape> JoltBody::GetShape() const
	{
		if (m_BodyID.IsInvalid()) return nullptr;

		const auto& bodyInterface = GetBodyInterface();
		JPH::RefConst<JPH::Shape> constShape = bodyInterface.GetShape(m_BodyID);
		return const_cast<JPH::Shape*>(constShape.GetPtr());
	}

	void JoltBody::Activate()
	{
		if (m_BodyID.IsInvalid()) return;

		auto& bodyInterface = GetBodyInterface();
		bodyInterface.ActivateBody(m_BodyID);
	}

	void JoltBody::Deactivate()
	{
		if (m_BodyID.IsInvalid()) return;

		auto& bodyInterface = GetBodyInterface();
		bodyInterface.DeactivateBody(m_BodyID);
	}

	bool JoltBody::IsActive() const
	{
		if (m_BodyID.IsInvalid()) return false;

		auto& bodyInterface = GetBodyInterface();
		return bodyInterface.IsActive(m_BodyID);
	}

	void JoltBody::CreateJoltBody()
	{
		if (!m_BodyID.IsInvalid()) return; // Already created

		auto& rigidBodyComponent = m_Entity.GetComponent<RigidBody3DComponent>();
		auto& transformComponent = m_Entity.GetComponent<TransformComponent>();

		// Create shape for the entity
		JPH::Ref<JPH::Shape> shape = JoltShapes::CreateShapeForEntity(m_Entity);
		if (!shape)
		{
			OLO_CORE_ERROR("Failed to create shape for entity {0}", (u64)m_Entity.GetUUID());
			return;
		}

		// Get transform
		glm::vec3 position = transformComponent.Translation;
		glm::quat rotation = glm::quat(transformComponent.Rotation);

		// Convert body type
		EBodyType bodyType = static_cast<EBodyType>(rigidBodyComponent.Type);
		JPH::EMotionType motionType = JoltUtils::ToJoltMotionType(bodyType);

		// Get object layer using the physics layer system
		JPH::ObjectLayer objectLayer = JoltLayerInterface::GetObjectLayerForCollider(rigidBodyComponent.LayerID, bodyType, rigidBodyComponent.IsTrigger);

		// Create body creation settings
		JPH::BodyCreationSettings bodySettings(
			shape,
			JoltUtils::ToJoltVector(position),
			JoltUtils::ToJoltQuat(rotation),
			motionType,
			objectLayer
		);

		// Set additional properties
		bodySettings.mIsSensor = rigidBodyComponent.IsTrigger;
		bodySettings.mGravityFactor = rigidBodyComponent.DisableGravity ? 0.0f : 1.0f;
		bodySettings.mLinearDamping = rigidBodyComponent.LinearDrag;
		bodySettings.mAngularDamping = rigidBodyComponent.AngularDrag;
		bodySettings.mUserData = static_cast<u64>(m_Entity.GetUUID());

		// Apply material properties from collider components
		ApplyMaterialProperties(bodySettings);

		if (motionType == JPH::EMotionType::Dynamic)
		{
			bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			bodySettings.mMassPropertiesOverride.mMass = rigidBodyComponent.Mass;
		}

		// Create the body
		m_BodyID = m_Scene->GetBodyInterface().CreateAndAddBody(bodySettings, JPH::EActivation::Activate);

		if (m_BodyID.IsInvalid())
		{
			OLO_CORE_ERROR("Failed to create Jolt body for entity {0}", (u64)m_Entity.GetUUID());
			return;
		}

		// Set initial velocities
		if (motionType != JPH::EMotionType::Static)
		{
			SetLinearVelocity(rigidBodyComponent.InitialLinearVelocity);
			SetAngularVelocity(rigidBodyComponent.InitialAngularVelocity);
			SetMaxLinearVelocity(rigidBodyComponent.MaxLinearVelocity);
			SetMaxAngularVelocity(rigidBodyComponent.MaxAngularVelocity);
		}

		// Store BodyID in component for easy access
		rigidBodyComponent.RuntimeBody = reinterpret_cast<void*>(static_cast<std::uintptr_t>(m_BodyID.GetIndexAndSequenceNumber()));

		// Cache initial state
		m_GravityEnabled = !rigidBodyComponent.DisableGravity;
		m_LockedAxes = rigidBodyComponent.LockedAxes;

		// Create axis lock constraint if needed
		if (m_LockedAxes != EActorAxis::None && motionType != JPH::EMotionType::Static)
		{
			const auto& bodyLockInterface = GetBodyLockInterface();
			JPH::BodyLockWrite lock(bodyLockInterface, m_BodyID);
			if (lock.Succeeded())
			{
				JPH::Body& body = lock.GetBody();
				CreateAxisLockConstraint(body);
			}
		}

		OLO_CORE_TRACE("Created Jolt body for entity {0}, BodyID: {1}", (u64)m_Entity.GetUUID(), m_BodyID.GetIndex());
	}

	void JoltBody::DestroyJoltBody()
	{
		if (m_BodyID.IsInvalid()) return;

		// Destroy axis lock constraint first
		DestroyAxisLockConstraint();

		auto& bodyInterface = m_Scene->GetBodyInterface();
		bodyInterface.RemoveBody(m_BodyID);
		bodyInterface.DestroyBody(m_BodyID);

		// Clear runtime body reference in component
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			component.RuntimeBody = nullptr;
		}

		m_BodyID = JPH::BodyID();
		OLO_CORE_TRACE("Destroyed Jolt body for entity {0}", (u64)m_Entity.GetUUID());
	}

	void JoltBody::UpdateBodyFromComponents()
	{
		if (m_BodyID.IsInvalid()) return;

		// Update properties from components
		if (m_Entity.HasComponent<RigidBody3DComponent>())
		{
			const auto& component = m_Entity.GetComponent<RigidBody3DComponent>();
			
			SetMass(component.Mass);
			SetLinearDrag(component.LinearDrag);
			SetAngularDrag(component.AngularDrag);
			SetGravityEnabled(!component.DisableGravity);
			SetTrigger(component.IsTrigger);
			SetMaxLinearVelocity(component.MaxLinearVelocity);
			SetMaxAngularVelocity(component.MaxAngularVelocity);
		}

		// Update shape if colliders changed
		JPH::Ref<JPH::Shape> newShape = JoltShapes::CreateShapeForEntity(m_Entity);
		if (newShape)
		{
			SetShape(newShape);
		}
	}

	JPH::BodyInterface& JoltBody::GetBodyInterface()
	{
		return m_Scene->GetBodyInterface();
	}

	const JPH::BodyInterface& JoltBody::GetBodyInterface() const
	{
		return m_Scene->GetBodyInterface();
	}

	const JPH::BodyLockInterface& JoltBody::GetBodyLockInterface()
	{
		return m_Scene->GetBodyLockInterface();
	}

	const JPH::BodyLockInterface& JoltBody::GetBodyLockInterface() const
	{
		return m_Scene->GetBodyLockInterface();
	}

	void JoltBody::ApplyMaterialProperties(JPH::BodyCreationSettings& bodySettings)
	{
		// Default material properties
		f32 friction = 0.5f;
		f32 restitution = 0.0f;

		// Check for collider components and use their material properties
		// Priority: Box > Sphere > Capsule > other colliders (first found wins)
		
		if (m_Entity.HasComponent<BoxCollider3DComponent>())
		{
			const auto& collider = m_Entity.GetComponent<BoxCollider3DComponent>();
			friction = collider.Material.m_StaticFriction;
			restitution = collider.Material.m_Restitution;
		}
		else if (m_Entity.HasComponent<SphereCollider3DComponent>())
		{
			const auto& collider = m_Entity.GetComponent<SphereCollider3DComponent>();
			friction = collider.Material.m_StaticFriction;
			restitution = collider.Material.m_Restitution;
		}
		else if (m_Entity.HasComponent<CapsuleCollider3DComponent>())
		{
			const auto& collider = m_Entity.GetComponent<CapsuleCollider3DComponent>();
			friction = collider.Material.m_StaticFriction;
			restitution = collider.Material.m_Restitution;
		}
		else if (m_Entity.HasComponent<MeshCollider3DComponent>())
		{
			const auto& collider = m_Entity.GetComponent<MeshCollider3DComponent>();
			friction = collider.Material.m_StaticFriction;
			restitution = collider.Material.m_Restitution;
		}
		else if (m_Entity.HasComponent<ConvexMeshCollider3DComponent>())
		{
			const auto& collider = m_Entity.GetComponent<ConvexMeshCollider3DComponent>();
			friction = collider.Material.m_StaticFriction;
			restitution = collider.Material.m_Restitution;
		}
		else if (m_Entity.HasComponent<TriangleMeshCollider3DComponent>())
		{
			const auto& collider = m_Entity.GetComponent<TriangleMeshCollider3DComponent>();
			friction = collider.Material.m_StaticFriction;
			restitution = collider.Material.m_Restitution;
		}

		// Apply the material properties to the body settings
		bodySettings.mFriction = friction;
		bodySettings.mRestitution = restitution;
	}

}