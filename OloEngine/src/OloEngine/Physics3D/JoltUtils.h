#pragma once

#include "Physics3DTypes.h"
#include "OloEngine/Core/Base.h"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/DVec3.h>
#include <Jolt/Math/Real.h>
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

		// Matrix operations constants
		static constexpr f32 kNormalizationEpsilon = 1e-6f;

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

		static JPH::RVec3 ToJoltRVec3(const glm::vec3& vector)
		{
			return JPH::RVec3(vector.x, vector.y, vector.z);
		}

		// Note: GLM's .x/.y/.z/.w components map directly to Jolt's JPH::Quat(x,y,z,w) constructor
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

		static glm::vec3 FromJoltRVec3(const JPH::RVec3& vector)
		{
			return glm::vec3(static_cast<f32>(vector.GetX()), static_cast<f32>(vector.GetY()), static_cast<f32>(vector.GetZ()));
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
			glm::vec3 m_Translation;
			glm::quat m_Rotation;
			glm::vec3 m_Scale;
		};

		// Utility functions
		static glm::vec3 GetTranslationFromTransform(const glm::mat4& transform)
		{
			return glm::vec3(transform[3]);
		}

		// Full transform decomposition - extracts all components (translation, rotation, scale)
		// Note: If you only need one component, prefer the direct extractors (GetTranslationFromTransform, GetRotationFromTransform, GetScaleFromTransform)
		// for better performance in hot paths
		static TransformComponents DecomposeTransform(const glm::mat4& transform)
		{
			TransformComponents components;
			
			// Extract translation directly from the matrix (faster than decompose for this component)
			components.m_Translation = glm::vec3(transform[3]);
			
			// Use glm::decompose for rotation and scale (use dummy translation to avoid overwriting)
			glm::vec3 skew;
			glm::vec4 perspective;
			glm::vec3 dummyTranslation;
			glm::decompose(transform, components.m_Scale, components.m_Rotation, dummyTranslation, skew, perspective);
			
			return components;
		}

		// Fast direct rotation extraction - builds 3x3 rotation matrix, normalizes columns to remove scale, converts to quaternion
		// Prefer this over DecomposeTransform() when only rotation is needed (hot path optimization)
		static glm::quat GetRotationFromTransform(const glm::mat4& transform)
		{
			// Extract the upper-left 3x3 rotation+scale matrix
			glm::mat3 rotScale = glm::mat3(transform);
			
			// Calculate column lengths
			f32 len0 = glm::length(rotScale[0]);
			f32 len1 = glm::length(rotScale[1]);
			f32 len2 = glm::length(rotScale[2]);
			
			// Check if any column is too small (near-zero scale)
			if (len0 < kNormalizationEpsilon || len1 < kNormalizationEpsilon || len2 < kNormalizationEpsilon)
			{
				// Fallback to robust decomposition when columns are degenerate
				auto components = DecomposeTransform(transform);
				return components.m_Rotation;
			}
			
			// Safe normalization - all columns have sufficient length
			glm::vec3 col0 = rotScale[0] / len0;
			glm::vec3 col1 = rotScale[1] / len1;
			glm::vec3 col2 = rotScale[2] / len2;
			
			// Verify orthogonality and apply Gram-Schmidt if needed
			f32 dot01 = glm::dot(col0, col1);
			f32 dot02 = glm::dot(col0, col2);
			f32 dot12 = glm::dot(col1, col2);
			
			// If columns are not sufficiently orthogonal, re-orthogonalize
			if (glm::abs(dot01) > kNormalizationEpsilon || glm::abs(dot02) > kNormalizationEpsilon || glm::abs(dot12) > kNormalizationEpsilon)
			{
				// Apply Gram-Schmidt orthogonalization
				// Keep col0 as reference, orthogonalize col1 and col2
				col1 = col1 - glm::dot(col1, col0) * col0;
				f32 len1_ortho = glm::length(col1);
				if (len1_ortho < kNormalizationEpsilon)
				{
					// col1 is parallel to col0, construct perpendicular vector
					glm::vec3 arbitrary = (glm::abs(col0.x) < 0.9f) ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
					col1 = glm::normalize(glm::cross(col0, arbitrary));
				}
				else
				{
					col1 = glm::normalize(col1);
				}
				
				// col2 = cross(col0, col1) for right-handed orthonormal basis
				col2 = glm::cross(col0, col1);
			}
			
			// Reconstruct pure rotation matrix from orthonormal columns
			glm::mat3 rotation(col0, col1, col2);
			
			// Convert rotation matrix to quaternion
			return glm::quat_cast(rotation);
		}

		// Fast direct scale extraction - computes per-axis scale as length of each transform column
		// Prefer this over DecomposeTransform() when only scale is needed (hot path optimization)
		static glm::vec3 GetScaleFromTransform(const glm::mat4& transform)
		{
			// Scale is the length of each column vector in the upper-left 3x3 matrix
			return glm::vec3(
				glm::length(glm::vec3(transform[0])), // X scale
				glm::length(glm::vec3(transform[1])), // Y scale
				glm::length(glm::vec3(transform[2]))  // Z scale
			);
		}

		// Overloaded APIs that accept precomputed decomposition to avoid repeated work
		static glm::quat GetRotationFromTransform(const TransformComponents& components)
		{
			return components.m_Rotation;
		}

		static glm::vec3 GetScaleFromTransform(const TransformComponents& components)
		{
			return components.m_Scale;
		}

		static glm::vec3 GetTranslationFromTransform(const TransformComponents& components)
		{
			return components.m_Translation;
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