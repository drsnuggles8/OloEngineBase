#include "OloEnginePCH.h"

#include "OloEngine/Project/ProjectSerializer.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

namespace OloEngine
{

	ProjectSerializer::ProjectSerializer(Ref<Project> project)
		: m_Project(project)
	{
	}

	bool ProjectSerializer::Serialize(const std::filesystem::path& filepath)
	{
		const auto& config = m_Project->GetConfig();

		YAML::Emitter out;
		{
			out << YAML::BeginMap; // Root
			out << YAML::Key << "Project" << YAML::Value;
			{
				out << YAML::BeginMap;// Project
				out << YAML::Key << "Name" << YAML::Value << config.Name;
				out << YAML::Key << "StartScene" << YAML::Value << config.StartScene.string();
				out << YAML::Key << "AssetDirectory" << YAML::Value << config.AssetDirectory.string();
				out << YAML::Key << "ScriptModulePath" << YAML::Value << config.ScriptModulePath.string();
				out << YAML::EndMap; // Project
			}
			out << YAML::EndMap; // Root
		}

		std::ofstream fout(filepath);
		fout << out.c_str();

		return true;
	}

	bool ProjectSerializer::Deserialize(const std::filesystem::path& filepath)
	{
		auto& config = m_Project->GetConfig();

		YAML::Node data;
		try
		{
			data = YAML::LoadFile(filepath.string());
		}
		catch (YAML::ParserException e)
		{
			OLO_CORE_ERROR("Failed to load project file '{0}'\n     {1}", filepath, e.what());
			return false;
		}

		auto projectNode = data["Project"];
		if (!projectNode)
		{
			OLO_CORE_ERROR("Failed to load project file '{0}' - missing 'Project' node", filepath);
			return false;
		}

		if (!projectNode.IsMap())
		{
			OLO_CORE_ERROR("Failed to load project file '{0}' - 'Project' node must be a map, got type {1}", filepath, static_cast<int>(projectNode.Type()));
			return false;
		}

		// Helper lambda for safe string extraction with validation
		auto safeGetString = [&](const YAML::Node& node, const char* key, std::string& output) -> bool
		{
			auto childNode = node[key];
			if (!childNode || !childNode.IsScalar())
			{
				OLO_CORE_ERROR("Failed to load project file '{0}' - missing or invalid '{1}' field", filepath, key);
				return false;
			}
			output = childNode.as<std::string>();
			return true;
		};

		// Helper lambda for safe filesystem path extraction with validation
		auto safeGetPath = [&](const YAML::Node& node, const char* key, std::filesystem::path& output) -> bool
		{
			auto childNode = node[key];
			if (!childNode || !childNode.IsScalar())
			{
				OLO_CORE_ERROR("Failed to load project file '{0}' - missing or invalid '{1}' field", filepath, key);
				return false;
			}
			
			std::filesystem::path extractedPath = childNode.as<std::string>();
			
			// Resolve relative paths against the project file's directory
			if (extractedPath.is_relative())
			{
				output = filepath.parent_path() / extractedPath;
				output = std::filesystem::weakly_canonical(output);
			}
			else
			{
				output = extractedPath;
			}
			
			// Check if the resolved path exists and log a warning if it doesn't
			std::error_code ec;
			if (!std::filesystem::exists(output, ec))
			{
				if (ec)
				{
					OLO_CORE_WARN("Failed to check existence of '{0}' path '{1}': {2}", key, output.string(), ec.message());
				}
				else
				{
					OLO_CORE_WARN("Path '{0}' for field '{1}' does not exist: {2}", key, output.string(), "Path not found");
				}
			}
			
			return true;
		};

		// Extract project configuration with proper validation
		// First, extract all values into local variables to ensure atomic update
		std::string name;
		std::filesystem::path startScene;
		std::filesystem::path assetDirectory;
		std::filesystem::path scriptModulePath;
		
		// Perform all validations separately to ensure all errors are logged
		bool nameValid = safeGetString(projectNode, "Name", name);
		bool startSceneValid = safeGetPath(projectNode, "StartScene", startScene);
		bool assetDirectoryValid = safeGetPath(projectNode, "AssetDirectory", assetDirectory);
		bool scriptModulePathValid = safeGetPath(projectNode, "ScriptModulePath", scriptModulePath);
		
		// Check if all validations succeeded
		bool allValid = nameValid && startSceneValid && assetDirectoryValid && scriptModulePathValid;
		
		// Only update config if all validations succeeded (atomic update)
		if (allValid)
		{
			config.Name = std::move(name);
			config.StartScene = std::move(startScene);
			config.AssetDirectory = std::move(assetDirectory);
			config.ScriptModulePath = std::move(scriptModulePath);
		}
		
		return allValid;
	}
}
