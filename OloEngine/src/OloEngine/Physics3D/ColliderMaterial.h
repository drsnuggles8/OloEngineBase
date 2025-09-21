#pragma once

#include "OloEngine/Core/Base.h"
#include <algorithm>
#include <cmath>

namespace OloEngine {

	/// @brief Material properties for physics colliders
	/// Defines surface interaction characteristics for collision response
	struct ColliderMaterial
	{
		static constexpr f32 MAX_FRICTION = 2.0f;   ///< Maximum allowed friction coefficient
		static constexpr f32 MIN_FRICTION = 0.0f;   ///< Minimum allowed friction coefficient
		static constexpr f32 MAX_RESTITUTION = 1.0f; ///< Maximum allowed restitution (perfect bounce)
		static constexpr f32 MIN_RESTITUTION = 0.0f; ///< Minimum allowed restitution (no bounce)
		static constexpr f32 MIN_DENSITY = 0.001f;  ///< Minimum allowed density (prevents zero/negative values)

		ColliderMaterial() 
		{ 
			Validate(); 
		}
		
		ColliderMaterial(f32 staticFriction, f32 dynamicFriction, f32 restitution, f32 density = 1000.0f)
			: m_StaticFriction(staticFriction), m_DynamicFriction(dynamicFriction), m_Restitution(restitution), m_Density(density) 
		{
			Validate();
		}

		/// @brief Get static friction coefficient
		f32 GetStaticFriction() const 
		{ 
			return m_StaticFriction; 
		}

		/// @brief Get dynamic friction coefficient  
		f32 GetDynamicFriction() const 
		{ 
			return m_DynamicFriction; 
		}

		/// @brief Get restitution (bounciness) factor
		f32 GetRestitution() const 
		{ 
			return m_Restitution; 
		}

		/// @brief Get material density in kg/m³
		f32 GetDensity() const 
		{ 
			return m_Density; 
		}

		/// @brief Set static friction with validation
		void SetStaticFriction(f32 friction) 
		{
			m_StaticFriction = friction;
			ValidateFriction();
		}

		/// @brief Set dynamic friction with validation
		void SetDynamicFriction(f32 friction) 
		{
			m_DynamicFriction = friction;
			ValidateFriction();
		}

		/// @brief Set restitution with validation
		void SetRestitution(f32 restitution) 
		{
			m_Restitution = restitution;
			ValidateRestitution();
		}

		/// @brief Set density (basic range check)
		void SetDensity(f32 density) 
		{
			m_Density = std::max(MIN_DENSITY, density); // Prevent zero/negative density
		}

		/// @brief Validate and clamp all material properties
		void Validate()
		{
			ValidateFriction();
			ValidateRestitution();
			SetDensity(m_Density); // Reuse density validation
		}

	private:
		f32 m_StaticFriction = 0.6f;    ///< Coefficient of static friction (0.0 = no friction, 1.0+ = high friction)
		f32 m_DynamicFriction = 0.6f;   ///< Coefficient of kinetic friction during sliding
		f32 m_Restitution = 0.0f;       ///< Bounciness factor (0.0 = no bounce, 1.0 = perfect bounce)
		f32 m_Density = 1000.0f;        ///< Material density in kg/m³ (water = 1000)
		/// @brief Clamp friction values to valid ranges and ensure physical constraints
		void ValidateFriction()
		{
			// Handle NaN/inf by replacing with default values
			if (!std::isfinite(m_StaticFriction))
				m_StaticFriction = 0.6f;
			if (!std::isfinite(m_DynamicFriction))
				m_DynamicFriction = 0.6f;

			// Clamp to valid ranges
			m_StaticFriction = std::clamp(m_StaticFriction, MIN_FRICTION, MAX_FRICTION);
			m_DynamicFriction = std::clamp(m_DynamicFriction, MIN_FRICTION, MAX_FRICTION);

			// Ensure dynamic friction doesn't exceed static friction (physical constraint)
			if (m_DynamicFriction > m_StaticFriction)
				m_DynamicFriction = m_StaticFriction;
		}

		/// @brief Clamp restitution to valid range
		void ValidateRestitution()
		{
			// Handle NaN/inf by replacing with default value
			if (!std::isfinite(m_Restitution))
				m_Restitution = 0.0f;

			// Clamp to valid range
			m_Restitution = std::clamp(m_Restitution, MIN_RESTITUTION, MAX_RESTITUTION);
		}
	};
}