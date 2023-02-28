// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLShader.h"
#include "OloEngine/Core/Timer.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>
#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_cross.hpp>
#include <spirv_cross/spirv_glsl.hpp>

#include <fstream>
#include <utility>

namespace OloEngine
{
	namespace Utils
	{
		static GLenum ShaderTypeFromString(std::string_view type)
		{
			if (type == "vertex")
			{
				return GL_VERTEX_SHADER;
			}
			if ((type == "fragment") || (type == "pixel"))
			{
				return GL_FRAGMENT_SHADER;
			}

			OLO_CORE_ASSERT(false, "Unknown shader type!");
			return 0;
		}

		[[nodiscard("Store this!")]] static shaderc_shader_kind GLShaderStageToShaderC(const GLenum stage)
		{
			switch (stage)
			{
				case GL_VERTEX_SHADER:
				{
					return shaderc_glsl_vertex_shader;
				}
				case GL_FRAGMENT_SHADER:
				{
					return shaderc_glsl_fragment_shader;
				}
			}
			OLO_CORE_ASSERT(false);
			return static_cast<shaderc_shader_kind>(0);
		}

		[[nodiscard("Store this!")]] static const char* GLShaderStageToString(const GLenum stage)
		{
			switch (stage)
			{
				case GL_VERTEX_SHADER:
				{
					return "GL_VERTEX_SHADER";
				}
				case GL_FRAGMENT_SHADER:
				{
					return "GL_FRAGMENT_SHADER";
				}
			}
			OLO_CORE_ASSERT(false);
			return nullptr;
		}

		[[nodiscard("Store this!")]] static const char* GetCacheDirectory()
		{
			// TODO(olbu): make sure the assets directory is valid
			return "assets/cache/shader/opengl";
		}

		static void CreateCacheDirectoryIfNeeded()
		{
			const std::filesystem::path cacheDirectory = GetCacheDirectory();
			if (!std::filesystem::exists(cacheDirectory))
			{
				std::filesystem::create_directories(cacheDirectory);
			}
		}

		[[nodiscard("Store this!")]] static const char* GLShaderStageCachedOpenGLFileExtension(const u32 stage)
		{
			switch (stage)
			{
				case GL_VERTEX_SHADER:
				{
					return ".cached_opengl.vert";
				}
				case GL_FRAGMENT_SHADER:
				{
					return ".cached_opengl.frag";
				}
			}
			OLO_CORE_ASSERT(false);
			return "";
		}

		[[nodiscard("Store this!")]] static const char* GLShaderStageCachedVulkanFileExtension(const u32 stage)
		{
			switch (stage)
			{
				case GL_VERTEX_SHADER:
				{
					return ".cached_vulkan.vert";
				}
				case GL_FRAGMENT_SHADER:
				{
					return ".cached_vulkan.frag";
				}
			}
			OLO_CORE_ASSERT(false);
			return "";
		}

		static bool IsAmdGpu()
		{
			const auto* const vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
			return std::strstr(vendor, "ATI") != nullptr;
		}

	}

	OpenGLShader::OpenGLShader(const std::string& filepath)
		: m_FilePath(filepath)
	{
		OLO_PROFILE_FUNCTION();

		Utils::CreateCacheDirectoryIfNeeded();

		const std::string source = ReadFile(filepath);
		const auto shaderSources = PreProcess(source);

		const Timer timer;
		CompileOrGetVulkanBinaries(shaderSources);
		if (Utils::IsAmdGpu())
		{
			CreateProgramForAmd();
		}
		else
		{
			CompileOrGetOpenGLBinaries();
			CreateProgram();
		}
		OLO_CORE_WARN("Shader creation took {0} ms", timer.ElapsedMillis());

		// Extract shader name from filepath
		auto lastSlash = filepath.find_last_of("/\\");
		const auto lastDot = filepath.rfind('.');
		lastSlash = lastSlash == std::string::npos ? 0 : (lastSlash + 1);
		const auto count = lastDot == std::string::npos ? (filepath.size() - lastSlash) : (lastDot - lastSlash);

		m_Name = filepath.substr(lastSlash, count);
	}

