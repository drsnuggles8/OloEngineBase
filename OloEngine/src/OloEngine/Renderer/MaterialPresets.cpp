#include "OloEnginePCH.h"
#include "MaterialPresets.h"

namespace OloEngine {

	namespace {
		// Helper function to create a material with clamped values and consistent setup
		Material CreateMaterialHelper(const std::string& name, const glm::vec3& baseColor, float metallic, float roughness)
		{
			// Clamp values to valid PBR ranges
			float clampedMetallic = glm::clamp(metallic, 0.0f, 1.0f);
			float clampedRoughness = glm::clamp(roughness, 0.0f, 1.0f);
			glm::vec3 clampedColor = glm::clamp(baseColor, glm::vec3(0.0f), glm::vec3(1.0f));
			
			auto materialRef = Material::CreatePBR(name, clampedColor, clampedMetallic, clampedRoughness);
			// Work directly with the reference for efficiency, then move for return
			materialRef->SetName(name);
			return std::move(*materialRef); // Use move semantics for efficient return
		}
	}

	Material MaterialPresets::CreateGold(const std::string& name)
	{
		return CreateMaterialHelper(name, glm::vec3(1.0f, 0.765f, 0.336f), 1.0f, 0.1f);
	}

	Material MaterialPresets::CreateSilver(const std::string& name)
	{
		return CreateMaterialHelper(name, glm::vec3(0.972f, 0.960f, 0.915f), 1.0f, 0.1f);
	}

	Material MaterialPresets::CreateCopper(const std::string& name)
	{
		return CreateMaterialHelper(name, glm::vec3(0.955f, 0.637f, 0.538f), 1.0f, 0.1f);
	}

	Material MaterialPresets::CreateAluminum(const std::string& name)
	{
		return CreateMaterialHelper(name, glm::vec3(0.913f, 0.921f, 0.925f), 1.0f, 0.1f);
	}

	Material MaterialPresets::CreateIron(const std::string& name)
	{
		return CreateMaterialHelper(name, glm::vec3(0.560f, 0.570f, 0.580f), 1.0f, 0.15f);
	}

	Material MaterialPresets::CreateChrome(const std::string& name)
	{
		return CreateMaterialHelper(name, glm::vec3(0.549f, 0.556f, 0.554f), 1.0f, 0.05f);
	}

	Material MaterialPresets::CreatePlastic(const std::string& name, const glm::vec3& color)
	{
		return CreateMaterialHelper(name, color, 0.0f, 0.5f);
	}

	Material MaterialPresets::CreateRubber(const std::string& name, const glm::vec3& color)
	{
		return CreateMaterialHelper(name, color, 0.0f, 0.9f);
	}

	Material MaterialPresets::CreateCeramic(const std::string& name, const glm::vec3& color)
	{
		return CreateMaterialHelper(name, color, 0.0f, 0.1f);
	}

	Material MaterialPresets::CreateWood(const std::string& name, const glm::vec3& color)
	{
		return CreateMaterialHelper(name, color, 0.0f, 0.8f);
	}

	Material MaterialPresets::CreateConcrete(const std::string& name, const glm::vec3& color)
	{
		return CreateMaterialHelper(name, color, 0.0f, 0.9f);
	}

	Material MaterialPresets::CreateGlass(const std::string& name, const glm::vec3& color)
	{
		return CreateMaterialHelper(name, color, 0.0f, 0.0f);
	}

	Material MaterialPresets::CreateEmissive(const std::string& name, const glm::vec3& color, float intensity)
	{
		Material material = CreateMaterialHelper(name, color, 0.0f, 0.5f);
		float clampedIntensity = glm::clamp(intensity, 0.0f, 10.0f); // Reasonable range for emissive intensity
		material.SetEmissiveFactor(glm::vec4(color * clampedIntensity, 1.0f));
		return material;
	}

	Material MaterialPresets::CreateMetal(const std::string& name, const glm::vec3& baseColor, float roughness)
	{
		return CreateMaterialHelper(name, baseColor, 1.0f, roughness);
	}

	Material MaterialPresets::CreateNonMetal(const std::string& name, const glm::vec3& baseColor, float roughness)
	{
		return CreateMaterialHelper(name, baseColor, 0.0f, roughness);
	}

}
