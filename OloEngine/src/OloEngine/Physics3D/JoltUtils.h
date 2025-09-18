#pragma once

#include "Physics3DTypes.h"
#include "OloEngine/Core/Base.h"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Body/MotionQuality.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace OloEngine {

	class JoltUtils
	{
	public:
		// Jolt Physics constants
		static constexpr u32 kMaxJoltLayers = 32;

		// Delete constructors and assignment operators to prevent instantiation
		JoltUtils() = delete;
		JoltUtils(const JoltUtils&) = delete;
		JoltUtils(JoltUtils&&) = delete;
		JoltUtils& operator=(const JoltUtils&) = delete;
		JoltUtils& operator=(JoltUtils&&) = delete;

		// GLM to Jolt conversions
		static JPH::Vec3 ToJoltVector(const glm::vec3& vector)
		{
			return JPH::Vec3(vector.x, vector.y, vector.z);
		}

		static JPH::Quat ToJoltQuat(const glm::quat& quat)
		{
			return JPH::Quat(quat.x, quat.y, quat.z, quat.w);
		}

		static JPH::Mat44 ToJoltMatrix(const glm::mat4& matrix)
		{
			return JPH::Mat44(
				JPH::Vec4(matrix[0][0], matrix[0][1], matrix[0][2], matrix[0][3]),
				JPH::Vec4(matrix[1][0], matrix[1][1], matrix[1][2], matrix[1][3]),
				JPH::Vec4(matrix[2][0], matrix[2][1], matrix[2][2], matrix[2][3]),
				JPH::Vec4(matrix[3][0], matrix[3][1], matrix[3][2], matrix[3][3])
			);
		}

		// Jolt to GLM conversions
		static glm::vec3 FromJoltVector(const JPH::Vec3& vector)
		{
			return glm::vec3(vector.GetX(), vector.GetY(), vector.GetZ());
		}

		static glm::quat FromJoltQuat(const JPH::Quat& quat)
		{
			return glm::quat(quat.GetW(), quat.GetX(), quat.GetY(), quat.GetZ());
		}

		static glm::mat4 FromJoltMatrix(const JPH::Mat44& matrix)
		{
			glm::mat4 result;
			for (i32 i = 0; i < 4; ++i)
			{
				for (i32 j = 0; j < 4; ++j)
				{
					result[i][j] = matrix(j, i);
				}
			}
			return result;
		}

		// Body type conversions
		static JPH::EMotionType ToJoltMotionType(EBodyType bodyType)
		{
			switch (bodyType)
			{
				case EBodyType::Static:    return JPH::EMotionType::Static;
				case EBodyType::Dynamic:   return JPH::EMotionType::Dynamic;
				case EBodyType::Kinematic: return JPH::EMotionType::Kinematic;
				default:
					OLO_CORE_ASSERT(false, "Unknown EBodyType");
					return JPH::EMotionType::Static;
			}
		}

		static EBodyType FromJoltMotionType(JPH::EMotionType motionType)
		{
			switch (motionType)
			{
				case JPH::EMotionType::Static:    return EBodyType::Static;
				case JPH::EMotionType::Dynamic:   return EBodyType::Dynamic;
				case JPH::EMotionType::Kinematic: return EBodyType::Kinematic;
				default:
					OLO_CORE_ASSERT(false, "Unknown JPH::EMotionType");
					return EBodyType::Static;
			}
		}

		// Collision detection conversions
		static JPH::EMotionQuality ToJoltMotionQuality(ECollisionDetectionType collisionDetection)
		{
			switch (collisionDetection)
			{
				case ECollisionDetectionType::Discrete:   return JPH::EMotionQuality::Discrete;
				case ECollisionDetectionType::Continuous: return JPH::EMotionQuality::LinearCast;
				default:
					OLO_CORE_ASSERT(false, "Unknown ECollisionDetectionType");
					return JPH::EMotionQuality::Discrete;
			}
		}

		// Transform decomposition result
		struct TransformComponents
		{
			glm::vec3 Translation;
			glm::quat Rotation;
			glm::vec3 Scale;
		};

		// Utility functions
		static glm::vec3 GetTranslationFromTransform(const glm::mat4& transform)
		{
			return glm::vec3(transform[3]);
		}

		static TransformComponents DecomposeTransform(const glm::mat4& transform)
		{
			TransformComponents components;
			
			// Extract translation directly from the matrix (faster than decompose for this component)
			components.Translation = glm::vec3(transform[3]);
			
			// Use glm::decompose for rotation and scale
			glm::vec3 skew;
			glm::vec4 perspective;
			glm::decompose(transform, components.Scale, components.Rotation, components.Translation, skew, perspective);
			
			return components;
		}

		static glm::quat GetRotationFromTransform(const glm::mat4& transform)
		{
			return DecomposeTransform(transform).Rotation;
		}

		static glm::vec3 GetScaleFromTransform(const glm::mat4& transform)
		{
			return DecomposeTransform(transform).Scale;
		}

		// Transform composition
		static glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
		{
			return glm::translate(glm::mat4(1.0f), translation) *
				   glm::mat4_cast(rotation) *
				   glm::scale(glm::mat4(1.0f), scale);
		}

		// Degrees/Radians conversion
		static f32 DegreesToRadians(f32 degrees)
		{
			return degrees * (glm::pi<f32>() / 180.0f);
		}

		static f32 RadiansToDegrees(f32 radians)
		{
			return radians * (180.0f / glm::pi<f32>());
		}

		// Jolt layer ID validation
		static bool IsValidLayerID(u32 layerID)
		{
			return layerID < kMaxJoltLayers; // Jolt supports up to kMaxJoltLayers layers
		}

		// Safe casting helpers
		template<typename T>
		static T* SafeCast(void* ptr)
		{
			return static_cast<T*>(ptr);
		}

		template<typename T>
		static const T* SafeCast(const void* ptr)
		{
			return static_cast<const T*>(ptr);
		}
	};

}