#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine {

	/// @brief Material properties for physics colliders
	/// Defines surface interaction characteristics for collision response
	struct ColliderMaterial
	{
		f32 m_StaticFriction = 0.6f;    ///< Coefficient of static friction (0.0 = no friction, 1.0+ = high friction)
		f32 m_DynamicFriction = 0.6f;   ///< Coefficient of kinetic friction during sliding
		f32 m_Restitution = 0.0f;       ///< Bounciness factor (0.0 = no bounce, 1.0 = perfect bounce)
		f32 m_Density = 1000.0f;        ///< Material density in kg/m³ (water = 1000)

		ColliderMaterial() = default;
		ColliderMaterial(f32 staticFriction, f32 dynamicFriction, f32 restitution, f32 density = 1000.0f)
			: m_StaticFriction(staticFriction), m_DynamicFriction(dynamicFriction), m_Restitution(restitution), m_Density(density) {}
	};
}