#pragma once
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Audio/AudioListener.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"

#include "box2d/box2d.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <utility>
#include <vector>
#include <string>

namespace OloEngine
{

	struct IDComponent
	{
		UUID ID;

		IDComponent() = default;
		IDComponent(const IDComponent&) = default;
	};

	struct TagComponent
	{
		std::string Tag;
		bool renaming = false;

		TagComponent() = default;
		TagComponent(const TagComponent& other) = default;
		explicit TagComponent(std::string tag)
			: Tag(std::move(tag)) {}

		explicit operator std::string& () { return Tag; }
		explicit operator const std::string& () const { return Tag; }
	};

	struct PrefabComponent
	{
		UUID m_PrefabID{};
		UUID m_PrefabEntityID{};
		PrefabComponent() = default;
		PrefabComponent(const PrefabComponent&) = default;
		PrefabComponent(UUID prefabID, UUID prefabEntityID)
			: m_PrefabID(prefabID), m_PrefabEntityID(prefabEntityID) {}

		[[nodiscard]] inline bool IsValid() const noexcept
		{
			return static_cast<u64>(m_PrefabID) != 0 && static_cast<u64>(m_PrefabEntityID) != 0;
		}
	};

	struct TransformComponent
	{
		glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

		TransformComponent() = default;
		TransformComponent(const TransformComponent& other) = default;
		explicit TransformComponent(const glm::vec3& translation)
			: Translation(translation) {}


		[[nodiscard("Store this!")]] glm::mat4 GetTransform() const
		{
			glm::mat4 const rotation = glm::toMat4(glm::quat(Rotation));

			return glm::translate(glm::mat4(1.0f), Translation)
				* rotation
				* glm::scale(glm::mat4(1.0f), Scale);
		}
	};

	struct SpriteRendererComponent
	{
		glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
		Ref<Texture2D> Texture = nullptr;
		f32 TilingFactor = 1.0f;

		SpriteRendererComponent() = default;
		SpriteRendererComponent(const SpriteRendererComponent&) = default;
		explicit SpriteRendererComponent(const glm::vec4& color)
			: Color(color) {}
	};

	struct CircleRendererComponent
	{
		glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
		f32 Thickness = 1.0f;
		f32 Fade = 0.005f;

		CircleRendererComponent() = default;
		CircleRendererComponent(const CircleRendererComponent&) = default;
	};

	struct CameraComponent
	{
		// TODO(olbu): think about moving to Scene
		SceneCamera Camera;
		bool Primary = true;
		bool FixedAspectRatio = false;

		CameraComponent() = default;
		CameraComponent(const CameraComponent&) = default;
	};

	// Physics

	struct Rigidbody2DComponent
	{
		enum class BodyType { Static = 0, Dynamic, Kinematic };
		BodyType Type = BodyType::Static;
		bool FixedRotation = false;

		// Storage for runtime
		b2BodyId RuntimeBody = b2_nullBodyId;

		Rigidbody2DComponent() = default;
		Rigidbody2DComponent(const Rigidbody2DComponent&) = default;
	};

	struct BoxCollider2DComponent
	{
		glm::vec2 Offset = { 0.0f, 0.0f };
		glm::vec2 Size = { 0.5f, 0.5f };

		// TODO(olbu): move into physics material in the future maybe
		f32 Density = 1.0f;
		f32 Friction = 0.5f;
		f32 Restitution = 0.0f;
		f32 RestitutionThreshold = 0.5f;

		// Storage for runtime
		void* RuntimeFixture = nullptr;

		BoxCollider2DComponent() = default;
		BoxCollider2DComponent(const BoxCollider2DComponent&) = default;
	};

	struct CircleCollider2DComponent
	{
		glm::vec2 Offset = { 0.0f, 0.0f };
		f32 Radius = 0.5f;

