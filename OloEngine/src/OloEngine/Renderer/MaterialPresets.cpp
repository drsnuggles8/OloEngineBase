#include "OloEnginePCH.h"
#include "MaterialPresets.h"

namespace OloEngine {

	Material MaterialPresets::CreateGold(const std::string& name)
	{
		auto materialRef = Material::CreatePBR(name, glm::vec3(1.0f, 0.765f, 0.336f), 1.0f, 0.1f);
		Material material = *materialRef; // Copy to value type for backward compatibility
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreateSilver(const std::string& name)
	{
		auto materialRef = Material::CreatePBR(name, glm::vec3(0.972f, 0.960f, 0.915f), 1.0f, 0.1f);
		Material material = *materialRef; // Copy to value type for backward compatibility
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreateCopper(const std::string& name)
	{
		auto materialRef = Material::CreatePBR(name, glm::vec3(0.955f, 0.637f, 0.538f), 1.0f, 0.1f);
		Material material = *materialRef; // Copy to value type for backward compatibility
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreateAluminum(const std::string& name)
	{
		auto materialRef = Material::CreatePBR(name, glm::vec3(0.913f, 0.921f, 0.925f), 1.0f, 0.1f);
		Material material = *materialRef;
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreateIron(const std::string& name)
	{
		auto materialRef = Material::CreatePBR(name, glm::vec3(0.560f, 0.570f, 0.580f), 1.0f, 0.15f);
		Material material = *materialRef;
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreateChrome(const std::string& name)
	{
		auto materialRef = Material::CreatePBR(name, glm::vec3(0.549f, 0.556f, 0.554f), 1.0f, 0.05f);
		Material material = *materialRef;
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreatePlastic(const std::string& name, const glm::vec3& color)
	{
		auto materialRef = Material::CreatePBR(name, color, 0.0f, 0.5f);
		Material material = *materialRef; // Copy to value type for backward compatibility
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreateRubber(const std::string& name, const glm::vec3& color)
	{
		auto materialRef = Material::CreatePBR(name, color, 0.0f, 0.9f);
		Material material = *materialRef;
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreateCeramic(const std::string& name, const glm::vec3& color)
	{
		auto materialRef = Material::CreatePBR(name, color, 0.0f, 0.1f);
		Material material = *materialRef;
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreateWood(const std::string& name, const glm::vec3& color)
	{
		auto materialRef = Material::CreatePBR(name, color, 0.0f, 0.8f);
		Material material = *materialRef;
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreateConcrete(const std::string& name, const glm::vec3& color)
	{
		auto materialRef = Material::CreatePBR(name, color, 0.0f, 0.9f);
		Material material = *materialRef;
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreateGlass(const std::string& name, const glm::vec3& color)
	{
		auto materialRef = Material::CreatePBR(name, color, 0.0f, 0.0f);
		Material material = *materialRef;
		material.SetName(name);
		// Glass typically has some transparency, but we'll keep it simple for now
		return material;
	}

	Material MaterialPresets::CreateEmissive(const std::string& name, const glm::vec3& color, float intensity)
	{
		auto materialRef = Material::CreatePBR(name, color, 0.0f, 0.5f);
		Material material = *materialRef;
		material.SetName(name);
		material.SetEmissiveFactor(glm::vec4(color * intensity, 1.0f));
		return material;
	}

	Material MaterialPresets::CreateMetal(const std::string& name, const glm::vec3& baseColor, float roughness)
	{
		auto materialRef = Material::CreatePBR(name, baseColor, 1.0f, roughness);
		Material material = *materialRef;
		material.SetName(name);
		return material;
	}

	Material MaterialPresets::CreateNonMetal(const std::string& name, const glm::vec3& baseColor, float roughness)
	{
		auto materialRef = Material::CreatePBR(name, baseColor, 0.0f, roughness);
		Material material = *materialRef;
		material.SetName(name);
		return material;
	}

}
