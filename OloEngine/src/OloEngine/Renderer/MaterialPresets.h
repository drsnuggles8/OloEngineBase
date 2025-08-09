#pragma once

#include "OloEngine/Renderer/Material.h"

namespace OloEngine {

	/**
	 * @brief Material presets utility class for common material configurations
	 * 
	 * This class provides factory methods for creating commonly used materials
	 * without cluttering the core Material class. These are essentially
	 * convenience presets that use the standard Material::CreatePBR method
	 * with predefined realistic PBR values.
	 */
	class MaterialPresets
	{
	public:
		// Metal presets
		static Material CreateGold(const std::string& name = "Gold");
		static Material CreateSilver(const std::string& name = "Silver");
		static Material CreateCopper(const std::string& name = "Copper");
		static Material CreateAluminum(const std::string& name = "Aluminum");
		static Material CreateIron(const std::string& name = "Iron");
		static Material CreateChrome(const std::string& name = "Chrome");
		
		// Non-metal presets
		static Material CreatePlastic(const std::string& name = "Plastic", const glm::vec3& color = glm::vec3(0.1f, 0.1f, 0.8f));
		static Material CreateRubber(const std::string& name = "Rubber", const glm::vec3& color = glm::vec3(0.2f, 0.2f, 0.2f));
		static Material CreateCeramic(const std::string& name = "Ceramic", const glm::vec3& color = glm::vec3(0.9f, 0.9f, 0.9f));
		static Material CreateWood(const std::string& name = "Wood", const glm::vec3& color = glm::vec3(0.6f, 0.4f, 0.2f));
		static Material CreateConcrete(const std::string& name = "Concrete", const glm::vec3& color = glm::vec3(0.7f, 0.7f, 0.7f));
		
		// Special effect presets
		static Material CreateGlass(const std::string& name = "Glass", const glm::vec3& color = glm::vec3(0.95f, 0.95f, 0.95f));
		static Material CreateEmissive(const std::string& name = "Emissive", const glm::vec3& color = glm::vec3(1.0f, 1.0f, 1.0f), float intensity = 1.0f);
		
		// Utility methods for custom materials with common patterns
		static Material CreateMetal(const std::string& name, const glm::vec3& baseColor, float roughness = 0.1f);
		static Material CreateNonMetal(const std::string& name, const glm::vec3& baseColor, float roughness = 0.5f);
		
	private:
		MaterialPresets() = delete; // Static utility class
	};

}