		// TODO(olbu): move into physics material in the future maybe
		f32 Density = 1.0f;
		f32 Friction = 0.5f;
		f32 Restitution = 0.0f;
		f32 RestitutionThreshold = 0.5f;

		// Storage for runtime
		void* RuntimeFixture = nullptr;

		CircleCollider2DComponent() = default;
		CircleCollider2DComponent(const CircleCollider2DComponent&) = default;
	};

	// 3D Physics Components

	enum class BodyType3D { Static = 0, Dynamic, Kinematic };

	struct RigidBody3DComponent
	{
		BodyType3D Type = BodyType3D::Static;
		u32 LayerID = 0;
		f32 Mass = 1.0f;
		f32 LinearDrag = 0.01f;
		f32 AngularDrag = 0.05f;
		bool DisableGravity = false;
		bool IsTrigger = false;
		EActorAxis LockedAxes = EActorAxis::None;
		
		glm::vec3 InitialLinearVelocity = { 0.0f, 0.0f, 0.0f };
		glm::vec3 InitialAngularVelocity = { 0.0f, 0.0f, 0.0f };
		
		f32 MaxLinearVelocity = 500.0f;
		f32 MaxAngularVelocity = 50.0f;

		// Storage for runtime - Jolt BodyID
		void* RuntimeBody = nullptr;

		RigidBody3DComponent() = default;
		RigidBody3DComponent(const RigidBody3DComponent&) = default;
	};

