#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>

namespace OloEngine
{
	// 3D Simplex noise — spatially coherent, suitable for particle turbulence
	// Based on the classic algorithm by Ken Perlin / Stefan Gustavson
	[[nodiscard]] f32 SimplexNoise3D(f32 x, f32 y, f32 z);

	// Convenience overload for glm::vec3
	[[nodiscard]] inline f32 SimplexNoise3D(const glm::vec3& v) { return SimplexNoise3D(v.x, v.y, v.z); }
}
