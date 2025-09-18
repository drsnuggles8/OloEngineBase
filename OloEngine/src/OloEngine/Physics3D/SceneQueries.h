#pragma once

#include "Physics3DTypes.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>
#include <vector>
#include <limits>

namespace OloEngine {

	// Forward declarations
	class JoltBody;
	class PhysicsShape;

	/**
	 * @brief Detailed information about a physics query hit
	 * 
	 * Contains comprehensive information about collision detection results,
	 * including hit position, normal, distance, and references to the hit entities.
	 */
	struct SceneQueryHit
	{
		UUID HitEntity = 0;
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f);
		f32 Distance = 0.0f;
		Ref<JoltBody> HitBody = nullptr;
		// TODO: Add PhysicsShape reference when shape abstraction is implemented
		// Ref<PhysicsShape> HitCollider = nullptr;

		bool HasHit() const { return HitEntity != 0; }

		void Clear()
		{
			HitEntity = 0;
			Position = glm::vec3(std::numeric_limits<f32>::max());
			Normal = glm::vec3(std::numeric_limits<f32>::max());
			Distance = std::numeric_limits<f32>::max();
			HitBody = nullptr;
		}
	};

	using ExcludedEntityMap = std::vector<UUID>;

	/**
	 * @brief Information for performing ray casting queries
	 * 
	 * Defines the parameters for casting a ray through the physics world,
	 * including origin, direction, distance limits, and entity filtering.
	 */
	struct RayCastInfo
	{
		glm::vec3 m_Origin = glm::vec3(0.0f);
		glm::vec3 m_Direction = glm::vec3(0.0f, 0.0f, 1.0f);
		f32 m_MaxDistance = 500.0f;
		u32 m_LayerMask = 0xFFFFFFFF;
		ExcludedEntityMap m_ExcludedEntities;

		RayCastInfo() = default;
		RayCastInfo(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance = 500.0f)
			: m_Origin(origin), m_Direction(direction), m_MaxDistance(maxDistance) {}
	};

	/**
	 * @brief Base information for shape casting queries
	 * 
	 * Common parameters shared across different shape casting operations.
	 */
	enum class ShapeCastType { Box, Sphere, Capsule };

	struct ShapeCastInfo
	{
		ShapeCastInfo(ShapeCastType castType)
			: m_Type(castType) {}

		glm::vec3 m_Origin = glm::vec3(0.0f);
		glm::vec3 m_Direction = glm::vec3(0.0f, 0.0f, 1.0f);
		f32 m_MaxDistance = 500.0f;
		u32 m_LayerMask = 0xFFFFFFFF;
		ExcludedEntityMap m_ExcludedEntities;

		ShapeCastType GetCastType() const { return m_Type; }

	protected:
		// Protected constructor for derived classes to initialize all common parameters
		ShapeCastInfo(ShapeCastType castType, const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance)
			: m_Type(castType), m_Origin(origin), m_Direction(direction), m_MaxDistance(maxDistance) {}

	private:
		ShapeCastType m_Type;
	};

	/**
	 * @brief Box shape casting parameters
	 * 
	 * Performs a sweep test using a box shape through the physics world.
	 */
	struct BoxCastInfo : public ShapeCastInfo
	{
		BoxCastInfo() : ShapeCastInfo(ShapeCastType::Box) {}
		BoxCastInfo(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& halfExtent, f32 maxDistance = 500.0f)
			: ShapeCastInfo(ShapeCastType::Box, origin, direction, maxDistance), m_HalfExtent(halfExtent)
		{
		}

		glm::vec3 m_HalfExtent = glm::vec3(0.5f);
	};

	/**
	 * @brief Sphere shape casting parameters
	 * 
	 * Performs a sweep test using a sphere shape through the physics world.
	 */
	struct SphereCastInfo : public ShapeCastInfo
	{
		SphereCastInfo() : ShapeCastInfo(ShapeCastType::Sphere) {}
		explicit SphereCastInfo(const glm::vec3& origin, const glm::vec3& direction, f32 radius, f32 maxDistance = 500.0f)
			: ShapeCastInfo(ShapeCastType::Sphere, origin, direction, maxDistance), m_Radius(radius)
		{
		}

		f32 m_Radius = 0.5f;
	};

	/**
	 * @brief Capsule shape casting parameters
	 * 
	 * Performs a sweep test using a capsule shape through the physics world.
	 */
	struct CapsuleCastInfo : public ShapeCastInfo
	{
		CapsuleCastInfo() : ShapeCastInfo(ShapeCastType::Capsule) {}
		explicit CapsuleCastInfo(const glm::vec3& origin, const glm::vec3& direction, f32 halfHeight, f32 radius, f32 maxDistance = 500.0f)
			: ShapeCastInfo(ShapeCastType::Capsule, origin, direction, maxDistance), m_HalfHeight(halfHeight), m_Radius(radius)
		{
		}

		f32 m_HalfHeight = 1.0f;
		f32 m_Radius = 0.5f;
	};

	/**
	 * @brief Base information for shape overlap queries
	 * 
	 * Common parameters for detecting overlapping objects with a given shape.
	 */
	struct ShapeOverlapInfo
	{
		ShapeOverlapInfo(ShapeCastType castType) : m_Type(castType) {}

		glm::vec3 m_Origin = glm::vec3(0.0f);
		glm::quat m_Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		u32 m_LayerMask = 0xFFFFFFFF;
		ExcludedEntityMap m_ExcludedEntities;

		ShapeCastType GetCastType() const { return m_Type; }

	private:
		ShapeCastType m_Type;
	};

	/**
	 * @brief Box overlap query parameters
	 * 
	 * Detects all objects overlapping with a box shape at a specific position.
	 */
	struct BoxOverlapInfo : public ShapeOverlapInfo
	{
		BoxOverlapInfo() : ShapeOverlapInfo(ShapeCastType::Box) {}
		BoxOverlapInfo(const glm::vec3& origin, const glm::vec3& halfExtentValue)
			: ShapeOverlapInfo(ShapeCastType::Box), m_HalfExtent(halfExtentValue)
		{
			m_Origin = origin;
		}

		glm::vec3 m_HalfExtent = glm::vec3(0.5f);
	};

	/**
	 * @brief Sphere overlap query parameters
	 * 
	 * Detects all objects overlapping with a sphere shape at a specific position.
	 */
	struct SphereOverlapInfo : public ShapeOverlapInfo
	{
		SphereOverlapInfo() : ShapeOverlapInfo(ShapeCastType::Sphere) {}
		SphereOverlapInfo(const glm::vec3& origin, f32 sphereRadius)
			: ShapeOverlapInfo(ShapeCastType::Sphere), m_Radius(sphereRadius)
		{
			m_Origin = origin;
		}

		f32 m_Radius = 0.5f;
	};

	/**
	 * @brief Capsule overlap query parameters
	 * 
	 * Detects all objects overlapping with a capsule shape at a specific position.
	 */
	struct CapsuleOverlapInfo : public ShapeOverlapInfo
	{
		CapsuleOverlapInfo() : ShapeOverlapInfo(ShapeCastType::Capsule) {}
		CapsuleOverlapInfo(const glm::vec3& origin, f32 capsuleHalfHeight, f32 capsuleRadius)
			: ShapeOverlapInfo(ShapeCastType::Capsule), m_HalfHeight(capsuleHalfHeight), m_Radius(capsuleRadius)
		{
			m_Origin = origin;
		}

		f32 m_HalfHeight = 1.0f;
		f32 m_Radius = 0.5f;
	};

	/**
	 * @brief Scene query interface for physics world queries
	 * 
	 * Provides methods for performing various types of spatial queries
	 * against the physics world, including ray casting, shape casting,
	 * and overlap detection.
	 */
	class SceneQueries
	{
	public:
		virtual ~SceneQueries() = default;

		// Ray casting
		virtual bool CastRay(const RayCastInfo& rayInfo, SceneQueryHit& outHit) = 0;

		// Shape casting (sweep tests)
		virtual bool CastShape(const ShapeCastInfo& shapeCastInfo, SceneQueryHit& outHit) = 0;
		virtual bool CastBox(const BoxCastInfo& boxCastInfo, SceneQueryHit& outHit) = 0;
		virtual bool CastSphere(const SphereCastInfo& sphereCastInfo, SceneQueryHit& outHit) = 0;
		virtual bool CastCapsule(const CapsuleCastInfo& capsuleCastInfo, SceneQueryHit& outHit) = 0;

		// Overlap queries
		virtual i32 OverlapShape(const ShapeOverlapInfo& overlapInfo, SceneQueryHit* outHits, i32 maxHits) = 0;
		virtual i32 OverlapBox(const BoxOverlapInfo& boxOverlapInfo, SceneQueryHit* outHits, i32 maxHits) = 0;
		virtual i32 OverlapSphere(const SphereOverlapInfo& sphereOverlapInfo, SceneQueryHit* outHits, i32 maxHits) = 0;
		virtual i32 OverlapCapsule(const CapsuleOverlapInfo& capsuleOverlapInfo, SceneQueryHit* outHits, i32 maxHits) = 0;

		// Multi-hit ray casting
		virtual i32 CastRayMultiple(const RayCastInfo& rayInfo, SceneQueryHit* outHits, i32 maxHits) = 0;

		// Multi-hit shape casting
		virtual i32 CastShapeMultiple(const ShapeCastInfo& shapeCastInfo, SceneQueryHit* outHits, i32 maxHits) = 0;
	};

	/**
	 * @brief Utility functions for scene queries
	 */
	namespace SceneQueryUtils
	{
		// Helper functions for creating common query types
		inline RayCastInfo CreateRayInfo(const glm::vec3& from, const glm::vec3& to)
		{
			glm::vec3 direction = to - from;
			f32 distance = glm::length(direction);
			if (distance > 0.0f)
				direction /= distance;
			return RayCastInfo(from, direction, distance);
		}

		inline BoxCastInfo CreateBoxCast(const glm::vec3& from, const glm::vec3& to, const glm::vec3& halfExtent)
		{
			glm::vec3 direction = to - from;
			f32 distance = glm::length(direction);
			if (distance > 0.0f)
				direction /= distance;
			return BoxCastInfo(from, direction, halfExtent, distance);
		}

		inline SphereCastInfo CreateSphereCast(const glm::vec3& from, const glm::vec3& to, f32 radius)
		{
			glm::vec3 direction = to - from;
			f32 distance = glm::length(direction);
			if (distance > 0.0f)
				direction /= distance;
			return SphereCastInfo(from, direction, radius, distance);
		}

		// Entity filtering helpers
		inline void AddExcludedEntity(ExcludedEntityMap& excludedEntities, UUID entityID)
		{
			excludedEntities.push_back(entityID);
		}

		inline bool IsEntityExcluded(const ExcludedEntityMap& excludedEntities, UUID entityID)
		{
			return std::find(excludedEntities.begin(), excludedEntities.end(), entityID) != excludedEntities.end();
		}
	}

}