	struct BoxCollider3DComponent
	{
		glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f };
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };

		// Physics material properties
		f32 Density = 1.0f;
		f32 Friction = 0.5f;
		f32 Restitution = 0.0f;

		BoxCollider3DComponent() = default;
		BoxCollider3DComponent(const BoxCollider3DComponent&) = default;
	};

	struct SphereCollider3DComponent
	{
		f32 Radius = 0.5f;
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };

		// Physics material properties
		f32 Density = 1.0f;
		f32 Friction = 0.5f;
		f32 Restitution = 0.0f;

		SphereCollider3DComponent() = default;
		SphereCollider3DComponent(const SphereCollider3DComponent&) = default;
	};

	struct CapsuleCollider3DComponent
	{
		f32 Radius = 0.5f;
		f32 HalfHeight = 1.0f;
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };

		// Physics material properties
		f32 Density = 1.0f;
		f32 Friction = 0.5f;
		f32 Restitution = 0.0f;

		CapsuleCollider3DComponent() = default;
		CapsuleCollider3DComponent(const CapsuleCollider3DComponent&) = default;
	};

	struct MeshCollider3DComponent
	{
		AssetHandle ColliderAsset = 0;  // Reference to MeshColliderAsset
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

		// Physics material properties
		f32 Density = 1.0f;
		f32 Friction = 0.5f;
		f32 Restitution = 0.0f;

		// Collision complexity setting
		bool UseComplexAsSimple = false;  // If true, use triangle mesh for dynamic bodies

		MeshCollider3DComponent() = default;
		MeshCollider3DComponent(const MeshCollider3DComponent&) = default;
		explicit MeshCollider3DComponent(AssetHandle colliderAsset) : ColliderAsset(colliderAsset) {}
	};

	struct ConvexMeshCollider3DComponent
	{
		AssetHandle ColliderAsset = 0;  // Reference to MeshColliderAsset
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

		// Physics material properties
		f32 Density = 1.0f;
		f32 Friction = 0.5f;
		f32 Restitution = 0.0f;

		// Convex hull settings
		f32 ConvexRadius = 0.05f;  // Jolt convex radius for shape rounding
		u32 MaxVertices = 256;     // Maximum vertices in convex hull

		ConvexMeshCollider3DComponent() = default;
		ConvexMeshCollider3DComponent(const ConvexMeshCollider3DComponent&) = default;
		explicit ConvexMeshCollider3DComponent(AssetHandle colliderAsset) : ColliderAsset(colliderAsset) {}
	};

	struct TriangleMeshCollider3DComponent
	{
		AssetHandle ColliderAsset = 0;  // Reference to MeshColliderAsset
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

		// Physics material properties
		f32 Density = 1.0f;
		f32 Friction = 0.5f;
		f32 Restitution = 0.0f;

		// Triangle mesh is always static - no additional settings needed

		TriangleMeshCollider3DComponent() = default;
		TriangleMeshCollider3DComponent(const TriangleMeshCollider3DComponent&) = default;
		explicit TriangleMeshCollider3DComponent(AssetHandle colliderAsset) : ColliderAsset(colliderAsset) {}
	};

	struct CharacterController3DComponent
	{
		f32 SlopeLimitDeg = 45.0f;
		f32 StepOffset = 0.4f;
		f32 JumpPower = 8.0f;
		u32 LayerID = 0;
		
		bool DisableGravity = false;
		bool ControlMovementInAir = false;
		bool ControlRotationInAir = false;

		CharacterController3DComponent() = default;
		CharacterController3DComponent(const CharacterController3DComponent&) = default;
	};

	struct TextComponent
	{
		std::string TextString;
		Ref<Font> FontAsset = Font::GetDefault();
		glm::vec4 Color{ 1.0f };
		f32 Kerning = 0.0f;
		f32 LineSpacing = 0.0f;
	};

	struct ScriptComponent
	{
		std::string ClassName;

		ScriptComponent() = default;
		ScriptComponent(const ScriptComponent&) = default;
	};

	struct AudioSourceComponent
	{
		AudioSourceConfig Config;

		Ref<AudioSource> Source = nullptr;

		AudioSourceComponent() = default;
		AudioSourceComponent(const AudioSourceComponent&) = default;
	};

	struct AudioListenerComponent
	{
		bool Active = true;
		AudioListenerConfig Config;

		Ref<AudioListener> Listener;

		AudioListenerComponent() = default;
		AudioListenerComponent(const AudioListenerComponent&) = default;
	};

	// Note: SubmeshComponent, MeshComponent, AnimationStateComponent, 
	// and SkeletonComponent are now defined in OloEngine/Animation/AnimatedMeshComponents.h
	// which is already included above

	// Material component for storing PBR material data
	struct MaterialComponent
	{
		Material m_Material;
		
		MaterialComponent() = default;
		MaterialComponent(const Material& material) : m_Material(material) {}
		MaterialComponent(const MaterialComponent&) = default;
	};

	// Entity relationship component for parent-child hierarchies (Hazel-style)
	struct RelationshipComponent
	{
		UUID m_ParentHandle{};
		std::vector<UUID> m_Children;

		RelationshipComponent() = default;
		RelationshipComponent(const RelationshipComponent&) = default;
		RelationshipComponent(RelationshipComponent&&) = default;
		RelationshipComponent& operator=(const RelationshipComponent&) = default;
		RelationshipComponent& operator=(RelationshipComponent&&) = default;
		explicit RelationshipComponent(UUID parent) : m_ParentHandle(parent) {}
	};

	template<typename... Component>
	struct ComponentGroup
	{
	};

	using AllComponents = ComponentGroup<
		TransformComponent,
		SpriteRendererComponent,
		CircleRendererComponent,
		CameraComponent,
		PrefabComponent,
		Rigidbody2DComponent,
		BoxCollider2DComponent,
		CircleCollider2DComponent,
		RigidBody3DComponent,
		BoxCollider3DComponent,
		SphereCollider3DComponent,
		CapsuleCollider3DComponent,
		CharacterController3DComponent,
		TextComponent,
		ScriptComponent,
		AudioSourceComponent,
		AudioListenerComponent,
		// AnimatedMeshComponent removed - deprecated
		SubmeshComponent,         // NEW
		MeshComponent,            // NEW
		AnimationStateComponent,
		SkeletonComponent,
		MaterialComponent,
		RelationshipComponent
	>;
}