	OpenGLShader::OpenGLShader(std::string name, std::string_view vertexSrc, std::string_view fragmentSrc)
		: m_Name(std::move(name))
	{
		OLO_PROFILE_FUNCTION();

		std::unordered_map<GLenum, std::string> sources;
		sources[GL_VERTEX_SHADER] = vertexSrc;
		sources[GL_FRAGMENT_SHADER] = fragmentSrc;

		CompileOrGetVulkanBinaries(sources);
		if (Utils::IsAmdGpu())
		{
			CreateProgramForAmd();
		}
		else
		{
			CompileOrGetOpenGLBinaries();
			CreateProgram();
		}
	}

	OpenGLShader::~OpenGLShader()
	{
		OLO_PROFILE_FUNCTION();

		glDeleteProgram(m_RendererID);
	}

	std::string OpenGLShader::ReadFile(const std::string& filepath)
	{
		OLO_PROFILE_FUNCTION();

		std::string result;
		if (std::ifstream in(filepath, std::ios::in | std::ios::binary); in)
		{
			in.seekg(0, std::ios::end);
			sizet const size = in.tellg();
			if (std::cmp_not_equal(size, -1))
			{
				result.resize(size);
				in.seekg(0, std::ios::beg);
				in.read(&result[0], size);
			}
			else
			{
				OLO_CORE_ERROR("Could not read from file '{0}'", filepath);
			}
		}
		else
		{
			OLO_CORE_ERROR("Could not open file '{0}'", filepath);
		}

		return result;
	}

	std::unordered_map<GLenum, std::string> OpenGLShader::PreProcess(std::string_view source)
	{
		OLO_PROFILE_FUNCTION();

		std::unordered_map<GLenum, std::string> shaderSources;

		const char* const typeToken = "#type";
		const sizet typeTokenLength = std::strlen(typeToken);
		sizet pos = source.find(typeToken, 0); //Start of shader type declaration line
		while (pos != std::string::npos)
		{
			const sizet eol = source.find_first_of("\r\n", pos); //End of shader type declaration line
			OLO_CORE_ASSERT(eol != std::string::npos, "Syntax error");
			const sizet begin = pos + typeTokenLength + 1; //Start of shader type name (after "#type " keyword)
			std::string_view type = source.substr(begin, eol - begin);
			OLO_CORE_ASSERT(Utils::ShaderTypeFromString(type), "Invalid shader type specified");

			const sizet nextLinePos = source.find_first_not_of("\r\n", eol); //Start of shader code after shader type declaration line
			OLO_CORE_ASSERT(nextLinePos != std::string::npos, "Syntax error");
			pos = source.find(typeToken, nextLinePos); //Start of next shader type declaration line

			shaderSources[Utils::ShaderTypeFromString(type)] = (pos == std::string::npos) ? source.substr(nextLinePos) : source.substr(nextLinePos, pos - nextLinePos);
		}

		return shaderSources;
	}

	void OpenGLShader::CompileOrGetVulkanBinaries(const std::unordered_map<GLenum, std::string>& shaderSources)
	{
		glCreateProgram();

		const shaderc::Compiler compiler;
		shaderc::CompileOptions options;
		options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
		if (const bool optimize = true)
		{
			options.SetOptimizationLevel(shaderc_optimization_level_performance);
		}

		const std::filesystem::path cacheDirectory = Utils::GetCacheDirectory();

		auto& shaderData = m_VulkanSPIRV;
		shaderData.clear();
		for (auto&& [stage, source] : shaderSources)
		{
			const std::filesystem::path shaderFilePath = m_FilePath;
			const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + Utils::GLShaderStageCachedVulkanFileExtension(stage));

			std::ifstream in(cachedPath, std::ios::in | std::ios::binary);
			if (in.is_open())
			{
				in.seekg(0, std::ios::end);
				const auto size = in.tellg();
				in.seekg(0, std::ios::beg);

				auto& data = shaderData[stage];
				data.resize(size / sizeof(u32));
				in.read(reinterpret_cast<char*>(data.data()), size);
			}
			else
			{
				shaderc::SpvCompilationResult const spirvModule = compiler.CompileGlslToSpv(source, Utils::GLShaderStageToShaderC(stage), m_FilePath.c_str(), options);
				if (spirvModule.GetCompilationStatus() != shaderc_compilation_status_success)
				{
					OLO_CORE_ERROR(spirvModule.GetErrorMessage());
					OLO_CORE_ASSERT(false);
				}

				shaderData[stage] = std::vector<u32>(spirvModule.cbegin(), spirvModule.cend());

				std::ofstream out(cachedPath, std::ios::out | std::ios::binary);
				if (out.is_open())
				{
					auto& data = shaderData[stage];
					out.write(reinterpret_cast<char*>(data.data()), data.size() * sizeof(u32));
					out.flush();
					out.close();
				}
			}
		}

