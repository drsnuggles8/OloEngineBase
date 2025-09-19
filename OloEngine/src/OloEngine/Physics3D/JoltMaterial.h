#pragma once

#include "OloEngine/Physics3D/ColliderMaterial.h"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <concepts>

namespace OloEngine {

	// Concept to constrain types that have a Material member with the required physics properties
	template<typename T>
	concept HasMaterialInterface = requires(const T& collider) {
		// Require that T has a Material member
		collider.Material;
		// Require that Material has StaticFriction and Restitution members that are convertible to float
		{ collider.Material.m_StaticFriction } -> std::convertible_to<float>;
		{ collider.Material.m_Restitution } -> std::convertible_to<float>;
	};

	// Forward declarations
	struct BoxCollider3DComponent;
	struct SphereCollider3DComponent;
	struct CapsuleCollider3DComponent;

	class JoltMaterial : public JPH::PhysicsMaterial
	{
	public:
		JoltMaterial() = default;
		JoltMaterial(float friction, float restitution)
			: m_Friction(friction), m_Restitution(restitution)
		{}

		float GetFriction() const { return m_Friction; }
		void SetFriction(float friction) { m_Friction = friction; }

		float GetRestitution() const { return m_Restitution; }
		void SetRestitution(float restitution) { m_Restitution = restitution; }

		inline static JPH::Ref<JoltMaterial> FromColliderMaterial(const ColliderMaterial& colliderMaterial)
		{
			return JPH::Ref<JoltMaterial>(new JoltMaterial(colliderMaterial.m_StaticFriction, colliderMaterial.m_Restitution));
		}

		// Templated helper to create materials from any collider component with a Material member
		template<typename T> requires HasMaterialInterface<T>
		static JoltMaterial CreateFromCollider(const T& collider)
		{
			return JoltMaterial(collider.Material.m_StaticFriction, collider.Material.m_Restitution);
		}

		// Helper functions to create materials from our collider components
		static JoltMaterial CreateFromBoxCollider(const BoxCollider3DComponent& collider);
		static JoltMaterial CreateFromSphereCollider(const SphereCollider3DComponent& collider);
		static JoltMaterial CreateFromCapsuleCollider(const CapsuleCollider3DComponent& collider);

	private:
		float m_Friction = 0.6f;
		float m_Restitution = 0.0f;
	};

}