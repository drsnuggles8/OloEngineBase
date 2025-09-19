#pragma once

#include "OloEngine/Physics3D/ColliderMaterial.h"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <concepts>
#include <algorithm>
#include <cmath>

// Forward declare components to avoid circular dependencies
namespace OloEngine {
	struct BoxCollider3DComponent;
	struct SphereCollider3DComponent;
	struct CapsuleCollider3DComponent;
}

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
		// Require that Material has getter methods that return values convertible to float
		{ collider.Material.GetStaticFriction() } -> std::convertible_to<float>;
		{ collider.Material.GetDynamicFriction() } -> std::convertible_to<float>;
		{ collider.Material.GetRestitution() } -> std::convertible_to<float>;
	};

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
		void SetRestitution(float restitution) 
		{ 
			// Clamp restitution to valid range [0.0f, 1.0f] to match ColliderMaterial validation
			m_Restitution = std::clamp(restitution, 0.0f, 1.0f); 
		}

		/// @brief Computes a single friction coefficient from static and dynamic friction values
		/// @param staticFriction The static friction coefficient
		/// @param dynamicFriction The dynamic friction coefficient  
		/// @return Combined friction value according to the current s_FrictionPolicy
		static float GetCombinedFriction(float staticFriction, float dynamicFriction)
		{
			// Sanitize inputs: treat negative or NaN values as 0.0f to protect Jolt Physics
			float cleanStaticFriction = (std::isnan(staticFriction) || !std::isfinite(staticFriction) || staticFriction < 0.0f) ? 0.0f : staticFriction;
			float cleanDynamicFriction = (std::isnan(dynamicFriction) || !std::isfinite(dynamicFriction) || dynamicFriction < 0.0f) ? 0.0f : dynamicFriction;

			switch (s_FrictionPolicy)
			{
				case FrictionCombinePolicy::UseStaticOnly:
					return cleanStaticFriction;
				case FrictionCombinePolicy::UseDynamicOnly:
					return cleanDynamicFriction;
				case FrictionCombinePolicy::UseMaximum:
					return std::max(cleanStaticFriction, cleanDynamicFriction);
				case FrictionCombinePolicy::UseAverage:
					return (cleanStaticFriction + cleanDynamicFriction) * 0.5f;
				case FrictionCombinePolicy::UseGeometricMean:
					return std::sqrt(cleanStaticFriction * cleanDynamicFriction);
				default:
					return std::max(cleanStaticFriction, cleanDynamicFriction); // Default to maximum
			}
		}

		inline static JPH::Ref<JoltMaterial> FromColliderMaterial(const ColliderMaterial& colliderMaterial)
		{
			float combinedFriction = GetCombinedFriction(colliderMaterial.GetStaticFriction(), colliderMaterial.GetDynamicFriction());
			return JPH::Ref<JoltMaterial>(new JoltMaterial(combinedFriction, colliderMaterial.GetRestitution()));
		}

		// Templated helper to create materials from any collider component with a Material member
		template<typename T> requires HasMaterialInterface<T>
		static JoltMaterial CreateFromCollider(const T& collider)
		{
			float combinedFriction = GetCombinedFriction(collider.Material.GetStaticFriction(), collider.Material.GetDynamicFriction());
			return JoltMaterial(combinedFriction, collider.Material.GetRestitution());
		}

		// Helper functions to create materials from our collider components
		static inline JoltMaterial CreateFromBoxCollider(const BoxCollider3DComponent& collider)
		{
			return CreateFromCollider(collider);
		}

		static inline JoltMaterial CreateFromSphereCollider(const SphereCollider3DComponent& collider)
		{
			return CreateFromCollider(collider);
		}

		static inline JoltMaterial CreateFromCapsuleCollider(const CapsuleCollider3DComponent& collider)
		{
			return CreateFromCollider(collider);
		}

	private:
		float m_Friction = 0.6f;
		float m_Restitution = 0.0f;
	};

}

// Include component definitions for inline method implementations
#include "OloEngine/Scene/Components.h"