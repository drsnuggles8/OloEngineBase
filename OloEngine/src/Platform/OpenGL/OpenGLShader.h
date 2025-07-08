#pragma once
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include <glm/glm.hpp>

namespace OloEngine
{
	class OpenGLShader : public Shader
	{
	using GLenum = unsigned int;
	public:
		OpenGLShader(const std::string& filepath);
		OpenGLShader(std::string  name, std::string_view vertexSrc, std::string_view fragmentSrc);
		~OpenGLShader() override;

		void Bind() const override;
		void Unbind() const override;

		void SetInt(const std::string& name, int value) override;
		void SetIntArray(const std::string& name, int* values, u32 count) override;
		void SetFloat(const std::string& name, f32 value) override;
		void SetFloat2(const std::string& name, const glm::vec2& value) override;
		void SetFloat3(const std::string& name, const glm::vec3& value) override;
		void SetFloat4(const std::string& name, const glm::vec4& value) override;
		void SetMat4(const std::string& name, const glm::mat4& value) override;

		[[nodiscard]] u32 GetRendererID() const override { return m_RendererID; }
		[[nodiscard ("Store this!")]] const std::string& GetName() const override { return m_Name; }
		[[nodiscard ("Store this!")]] const std::string& GetFilePath() const override { return m_FilePath; }

		void Reload() override;

		void UploadUniformInt(const std::string& name, int value) const;
		void UploadUniformIntArray(const std::string& name, int const* values, u32 count) const;
		void UploadUniformFloat(const std::string& name, f32 value) const;
		void UploadUniformFloat2(const std::string& name, const glm::vec2& value) const;
		void UploadUniformFloat3(const std::string& name, const glm::vec3& value) const;
		void UploadUniformFloat4(const std::string& name, const glm::vec4& value) const;

		void UploadUniformMat3(const std::string& name, const glm::mat3& matrix) const;
		void UploadUniformMat4(const std::string& name, const glm::mat4& matrix) const;

		// Resource registry access
		ShaderResourceRegistry& GetResourceRegistry() { return m_ResourceRegistry; }
		const ShaderResourceRegistry& GetResourceRegistry() const { return m_ResourceRegistry; }

		// Initialize resource registry (called after shader is fully constructed)
		void InitializeResourceRegistry(const Ref<Shader>& shaderRef);

		// Convenience methods for setting shader resources
		template<typename T>
		bool SetShaderResource(const std::string& name, const Ref<T>& resource)
		{
			return m_ResourceRegistry.SetResource(name, resource);
		}

		bool SetShaderResource(const std::string& name, const ShaderResourceInput& input)
		{
			return m_ResourceRegistry.SetResource(name, input);
		}
	private:
		static std::string ReadFile(const std::string& filepath);
		static std::unordered_map<GLenum, std::string> PreProcess(std::string_view source);

		void CompileOrGetVulkanBinaries(const std::unordered_map<GLenum, std::string>& shaderSources);
		void CompileOrGetOpenGLBinaries();
		void CreateProgram();

		void CompileOpenGLBinariesForAmd(GLenum const& program, std::array<u32, 2>& glShadersIDs) const;
		void CreateProgramForAmd();

		void Reflect(GLenum stage, const std::vector<u32>& shaderData);
	private:
		u32 m_RendererID{};
		std::string m_FilePath;
		std::string m_Name;
		std::unordered_map<GLenum, std::vector<u32>> m_VulkanSPIRV;
		std::unordered_map<GLenum, std::vector<u32>> m_OpenGLSPIRV;

		std::unordered_map<GLenum, std::string> m_OpenGLSourceCode;
		std::unordered_map<GLenum, std::string> m_OriginalSourceCode; // Store original preprocessed source

		// Resource registry for automatic resource management
		ShaderResourceRegistry m_ResourceRegistry;
	};

}
