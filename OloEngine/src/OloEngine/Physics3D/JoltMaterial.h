#pragma once

#include "OloEngine/Physics3D/ColliderMaterial.h"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <concepts>
#include <algorithm>
#include <cmath>

namespace OloEngine {

	/// @brief Policy for combining static and dynamic friction coefficients into a single value for Jolt Physics
	enum class FrictionCombinePolicy : u8
	{
		UseStaticOnly = 0,          ///< Use only static friction (legacy behavior)
		UseDynamicOnly,             ///< Use only dynamic friction  
		UseMaximum,                 ///< Use the maximum of static and dynamic friction (default)
		UseAverage,                 ///< Use the average of static and dynamic friction
		UseGeometricMean            ///< Use the geometric mean of static and dynamic friction
	};

	// Concept to constrain types that have a Material member with the required physics properties
	template<typename T>
	concept HasMaterialInterface = requires(const T& collider) {
		// Require that T has a Material member
		collider.Material;
		// Require that Material has StaticFriction, DynamicFriction, and Restitution members that are convertible to float
		{ collider.Material.m_StaticFriction } -> std::convertible_to<float>;
		{ collider.Material.m_DynamicFriction } -> std::convertible_to<float>;
		{ collider.Material.m_Restitution } -> std::convertible_to<float>;
	};

	// Forward declarations
	struct BoxCollider3DComponent;
	struct SphereCollider3DComponent;
	struct CapsuleCollider3DComponent;

	class JoltMaterial : public JPH::PhysicsMaterial
	{
	public:
		/// @brief Global policy for combining static and dynamic friction coefficients
		/// Can be modified at runtime to change friction behavior globally
		static FrictionCombinePolicy s_FrictionPolicy;

		JoltMaterial() = default;
		JoltMaterial(float friction, float restitution)
			: m_Friction(friction), m_Restitution(restitution)
		{}

		float GetFriction() const { return m_Friction; }
		void SetFriction(float friction) { m_Friction = friction; }

		float GetRestitution() const { return m_Restitution; }
		void SetRestitution(float restitution) { m_Restitution = restitution; }

		/// @brief Computes a single friction coefficient from static and dynamic friction values
		/// @param staticFriction The static friction coefficient
		/// @param dynamicFriction The dynamic friction coefficient  
		/// @return Combined friction value according to the current s_FrictionPolicy
		static float GetCombinedFriction(float staticFriction, float dynamicFriction)
		{
			switch (s_FrictionPolicy)
			{
				case FrictionCombinePolicy::UseStaticOnly:
					return staticFriction;
				case FrictionCombinePolicy::UseDynamicOnly:
					return dynamicFriction;
				case FrictionCombinePolicy::UseMaximum:
					return std::max(staticFriction, dynamicFriction);
				case FrictionCombinePolicy::UseAverage:
					return (staticFriction + dynamicFriction) * 0.5f;
				case FrictionCombinePolicy::UseGeometricMean:
					return std::sqrt(staticFriction * dynamicFriction);
				default:
					return std::max(staticFriction, dynamicFriction); // Default to maximum
			}
		}

		float GetFriction() const { return m_Friction; }
		void SetFriction(float friction) { m_Friction = friction; }

		float GetRestitution() const { return m_Restitution; }
		void SetRestitution(float restitution) { m_Restitution = restitution; }

		inline static JPH::Ref<JoltMaterial> FromColliderMaterial(const ColliderMaterial& colliderMaterial)
		{
			float combinedFriction = GetCombinedFriction(colliderMaterial.m_StaticFriction, colliderMaterial.m_DynamicFriction);
			return JPH::Ref<JoltMaterial>(new JoltMaterial(combinedFriction, colliderMaterial.m_Restitution));
		}

		// Templated helper to create materials from any collider component with a Material member
		template<typename T> requires HasMaterialInterface<T>
		static JoltMaterial CreateFromCollider(const T& collider)
		{
			float combinedFriction = GetCombinedFriction(collider.Material.m_StaticFriction, collider.Material.m_DynamicFriction);
			return JoltMaterial(combinedFriction, collider.Material.m_Restitution);
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