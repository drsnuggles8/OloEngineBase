#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <utility>

#include "SceneCamera.h"
#include "NativeScript.h"
#include "OloEngine/Renderer/Texture.h"

namespace OloEngine {

	struct TagComponent
	{
		std::string Tag;
		bool renaming = false;

		TagComponent() = default;
		TagComponent(const TagComponent& other) = default;
		explicit TagComponent(std::string  tag)
			: Tag(std::move(tag)) {}

		explicit operator std::string& () { return Tag; }
		explicit operator const std::string& () const { return Tag; }
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


		[[nodiscard("Store this, you probably wanted another function!")]] glm::mat4 GetTransform() const
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
		Ref<Texture2D> Texture;
		float TilingFactor = 1.0f;

		SpriteRendererComponent() = default;
		SpriteRendererComponent(const SpriteRendererComponent&) = default;
		explicit SpriteRendererComponent(const glm::vec4& color)
			: Color(color) {}
	};

	struct CameraComponent
	{
		SceneCamera Camera;
		bool Primary = true; // TODO: think about moving to Scene
		bool FixedAspectRatio = false;

		CameraComponent() = default;
		CameraComponent(const CameraComponent&) = default;
	};

	struct NativeScriptComponent
	{
		std::function<Scope<NativeScript>(Entity entity)> InstantiateScript;
		Scope<NativeScript> Instance;

		template<typename T, typename... Args>
		void Bind(Args... args)
		{
			InstantiateScript = [args...](Entity entity)->Scope<NativeScript> { return CreateScope<T>(entity, args...); };
		}
	};

}
