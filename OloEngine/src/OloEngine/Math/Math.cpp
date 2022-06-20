// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Math.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include <cmath>

namespace OloEngine::Math {

	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& translation, glm::vec3& rotation, glm::vec3& scale)
	{
		// From glm::decompose in matrix_decompose.inl

		using T = float;

		glm::mat4 LocalMatrix(transform);

		// Normalize the matrix.
		if (glm::epsilonEqual(LocalMatrix[3][3], static_cast<float>(0), glm::epsilon<T>()))
		{
			return false;
		}

		// First, isolate perspective.  This is the messiest.
		if (
			glm::epsilonNotEqual(LocalMatrix[0][3], static_cast<T>(0), glm::epsilon<T>()) ||
			glm::epsilonNotEqual(LocalMatrix[1][3], static_cast<T>(0), glm::epsilon<T>()) ||
			glm::epsilonNotEqual(LocalMatrix[2][3], static_cast<T>(0), glm::epsilon<T>()))
		{
			// Clear the perspective partition
			LocalMatrix[0][3] = LocalMatrix[1][3] = LocalMatrix[2][3] = static_cast<T>(0);
			LocalMatrix[3][3] = static_cast<T>(1);
		}

		// Next take care of translation (easy).
		translation = glm::vec3(LocalMatrix[3]);
		LocalMatrix[3] = glm::vec4(0, 0, 0, LocalMatrix[3].w);

		glm::vec3 Row[3], Pdum3;

		// Now get scale and shear.
		for (glm::length_t i = 0; i < 3; ++i)
		{
			for (glm::length_t j = 0; j < 3; ++j)
			{
				Row[i][j] = LocalMatrix[i][j];
			}
		}

		// Compute X scale factor and normalize first row.
		scale.x = glm::length(Row[0]);
		Row[0] = glm::detail::scale(Row[0], static_cast<T>(1));
		scale.y = glm::length(Row[1]);
		Row[1] = glm::detail::scale(Row[1], static_cast<T>(1));
		scale.z = glm::length(Row[2]);
		Row[2] = glm::detail::scale(Row[2], static_cast<T>(1));

		// At this point, the matrix (in rows[]) is orthonormal.
		// Check for a coordinate system flip.  If the determinant
		// is -1, then negate the matrix and the scaling factors.
		Pdum3 = glm::cross(Row[1], Row[2]); // v3Cross(row[1], row[2], Pdum3);
		if (glm::dot(Row[0], Pdum3) < 0)
		{
			for (glm::length_t i = 0; i < 3; ++i)
			{
				scale[i] *= static_cast<T>(-1);
				Row[i] *= static_cast<T>(-1);
			}
		}

		rotation.y = std::asin(-Row[0][2]);
		if (const double epsilon = 1e-5; std::abs(std::cos(rotation.y) - 0) > epsilon)
		{
			rotation.x = std::atan2(Row[1][2], Row[2][2]);
			rotation.z = std::atan2(Row[0][1], Row[0][0]);
		}
		else
		{
			rotation.x = std::atan2(-Row[2][0], Row[1][1]);
			rotation.z = 0;
		}


		return true;
	}

}
