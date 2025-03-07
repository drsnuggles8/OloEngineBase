#pragma once

#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
	// Forward declarations
	class Mesh;
	
	// Material properties for 3D lighting
	struct Material
	{
		glm::vec3 Ambient = { 0.1f, 0.1f, 0.1f };
		glm::vec3 Diffuse = { 0.7f, 0.7f, 0.7f };
		glm::vec3 Specular = { 0.5f, 0.5f, 0.5f };
		float Shininess = 32.0f;
		
		// Texture maps for PBR-style rendering
		Ref<Texture2D> DiffuseMap = nullptr;
		Ref<Texture2D> SpecularMap = nullptr;
		
		// Controls whether to use texture maps or solid colors
		bool UseTextureMaps = false;
	};

	// Light types enumeration
	enum class LightType
	{
		Directional = 0,
		Point = 1,
		Spot = 2
	};

	// Base light properties common to all light types
	struct Light
	{
		LightType Type = LightType::Point;
		
		// Common light properties
		glm::vec3 Position = { 1.2f, 1.0f, 2.0f };  // Used by point and spot lights
		glm::vec3 Direction = { 0.0f, -1.0f, 0.0f }; // Used by directional and spot lights
		
		glm::vec3 Ambient = { 0.2f, 0.2f, 0.2f };
		glm::vec3 Diffuse = { 0.5f, 0.5f, 0.5f };
		glm::vec3 Specular = { 1.0f, 1.0f, 1.0f };
		
		// Point light attenuation factors
		float Constant = 1.0f;    // Should be 1.0 in most cases
		float Linear = 0.09f;     // Controls linear attenuation
		float Quadratic = 0.032f; // Controls quadratic attenuation
		
		// Spotlight properties
		float CutOff = glm::cos(glm::radians(12.5f));     // Spotlight inner cutoff (cosine of angle)
		float OuterCutOff = glm::cos(glm::radians(17.5f)); // Spotlight outer cutoff for soft edges
	};

	class Renderer3D
	{
	public:
		static void Init();
		static void Shutdown();

		static void BeginScene(const glm::mat4& viewProjectionMatrix);
		static void EndScene();

		// Draw methods
		static void DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material);
		
		// Convenience method that uses the internally stored cube mesh
		static void DrawCube(const glm::mat4& modelMatrix, const Material& material);
		
		// Special method for visualizing light sources
		static void DrawLightCube(const glm::mat4& modelMatrix);

		// Light and view setters
		static void SetLight(const Light& light);
		static void SetViewPosition(const glm::vec3& position);

	private:
		// Helper methods for updating uniform buffers
		static void UpdateTransformUBO(const glm::mat4& modelMatrix);
		static void UpdateLightPropertiesUBO(const Material& material);
		static void UpdateTextureFlag(const Material& material);
		
		static ShaderLibrary m_ShaderLibrary;
	};
}
