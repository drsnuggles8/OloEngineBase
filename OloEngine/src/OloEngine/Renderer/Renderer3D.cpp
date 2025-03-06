#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MSDFData.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
	struct Renderer3DData
	{
		Ref<VertexArray> VertexArray;
		Ref<VertexBuffer> VertexBuffer;
		Ref<Shader> LightCubeShader;
		Ref<Shader> LightingShader;
		Ref<IndexBuffer> IndexBuffer;
		Ref<UniformBuffer> UBO;
		Ref<UniformBuffer> LightPropertiesBuffer;
		Ref<UniformBuffer> TextureFlagBuffer;

		glm::mat4 ViewProjectionMatrix;

		// Light and material properties
		Light SceneLight;
		glm::vec3 ViewPos;
	};

	static Renderer3DData s_Data;

	ShaderLibrary Renderer3D::m_ShaderLibrary;

	void Renderer3D::Init()
	{
		OLO_PROFILE_FUNCTION();

		s_Data.VertexArray = VertexArray::Create();

		// Cube vertices with position, normals, and texture coordinates
		f32 vertices[] = {
			// positions             // normals           // texture coords
			// Front face
			 0.5f,  0.5f,  0.5f,     0.0f,  0.0f,  1.0f,  1.0f, 1.0f,
			 0.5f, -0.5f,  0.5f,     0.0f,  0.0f,  1.0f,  1.0f, 0.0f,
			-0.5f, -0.5f,  0.5f,     0.0f,  0.0f,  1.0f,  0.0f, 0.0f,
			-0.5f,  0.5f,  0.5f,     0.0f,  0.0f,  1.0f,  0.0f, 1.0f,

			// Back face
			 0.5f,  0.5f, -0.5f,     0.0f,  0.0f, -1.0f,  0.0f, 1.0f,
			 0.5f, -0.5f, -0.5f,     0.0f,  0.0f, -1.0f,  0.0f, 0.0f,
			-0.5f, -0.5f, -0.5f,     0.0f,  0.0f, -1.0f,  1.0f, 0.0f,
			-0.5f,  0.5f, -0.5f,     0.0f,  0.0f, -1.0f,  1.0f, 1.0f,

			// Right face
			 0.5f,  0.5f,  0.5f,     1.0f,  0.0f,  0.0f,  0.0f, 1.0f,
			 0.5f, -0.5f,  0.5f,     1.0f,  0.0f,  0.0f,  0.0f, 0.0f,
			 0.5f, -0.5f, -0.5f,     1.0f,  0.0f,  0.0f,  1.0f, 0.0f,
			 0.5f,  0.5f, -0.5f,     1.0f,  0.0f,  0.0f,  1.0f, 1.0f,

			 // Left face
			 -0.5f,  0.5f,  0.5f,    -1.0f,  0.0f,  0.0f,  1.0f, 1.0f,
			 -0.5f, -0.5f,  0.5f,    -1.0f,  0.0f,  0.0f,  1.0f, 0.0f,
			 -0.5f, -0.5f, -0.5f,    -1.0f,  0.0f,  0.0f,  0.0f, 0.0f,
			 -0.5f,  0.5f, -0.5f,    -1.0f,  0.0f,  0.0f,  0.0f, 1.0f,

			 // Top face
			  0.5f,  0.5f,  0.5f,     0.0f,  1.0f,  0.0f,  1.0f, 0.0f,
			  0.5f,  0.5f, -0.5f,     0.0f,  1.0f,  0.0f,  1.0f, 1.0f,
			 -0.5f,  0.5f, -0.5f,     0.0f,  1.0f,  0.0f,  0.0f, 1.0f,
			 -0.5f,  0.5f,  0.5f,     0.0f,  1.0f,  0.0f,  0.0f, 0.0f,

			 // Bottom face
			  0.5f, -0.5f,  0.5f,     0.0f, -1.0f,  0.0f,  1.0f, 1.0f,
			  0.5f, -0.5f, -0.5f,     0.0f, -1.0f,  0.0f,  1.0f, 0.0f,
			 -0.5f, -0.5f, -0.5f,     0.0f, -1.0f,  0.0f,  0.0f, 0.0f,
			 -0.5f, -0.5f,  0.5f,     0.0f, -1.0f,  0.0f,  0.0f, 1.0f
		};

		u32 indices[] = {
			// front face
			0, 1, 3, 1, 2, 3,
			// back face
			4, 5, 7, 5, 6, 7,
			// right face
			8, 9, 11, 9, 10, 11,
			// left face
			12, 13, 15, 13, 14, 15,
			// top face
			16, 17, 19, 17, 18, 19,
			// bottom face
			20, 21, 23, 21, 22, 23
		};

		s_Data.VertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
		s_Data.VertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position" },
			{ ShaderDataType::Float3, "a_Normal" },
			{ ShaderDataType::Float2, "a_TexCoord" }
		});
		s_Data.VertexArray->AddVertexBuffer(s_Data.VertexBuffer);

		s_Data.IndexBuffer = IndexBuffer::Create(indices, sizeof(indices) / sizeof(u32));
		s_Data.VertexArray->SetIndexBuffer(s_Data.IndexBuffer);

		m_ShaderLibrary.Load("assets/shaders/LightCube.glsl");
		m_ShaderLibrary.Load("assets/shaders/Lighting3D.glsl");

		s_Data.LightCubeShader = m_ShaderLibrary.Get("LightCube");
		s_Data.LightingShader = m_ShaderLibrary.Get("Lighting3D");

		// Create uniform buffers with proper sizes
		s_Data.UBO = UniformBuffer::Create(sizeof(glm::mat4) * 2, 0);
		
		// Calculate the total size needed for all lighting properties
		// Material: 3 vec4s (ambient, diffuse, specular+shininess) + 1 vec4 padding = 4 vec4s
		// Light: Position vec4 + Direction vec4 + 3 color vec4s + 2 param vec4s = 6 vec4s
		// ViewPos + LightType: 1 vec4
		// Total: 11 vec4s = 11 * 16 bytes = 176 bytes
		s_Data.LightPropertiesBuffer = UniformBuffer::Create(sizeof(glm::vec4) * 12, 1);
		
		s_Data.TextureFlagBuffer = UniformBuffer::Create(sizeof(int), 2);

		// Set default values
		s_Data.SceneLight.Position = glm::vec3(1.2f, 1.0f, 2.0f);
		s_Data.SceneLight.Ambient = glm::vec3(0.2f, 0.2f, 0.2f);
		s_Data.SceneLight.Diffuse = glm::vec3(0.5f, 0.5f, 0.5f);
		s_Data.SceneLight.Specular = glm::vec3(1.0f, 1.0f, 1.0f);

		s_Data.ViewPos = glm::vec3(0.0f, 0.0f, 3.0f);
	}

	void Renderer3D::Shutdown()
	{
		OLO_PROFILE_FUNCTION();
	}

	void Renderer3D::BeginScene(const glm::mat4& viewProjectionMatrix)
	{
		s_Data.ViewProjectionMatrix = viewProjectionMatrix;
	}

	void Renderer3D::SetLight(const Light& light)
	{
		s_Data.SceneLight = light;
	}

	void Renderer3D::SetViewPosition(const glm::vec3& position)
	{
		s_Data.ViewPos = position;
	}

	void Renderer3D::EndScene()
	{}

	void Renderer3D::DrawCube(const glm::mat4& modelMatrix, const Material& material)
	{
		s_Data.LightingShader->Bind();

		// Update the UBO with the view projection and model matrices
		UniformData viewProjectionData = { &s_Data.ViewProjectionMatrix, sizeof(glm::mat4), 0 };
		UniformData modelData = { &modelMatrix, sizeof(glm::mat4), sizeof(glm::mat4) };
		s_Data.UBO->SetData(viewProjectionData);
		s_Data.UBO->SetData(modelData);

		// Prepare material data with padding for std140 layout
		glm::vec4 materialAmbient(material.Ambient, 0.0f);
		glm::vec4 materialDiffuse(material.Diffuse, 0.0f);
		glm::vec4 materialSpecular(material.Specular, material.Shininess);
		glm::vec4 padding1(0.0f); // Padding to align to vec4 boundary

		// Prepare light data with padding for std140 layout
		int lightType = static_cast<int>(s_Data.SceneLight.Type);
		glm::vec4 lightPosition(s_Data.SceneLight.Position, 0.0f);
		glm::vec4 lightDirection(s_Data.SceneLight.Direction, 0.0f);
		glm::vec4 lightAmbient(s_Data.SceneLight.Ambient, 0.0f);
		glm::vec4 lightDiffuse(s_Data.SceneLight.Diffuse, 0.0f);
		glm::vec4 lightSpecular(s_Data.SceneLight.Specular, 0.0f);
		
		// Attenuation parameters and spotlight parameters
		glm::vec4 lightAttenuationParams(
			s_Data.SceneLight.Constant, 
			s_Data.SceneLight.Linear, 
			s_Data.SceneLight.Quadratic, 
			0.0f // padding
		);
		
		glm::vec4 lightSpotParams(
			s_Data.SceneLight.CutOff,
			s_Data.SceneLight.OuterCutOff,
			0.0f, // padding
			0.0f  // padding
		);
		
		glm::vec4 viewPos(s_Data.ViewPos, static_cast<float>(lightType)); // Use w component to store light type

		// Update the LightProperties UBO with proper offsets
		// Use precise offsets to match the std140 layout requirements
		size_t offset = 0;
		UniformData materialAmbientData = { &materialAmbient, sizeof(glm::vec4), offset };
		offset += sizeof(glm::vec4);
		
		UniformData materialDiffuseData = { &materialDiffuse, sizeof(glm::vec4), offset };
		offset += sizeof(glm::vec4);
		
		UniformData materialSpecularData = { &materialSpecular, sizeof(glm::vec4), offset };
		offset += sizeof(glm::vec4);
		
		UniformData paddingData = { &padding1, sizeof(glm::vec4), offset };
		offset += sizeof(glm::vec4);
		
		UniformData lightPositionData = { &lightPosition, sizeof(glm::vec4), offset };
		offset += sizeof(glm::vec4);
		
		UniformData lightDirectionData = { &lightDirection, sizeof(glm::vec4), offset };
		offset += sizeof(glm::vec4);
		
		UniformData lightAmbientData = { &lightAmbient, sizeof(glm::vec4), offset };
		offset += sizeof(glm::vec4);
		
		UniformData lightDiffuseData = { &lightDiffuse, sizeof(glm::vec4), offset };
		offset += sizeof(glm::vec4);
		
		UniformData lightSpecularData = { &lightSpecular, sizeof(glm::vec4), offset };
		offset += sizeof(glm::vec4);
		
		UniformData lightAttenuationData = { &lightAttenuationParams, sizeof(glm::vec4), offset };
		offset += sizeof(glm::vec4);
		
		UniformData lightSpotParamsData = { &lightSpotParams, sizeof(glm::vec4), offset };
		offset += sizeof(glm::vec4);
		
		UniformData viewPosData = { &viewPos, sizeof(glm::vec4), offset };

		s_Data.LightPropertiesBuffer->SetData(materialAmbientData);
		s_Data.LightPropertiesBuffer->SetData(materialDiffuseData);
		s_Data.LightPropertiesBuffer->SetData(materialSpecularData);
		s_Data.LightPropertiesBuffer->SetData(paddingData);
		s_Data.LightPropertiesBuffer->SetData(lightPositionData);
		s_Data.LightPropertiesBuffer->SetData(lightDirectionData);
		s_Data.LightPropertiesBuffer->SetData(lightAmbientData);
		s_Data.LightPropertiesBuffer->SetData(lightDiffuseData);
		s_Data.LightPropertiesBuffer->SetData(lightSpecularData);
		s_Data.LightPropertiesBuffer->SetData(lightAttenuationData);
		s_Data.LightPropertiesBuffer->SetData(lightSpotParamsData);
		s_Data.LightPropertiesBuffer->SetData(viewPosData);

		// Set texture usage flag
		int useTextureMaps = material.UseTextureMaps ? 1 : 0;
		UniformData textureFlagData = { &useTextureMaps, sizeof(int), 0 };
		s_Data.TextureFlagBuffer->SetData(textureFlagData);

		// Bind texture maps if we're using them
		if (material.UseTextureMaps)
		{
			if (material.DiffuseMap)
			{
				material.DiffuseMap->Bind(0);
			}

			if (material.SpecularMap)
			{
				material.SpecularMap->Bind(1);
			}
		}

		s_Data.VertexArray->Bind();
		RenderCommand::DrawIndexed(s_Data.VertexArray);
	}

	void Renderer3D::DrawLightCube(const glm::mat4& modelMatrix)
	{
		s_Data.LightCubeShader->Bind();

		// Update the UBO with the view projection and model matrices
		UniformData viewProjectionData = { &s_Data.ViewProjectionMatrix, sizeof(glm::mat4), 0 };
		UniformData modelData = { &modelMatrix, sizeof(glm::mat4), sizeof(glm::mat4) };
		s_Data.UBO->SetData(viewProjectionData);
		s_Data.UBO->SetData(modelData);

		s_Data.VertexArray->Bind();
		RenderCommand::DrawIndexed(s_Data.VertexArray);
	}
}
