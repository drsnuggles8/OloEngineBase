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

				out << YAML::Key << "FixedTimestep" << YAML::Value << physicsSettings.FixedTimestep;
				out << YAML::Key << "Gravity" << YAML::Value << physicsSettings.Gravity;
				out << YAML::Key << "PositionSolverIterations" << YAML::Value << physicsSettings.PositionSolverIterations;
				out << YAML::Key << "VelocitySolverIterations" << YAML::Value << physicsSettings.VelocitySolverIterations;
				out << YAML::Key << "MaxBodies" << YAML::Value << physicsSettings.MaxBodies;
				out << YAML::Key << "MaxBodyPairs" << YAML::Value << physicsSettings.MaxBodyPairs;
				out << YAML::Key << "MaxContactConstraints" << YAML::Value << physicsSettings.MaxContactConstraints;

				out << YAML::Key << "CaptureOnPlay" << YAML::Value << physicsSettings.CaptureOnPlay;
				out << YAML::Key << "CaptureMethod" << YAML::Value << static_cast<i32>(physicsSettings.CaptureMethod);

				// Advanced Jolt settings
				out << YAML::Key << "Baumgarte" << YAML::Value << physicsSettings.Baumgarte;
				out << YAML::Key << "SpeculativeContactDistance" << YAML::Value << physicsSettings.SpeculativeContactDistance;
				out << YAML::Key << "PenetrationSlop" << YAML::Value << physicsSettings.PenetrationSlop;
				out << YAML::Key << "LinearCastThreshold" << YAML::Value << physicsSettings.LinearCastThreshold;
				out << YAML::Key << "MinVelocityForRestitution" << YAML::Value << physicsSettings.MinVelocityForRestitution;
				out << YAML::Key << "TimeBeforeSleep" << YAML::Value << physicsSettings.TimeBeforeSleep;
				out << YAML::Key << "PointVelocitySleepThreshold" << YAML::Value << physicsSettings.PointVelocitySleepThreshold;

				// Boolean settings
				out << YAML::Key << "DeterministicSimulation" << YAML::Value << physicsSettings.DeterministicSimulation;
				out << YAML::Key << "ConstraintWarmStart" << YAML::Value << physicsSettings.ConstraintWarmStart;
				out << YAML::Key << "UseBodyPairContactCache" << YAML::Value << physicsSettings.UseBodyPairContactCache;
				out << YAML::Key << "UseManifoldReduction" << YAML::Value << physicsSettings.UseManifoldReduction;
				out << YAML::Key << "UseLargeIslandSplitter" << YAML::Value << physicsSettings.UseLargeIslandSplitter;
				out << YAML::Key << "AllowSleeping" << YAML::Value << physicsSettings.AllowSleeping;

				// Physics layers serialization
				if (PhysicsLayerManager::GetLayerCount() > 1)
				{
					out << YAML::Key << "Layers";
					out << YAML::Value << YAML::BeginSeq;
					for (const auto& layer : PhysicsLayerManager::GetLayers())
					{
						// Never serialize the Default layer (layer 0)
						if (layer.LayerID == 0)
							continue;

						out << YAML::BeginMap;
						out << YAML::Key << "Name" << YAML::Value << layer.Name;
						out << YAML::Key << "CollidesWithSelf" << YAML::Value << layer.CollidesWithSelf;
						out << YAML::Key << "CollidesWith" << YAML::Value;
						out << YAML::BeginSeq;
						for (const auto& collidingLayer : PhysicsLayerManager::GetLayerCollisions(layer.LayerID))
						{
							out << YAML::BeginMap;
							out << YAML::Key << "Name" << YAML::Value << collidingLayer.Name;
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
			physicsSettings.FixedTimestep = physicsNode["FixedTimestep"].as<f32>(physicsSettings.FixedTimestep);
			physicsSettings.Gravity = physicsNode["Gravity"].as<glm::vec3>(physicsSettings.Gravity);
			physicsSettings.PositionSolverIterations = physicsNode["PositionSolverIterations"].as<u32>(physicsSettings.PositionSolverIterations);
			physicsSettings.VelocitySolverIterations = physicsNode["VelocitySolverIterations"].as<u32>(physicsSettings.VelocitySolverIterations);
			physicsSettings.MaxBodies = physicsNode["MaxBodies"].as<u32>(physicsSettings.MaxBodies);
			physicsSettings.MaxBodyPairs = physicsNode["MaxBodyPairs"].as<u32>(physicsSettings.MaxBodyPairs);
			physicsSettings.MaxContactConstraints = physicsNode["MaxContactConstraints"].as<u32>(physicsSettings.MaxContactConstraints);

			// Debug settings
			physicsSettings.CaptureOnPlay = physicsNode["CaptureOnPlay"].as<bool>(physicsSettings.CaptureOnPlay);
			physicsSettings.CaptureMethod = static_cast<PhysicsDebugType>(physicsNode["CaptureMethod"].as<i32>(static_cast<i32>(physicsSettings.CaptureMethod)));

			// Advanced Jolt settings
			physicsSettings.Baumgarte = physicsNode["Baumgarte"].as<f32>(physicsSettings.Baumgarte);
			physicsSettings.SpeculativeContactDistance = physicsNode["SpeculativeContactDistance"].as<f32>(physicsSettings.SpeculativeContactDistance);
			physicsSettings.PenetrationSlop = physicsNode["PenetrationSlop"].as<f32>(physicsSettings.PenetrationSlop);
			physicsSettings.LinearCastThreshold = physicsNode["LinearCastThreshold"].as<f32>(physicsSettings.LinearCastThreshold);
			physicsSettings.MinVelocityForRestitution = physicsNode["MinVelocityForRestitution"].as<f32>(physicsSettings.MinVelocityForRestitution);
			physicsSettings.TimeBeforeSleep = physicsNode["TimeBeforeSleep"].as<f32>(physicsSettings.TimeBeforeSleep);
			physicsSettings.PointVelocitySleepThreshold = physicsNode["PointVelocitySleepThreshold"].as<f32>(physicsSettings.PointVelocitySleepThreshold);

			// Boolean settings
			physicsSettings.DeterministicSimulation = physicsNode["DeterministicSimulation"].as<bool>(physicsSettings.DeterministicSimulation);
			physicsSettings.ConstraintWarmStart = physicsNode["ConstraintWarmStart"].as<bool>(physicsSettings.ConstraintWarmStart);
			physicsSettings.UseBodyPairContactCache = physicsNode["UseBodyPairContactCache"].as<bool>(physicsSettings.UseBodyPairContactCache);
			physicsSettings.UseManifoldReduction = physicsNode["UseManifoldReduction"].as<bool>(physicsSettings.UseManifoldReduction);
			physicsSettings.UseLargeIslandSplitter = physicsNode["UseLargeIslandSplitter"].as<bool>(physicsSettings.UseLargeIslandSplitter);
			physicsSettings.AllowSleeping = physicsNode["AllowSleeping"].as<bool>(physicsSettings.AllowSleeping);

			// Physics layers deserialization
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
					layerInfo.CollidesWithSelf = layer["CollidesWithSelf"].as<bool>(true);

					auto collidesWith = layer["CollidesWith"];
					if (collidesWith)
					{
						for (auto collisionLayer : collidesWith)
						{
							const std::string otherLayerName = collisionLayer["Name"].as<std::string>();
							const auto& otherLayer = PhysicsLayerManager::GetLayer(otherLayerName);
							if (otherLayer.IsValid())
							{
								PhysicsLayerManager::SetLayerCollision(layerInfo.LayerID, otherLayer.LayerID, true);
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
