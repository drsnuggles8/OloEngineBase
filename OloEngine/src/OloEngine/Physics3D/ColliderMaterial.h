#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine {

	struct ColliderMaterial
	{
		f32 StaticFriction = 0.6f;
		f32 DynamicFriction = 0.6f;
		f32 Restitution = 0.0f;
		f32 Density = 1000.0f; // kg/m^3

		ColliderMaterial() = default;
		ColliderMaterial(f32 staticFriction, f32 dynamicFriction, f32 restitution, f32 density = 1000.0f)
			: StaticFriction(staticFriction), DynamicFriction(dynamicFriction), Restitution(restitution), Density(density) {}
	};

}