		for (auto&& [stage, data] : shaderData)
		{
			Reflect(stage, data);
		}
	}

	void OpenGLShader::CompileOrGetOpenGLBinaries()
	{
		auto& shaderData = m_OpenGLSPIRV;

		const shaderc::Compiler compiler;
		shaderc::CompileOptions options;
		options.SetTargetEnvironment(shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);

		const std::filesystem::path cacheDirectory = Utils::GetCacheDirectory();

		shaderData.clear();
		m_OpenGLSourceCode.clear();
		for (auto&& [stage, spirv]: m_VulkanSPIRV)
		{
			const std::filesystem::path shaderFilePath = m_FilePath;
			const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + Utils::GLShaderStageCachedOpenGLFileExtension(stage));

			std::ifstream in(cachedPath, std::ios::in | std::ios::binary);
			if (in.is_open())
			{
				in.seekg(0, std::ios::end);
				const auto size = in.tellg();
				in.seekg(0, std::ios::beg);

				auto& data = shaderData[stage];
				data.resize(size / sizeof(u32));
				in.read(reinterpret_cast<char*>(data.data()), size);
			}
			else
			{
				spirv_cross::CompilerGLSL glslCompiler(spirv);
				m_OpenGLSourceCode[stage] = glslCompiler.compile();
				auto const& source = m_OpenGLSourceCode[stage];

				shaderc::SpvCompilationResult const spirvModule = compiler.CompileGlslToSpv(source, Utils::GLShaderStageToShaderC(stage), m_FilePath.c_str());
				if (spirvModule.GetCompilationStatus() != shaderc_compilation_status_success)
				{
					OLO_CORE_ERROR(spirvModule.GetErrorMessage());
					OLO_CORE_ASSERT(false);
				}

				shaderData[stage] = std::vector<u32>(spirvModule.cbegin(), spirvModule.cend());

				std::ofstream out(cachedPath, std::ios::out | std::ios::binary);
				if (out.is_open())
				{
					auto& data = shaderData[stage];
					out.write(reinterpret_cast<char*>(data.data()), data.size() * sizeof(u32));
					out.flush();
					out.close();
				}
			}
		}
	}

	void OpenGLShader::CreateProgram()
	{
		const GLuint program = glCreateProgram();

		std::vector<GLuint> shaderIDs;
		for (auto&& [stage, spirv] : m_OpenGLSPIRV)
		{
			const GLuint shaderID = shaderIDs.emplace_back(glCreateShader(stage));
			glShaderBinary(1, &shaderID, GL_SHADER_BINARY_FORMAT_SPIR_V, spirv.data(), static_cast<GLsizei>(spirv.size() * sizeof(u32)));
			glSpecializeShader(shaderID, "main", 0, nullptr, nullptr);
			glAttachShader(program, shaderID);
		}

		glLinkProgram(program);

		GLint isLinked{};
		glGetProgramiv(program, GL_LINK_STATUS, &isLinked);
		if (GL_FALSE == isLinked)
		{
			GLint maxLength{};
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &maxLength);

			std::vector<GLchar> infoLog(maxLength);
			glGetProgramInfoLog(program, maxLength, &maxLength, infoLog.data());
			OLO_CORE_ERROR("Shader linking failed ({0}):\n{1}", m_FilePath, infoLog.data());

			glDeleteProgram(program);

			for (const auto id : shaderIDs)
			{
				glDeleteShader(id);
			}
		}

		for (const auto id : shaderIDs)
		{
			glDetachShader(program, id);
			glDeleteShader(id);
		}

		m_RendererID = program;
	}

	static bool VerifyProgramLink(GLenum const& program)
	{
		int isLinked = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &isLinked);
		if (GL_FALSE == isLinked)
		{
			GLint maxLength = 0;
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &maxLength);

			std::vector<GLchar> infoLog(maxLength);
			glGetProgramInfoLog(program, maxLength, &maxLength, &infoLog[0]);

			glDeleteProgram(program);

			OLO_CORE_ERROR("{0}", infoLog.data());
			OLO_CORE_ASSERT(false, "[OpenGL] Shader link failure!");
			return false;
		}
		return true;
	}

	void OpenGLShader::CreateProgramForAmd()
	{
		GLuint program = glCreateProgram();

		const std::filesystem::path cacheDirectory = Utils::GetCacheDirectory();
		const std::filesystem::path shaderFilePath = m_FilePath;
		const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + ".cached_opengl.pgr");

		if (std::ifstream in(cachedPath, std::ios::ate | std::ios::binary); in.is_open())
		{
			const auto size = in.tellg();
			in.seekg(0);

			auto data = std::vector<char>(size);
			u32 format = 0;
			in.read(reinterpret_cast<char*>(&format), sizeof(u32));
			in.read(data.data(), size);
			glProgramBinary(program, format, data.data(), static_cast<GLsizei>(data.size()));

			const bool linked = VerifyProgramLink(program);

			if (!linked)
			{
				return;
			}
		}
		else
		{
			std::array<u32, 2> glShadersIDs{};
			CompileOpenGLBinariesForAmd(program, glShadersIDs);
			glLinkProgram(program);


			if (const bool linked = VerifyProgramLink(program))
			{
				// Save program data
				GLint formats = 0;
				glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
				OLO_CORE_ASSERT(formats > 0, "Driver does not support binary format");
				Utils::CreateCacheDirectoryIfNeeded();
				GLint length = 0;
				glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
				auto shaderData = std::vector<char>(length);
				u32 format = 0;
				glGetProgramBinary(program, length, nullptr, &format, shaderData.data());
				std::ofstream out(cachedPath, std::ios::out | std::ios::binary);
				if (out.is_open())
				{
					out.write(reinterpret_cast<char*>(&format), sizeof(u32));
					out.write(shaderData.data(), shaderData.size());
					out.flush();
					out.close();
				}
			}

			for (auto const& id : glShadersIDs)
			{
				glDetachShader(program, id);
			}
		}

		m_RendererID = program;
	}

	void OpenGLShader::CompileOpenGLBinariesForAmd(GLenum const& program, std::array<u32, 2>& glShadersIDs) const
	{
		int glShaderIDIndex = 0;
		for (auto&& [stage, spirv] : m_VulkanSPIRV)
		{
			spirv_cross::CompilerGLSL glslCompiler(spirv);
			const auto source = glslCompiler.compile();

			u32 shader = glCreateShader(stage);

			const GLchar* const sourceCStr = source.c_str();
			glShaderSource(shader, 1, &sourceCStr, nullptr);

			glCompileShader(shader);

			int isCompiled = 0;
			glGetShaderiv(shader, GL_COMPILE_STATUS, &isCompiled);
			if (GL_FALSE == isCompiled)
			{
				int maxLength = 0;
				glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &maxLength);

				std::vector<char> infoLog(maxLength);
				glGetShaderInfoLog(shader, maxLength, &maxLength, &infoLog[0]);

				glDeleteShader(shader);

				OLO_CORE_ERROR("{0}", infoLog.data());
				OLO_CORE_ASSERT(false, "[OpenGL] Shader compilation failure!");
				return;
			}
			glAttachShader(program, shader);
			glShadersIDs[glShaderIDIndex++] = shader;
		}
	}

	void OpenGLShader::Reflect(const GLenum stage, const std::vector<u32>& shaderData)
	{
		const spirv_cross::Compiler compiler(shaderData);
		const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

		OLO_CORE_TRACE("OpenGLShader::Reflect - {0} {1}", Utils::GLShaderStageToString(stage), m_FilePath);
		OLO_CORE_TRACE("    {0} uniform buffers", resources.uniform_buffers.size());
		OLO_CORE_TRACE("    {0} resources", resources.sampled_images.size());

		OLO_CORE_TRACE("Uniform buffers:");
		for (const auto& resource : resources.uniform_buffers)
		{
			const auto& bufferType = compiler.get_type(resource.base_type_id);
			sizet bufferSize = compiler.get_declared_struct_size(bufferType);
			u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
			sizet memberCount = bufferType.member_types.size();

			OLO_CORE_TRACE("  {0}", resource.name);
			OLO_CORE_TRACE("    Size = {0}", bufferSize);
			OLO_CORE_TRACE("    Binding = {0}", binding);
			OLO_CORE_TRACE("    Members = {0}", memberCount);
		}
	}

	void OpenGLShader::Bind() const
	{
		OLO_PROFILE_FUNCTION();

		glUseProgram(m_RendererID);
	}

	void OpenGLShader::Unbind() const
	{
		OLO_PROFILE_FUNCTION();

		glUseProgram(0);
	}

	void OpenGLShader::SetInt(const std::string& name, const int value)
	{
		OLO_PROFILE_FUNCTION();

		UploadUniformInt(name, value);
	}

	void OpenGLShader::SetIntArray(const std::string& name, int* const values, const u32 count)
	{
		UploadUniformIntArray(name, values, count);
	}

	void OpenGLShader::SetFloat(const std::string& name, const f32 value)
	{
		OLO_PROFILE_FUNCTION();

		UploadUniformFloat(name, value);
	}

	void OpenGLShader::SetFloat2(const std::string& name, const glm::vec2& value)
	{
		OLO_PROFILE_FUNCTION();

		UploadUniformFloat2(name, value);
	}

	void OpenGLShader::SetFloat3(const std::string& name, const glm::vec3& value)
	{
		OLO_PROFILE_FUNCTION();

		UploadUniformFloat3(name, value);
	}

	void OpenGLShader::SetFloat4(const std::string& name, const glm::vec4& value)
	{
		OLO_PROFILE_FUNCTION();

		UploadUniformFloat4(name, value);
	}

	void OpenGLShader::SetMat4(const std::string& name, const glm::mat4& value)
	{
		OLO_PROFILE_FUNCTION();

		UploadUniformMat4(name, value);
	}

	void OpenGLShader::UploadUniformInt(const std::string& name, const int value) const
	{
		const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
		glUniform1i(location, value);
	}

	void OpenGLShader::UploadUniformIntArray(const std::string& name, int const* const values, const u32 count) const
	{
		const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
		glUniform1iv(location, count, values);
	}

	void OpenGLShader::UploadUniformFloat(const std::string& name, const f32 value) const
	{
		const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
		glUniform1f(location, value);
	}

	void OpenGLShader::UploadUniformFloat2(const std::string& name, const glm::vec2& value) const
	{
		const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
		glUniform2f(location, value.x, value.y);
	}

	void OpenGLShader::UploadUniformFloat3(const std::string& name, const glm::vec3& value) const
	{
		const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
		glUniform3f(location, value.x, value.y, value.z);
	}

	void OpenGLShader::UploadUniformFloat4(const std::string& name, const glm::vec4& value) const
	{
		const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
		glUniform4f(location, value.x, value.y, value.z, value.w);
	}

	void OpenGLShader::UploadUniformMat3(const std::string& name, const glm::mat3& matrix) const
	{
		const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
		glUniformMatrix3fv(location, 1, GL_FALSE, glm::value_ptr(matrix));
	}

	void OpenGLShader::UploadUniformMat4(const std::string& name, const glm::mat4& matrix) const
	{
		const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
		glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(matrix));
	}

}
