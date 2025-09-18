#include "OloEnginePCH.h"

#include "OloEngine/Project/ProjectSerializer.h"
#include "OloEngine/Physics3D/Physics3DSystem.h"
#include "OloEngine/Physics3D/PhysicsLayer.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

// YAML conversion for glm::vec3
namespace YAML {
	template<>
	struct convert<glm::vec3> {
		static Node encode(const glm::vec3& rhs) {
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			return node;
		}

		static bool decode(const Node& node, glm::vec3& rhs) {
			if (!node.IsSequence() || node.size() != 3) {
				return false;
			}

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			return true;
		}
	};
}

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

			// Physics settings serialization
			out << YAML::Key << "Physics" << YAML::Value;
			{
				out << YAML::BeginMap;

			const auto& physicsSettings = Physics3DSystem::GetSettings();

			out << YAML::Key << "FixedTimestep" << YAML::Value << physicsSettings.m_FixedTimestep;
			out << YAML::Key << "Gravity" << YAML::Value << physicsSettings.m_Gravity;
			out << YAML::Key << "PositionSolverIterations" << YAML::Value << physicsSettings.m_PositionSolverIterations;
			out << YAML::Key << "VelocitySolverIterations" << YAML::Value << physicsSettings.m_VelocitySolverIterations;
			out << YAML::Key << "MaxBodies" << YAML::Value << physicsSettings.m_MaxBodies;
			out << YAML::Key << "MaxBodyPairs" << YAML::Value << physicsSettings.m_MaxBodyPairs;
			out << YAML::Key << "MaxContactConstraints" << YAML::Value << physicsSettings.m_MaxContactConstraints;

			out << YAML::Key << "CaptureOnPlay" << YAML::Value << physicsSettings.m_CaptureOnPlay;
			out << YAML::Key << "CaptureMethod" << YAML::Value << static_cast<i32>(physicsSettings.m_CaptureMethod);

			// Advanced Jolt settings
			out << YAML::Key << "Baumgarte" << YAML::Value << physicsSettings.m_Baumgarte;
			out << YAML::Key << "SpeculativeContactDistance" << YAML::Value << physicsSettings.m_SpeculativeContactDistance;
			out << YAML::Key << "PenetrationSlop" << YAML::Value << physicsSettings.m_PenetrationSlop;
			out << YAML::Key << "LinearCastThreshold" << YAML::Value << physicsSettings.m_LinearCastThreshold;
			out << YAML::Key << "MinVelocityForRestitution" << YAML::Value << physicsSettings.m_MinVelocityForRestitution;
			out << YAML::Key << "TimeBeforeSleep" << YAML::Value << physicsSettings.m_TimeBeforeSleep;
			out << YAML::Key << "PointVelocitySleepThreshold" << YAML::Value << physicsSettings.m_PointVelocitySleepThreshold;

			// Boolean settings
			out << YAML::Key << "DeterministicSimulation" << YAML::Value << physicsSettings.m_DeterministicSimulation;
			out << YAML::Key << "ConstraintWarmStart" << YAML::Value << physicsSettings.m_ConstraintWarmStart;
			out << YAML::Key << "UseBodyPairContactCache" << YAML::Value << physicsSettings.m_UseBodyPairContactCache;
			out << YAML::Key << "UseManifoldReduction" << YAML::Value << physicsSettings.m_UseManifoldReduction;
			out << YAML::Key << "UseLargeIslandSplitter" << YAML::Value << physicsSettings.m_UseLargeIslandSplitter;
			out << YAML::Key << "AllowSleeping" << YAML::Value << physicsSettings.m_AllowSleeping;				// Physics layers serialization
				if (PhysicsLayerManager::GetLayerCount() > 1)
				{
					out << YAML::Key << "Layers";
					out << YAML::Value << YAML::BeginSeq;
					
					// Reusable vector to avoid allocations in the loop
					std::vector<PhysicsLayer> collidingLayers;
					
					for (const auto& layer : PhysicsLayerManager::GetLayers())
					{
						// Never serialize the Default layer (layer 0)
						if (layer.m_LayerID == 0)
							continue;

						out << YAML::BeginMap;
						out << YAML::Key << "Name" << YAML::Value << layer.m_Name;
						out << YAML::Key << "CollidesWithSelf" << YAML::Value << layer.m_CollidesWithSelf;
						out << YAML::Key << "CollidesWith" << YAML::Value;
						out << YAML::BeginSeq;
						
						PhysicsLayerManager::GetLayerCollisions(layer.m_LayerID, collidingLayers);
						for (const auto& collidingLayer : collidingLayers)
						{
							out << YAML::BeginMap;
							out << YAML::Key << "Name" << YAML::Value << collidingLayer.m_Name;
							out << YAML::EndMap;
						}
						out << YAML::EndSeq;
						out << YAML::EndMap;
					}
					out << YAML::EndSeq;
				}

				out << YAML::EndMap;
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

		// Helper lambda for asset-relative paths (StartScene, ScriptModulePath)
		auto safeGetAssetPath = [&](const YAML::Node& node, const char* key, std::filesystem::path& output, const std::filesystem::path& assetDir) -> bool
		{
			auto childNode = node[key];
			if (!childNode || !childNode.IsScalar())
			{
				OLO_CORE_ERROR("Failed to load project file '{0}' - missing or invalid '{1}' field", filepath, key);
				return false;
			}
			
			std::filesystem::path extractedPath = childNode.as<std::string>();
			
			// Resolve relative paths against the asset directory within the project
			if (extractedPath.is_relative())
			{
				output = assetDir / extractedPath;
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
		
		// First get name and asset directory since other paths depend on asset directory
		bool nameValid = safeGetString(projectNode, "Name", name);
		bool assetDirectoryValid = safeGetPath(projectNode, "AssetDirectory", assetDirectory);
		
		// Now resolve asset-relative paths using the asset directory
		bool startSceneValid = false;
		bool scriptModulePathValid = false;
		
		if (assetDirectoryValid)
		{
			startSceneValid = safeGetAssetPath(projectNode, "StartScene", startScene, assetDirectory);
			scriptModulePathValid = safeGetAssetPath(projectNode, "ScriptModulePath", scriptModulePath, assetDirectory);
		}
		
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

		// Physics settings deserialization
		auto physicsNode = data["Physics"];
		if (physicsNode)
		{
		auto& physicsSettings = Physics3DSystem::GetSettings();

		// Basic simulation settings
		physicsSettings.m_FixedTimestep = physicsNode["FixedTimestep"].as<f32>(physicsSettings.m_FixedTimestep);
		physicsSettings.m_Gravity = physicsNode["Gravity"].as<glm::vec3>(physicsSettings.m_Gravity);
		physicsSettings.m_PositionSolverIterations = physicsNode["PositionSolverIterations"].as<u32>(physicsSettings.m_PositionSolverIterations);
		physicsSettings.m_VelocitySolverIterations = physicsNode["VelocitySolverIterations"].as<u32>(physicsSettings.m_VelocitySolverIterations);
		physicsSettings.m_MaxBodies = physicsNode["MaxBodies"].as<u32>(physicsSettings.m_MaxBodies);
		physicsSettings.m_MaxBodyPairs = physicsNode["MaxBodyPairs"].as<u32>(physicsSettings.m_MaxBodyPairs);
		physicsSettings.m_MaxContactConstraints = physicsNode["MaxContactConstraints"].as<u32>(physicsSettings.m_MaxContactConstraints);

		// Debug settings
		physicsSettings.m_CaptureOnPlay = physicsNode["CaptureOnPlay"].as<bool>(physicsSettings.m_CaptureOnPlay);
		physicsSettings.m_CaptureMethod = static_cast<PhysicsDebugType>(physicsNode["CaptureMethod"].as<i32>(static_cast<i32>(physicsSettings.m_CaptureMethod)));

		// Advanced Jolt settings
		physicsSettings.m_Baumgarte = physicsNode["Baumgarte"].as<f32>(physicsSettings.m_Baumgarte);
		physicsSettings.m_SpeculativeContactDistance = physicsNode["SpeculativeContactDistance"].as<f32>(physicsSettings.m_SpeculativeContactDistance);
		physicsSettings.m_PenetrationSlop = physicsNode["PenetrationSlop"].as<f32>(physicsSettings.m_PenetrationSlop);
		physicsSettings.m_LinearCastThreshold = physicsNode["LinearCastThreshold"].as<f32>(physicsSettings.m_LinearCastThreshold);
		physicsSettings.m_MinVelocityForRestitution = physicsNode["MinVelocityForRestitution"].as<f32>(physicsSettings.m_MinVelocityForRestitution);
		physicsSettings.m_TimeBeforeSleep = physicsNode["TimeBeforeSleep"].as<f32>(physicsSettings.m_TimeBeforeSleep);
		physicsSettings.m_PointVelocitySleepThreshold = physicsNode["PointVelocitySleepThreshold"].as<f32>(physicsSettings.m_PointVelocitySleepThreshold);

		// Boolean settings
		physicsSettings.m_DeterministicSimulation = physicsNode["DeterministicSimulation"].as<bool>(physicsSettings.m_DeterministicSimulation);
		physicsSettings.m_ConstraintWarmStart = physicsNode["ConstraintWarmStart"].as<bool>(physicsSettings.m_ConstraintWarmStart);
		physicsSettings.m_UseBodyPairContactCache = physicsNode["UseBodyPairContactCache"].as<bool>(physicsSettings.m_UseBodyPairContactCache);
		physicsSettings.m_UseManifoldReduction = physicsNode["UseManifoldReduction"].as<bool>(physicsSettings.m_UseManifoldReduction);
		physicsSettings.m_UseLargeIslandSplitter = physicsNode["UseLargeIslandSplitter"].as<bool>(physicsSettings.m_UseLargeIslandSplitter);
		physicsSettings.m_AllowSleeping = physicsNode["AllowSleeping"].as<bool>(physicsSettings.m_AllowSleeping);			// Physics layers deserialization
			auto physicsLayers = physicsNode["Layers"];
			if (physicsLayers)
			{
				// Clear existing layers (except default)
				PhysicsLayerManager::ClearLayers();
				
				for (auto layer : physicsLayers)
				{
					const std::string layerName = layer["Name"].as<std::string>();
					u32 layerId = PhysicsLayerManager::AddLayer(layerName, false);
					
					PhysicsLayer& layerInfo = PhysicsLayerManager::GetLayer(layerId);
					layerInfo.m_CollidesWithSelf = layer["CollidesWithSelf"].as<bool>(true);

					auto collidesWith = layer["CollidesWith"];
					if (collidesWith)
					{
						for (auto collisionLayer : collidesWith)
						{
							const std::string otherLayerName = collisionLayer["Name"].as<std::string>();
							const auto& otherLayer = PhysicsLayerManager::GetLayer(otherLayerName);
							if (otherLayer.IsValid())
							{
								PhysicsLayerManager::SetLayerCollision(layerInfo.m_LayerID, otherLayer.m_LayerID, true);
							}
						}
					}
				}
			}

			// Apply the physics settings
			Physics3DSystem::ApplySettings();
		}
		
		return allValid;
	}
}
