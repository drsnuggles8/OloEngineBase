#pragma once

#include <string>
#include <unordered_map>

#include <glm/glm.hpp>
#include "OloEngine/Core/Ref.h"
#include "ShaderLibrary.h" // Include the new ShaderLibrary header

namespace OloEngine
{
	// Forward declaration
	class ShaderResourceRegistry;

	class Shader : public RefCounted
	{
	public:
		virtual ~Shader() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual void SetInt(const std::string& name, int value) = 0;
		virtual void SetIntArray(const std::string& name, int* values, u32 count) = 0;
		virtual void SetFloat(const std::string& name, f32 value) = 0;
		virtual void SetFloat2(const std::string& name, const glm::vec2& value) = 0;
		virtual void SetFloat3(const std::string& name, const glm::vec3& value) = 0;
		virtual void SetFloat4(const std::string& name, const glm::vec4& value) = 0;
		virtual void SetMat4(const std::string& name, const glm::mat4& value) = 0;

		[[nodiscard]] virtual u32 GetRendererID() const = 0;

		[[nodiscard("Store this!")]] virtual const std::string& GetName() const = 0;
		[[nodiscard("Store this!")]] virtual const std::string& GetFilePath() const = 0;

		virtual void Reload() = 0;

		// Resource registry access (safe interface)
		virtual ShaderResourceRegistry* GetResourceRegistry() = 0;
		virtual const ShaderResourceRegistry* GetResourceRegistry() const = 0;

		static AssetRef<Shader> Create(const std::string& filepath);
		static AssetRef<Shader> Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc);
	};
}
