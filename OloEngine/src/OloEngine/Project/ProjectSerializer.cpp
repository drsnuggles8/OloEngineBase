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

// Physics settings validation helpers
namespace {
	// Validation ranges for physics settings
	constexpr f32 MIN_FIXED_TIMESTEP = 1.0f / 300.0f;  // 300 Hz max frequency
	constexpr f32 MAX_FIXED_TIMESTEP = 1.0f / 10.0f;   // 10 Hz min frequency
	constexpr f32 MAX_GRAVITY_MAGNITUDE = 100.0f;      // Reasonable max gravity magnitude
	constexpr u32 MIN_SOLVER_ITERATIONS = 1u;
	constexpr u32 MAX_SOLVER_ITERATIONS = 50u;
	constexpr u32 MIN_MAX_BODIES = 100u;
	constexpr u32 MAX_MAX_BODIES = 1000000u;
	constexpr u32 MIN_MAX_PAIRS = 100u;
	constexpr u32 MAX_MAX_PAIRS = 1000000u;
	constexpr u32 MIN_MAX_CONTACTS = 100u;
	constexpr u32 MAX_MAX_CONTACTS = 100000u;
	constexpr f32 MIN_BAUMGARTE = 0.01f;
	constexpr f32 MAX_BAUMGARTE = 1.0f;
	constexpr f32 MIN_CONTACT_DISTANCE = 0.001f;
	constexpr f32 MAX_CONTACT_DISTANCE = 1.0f;
	constexpr f32 MIN_SLOP = 0.001f;
	constexpr f32 MAX_SLOP = 0.5f;
	constexpr f32 MIN_CAST_THRESHOLD = 0.1f;
	constexpr f32 MAX_CAST_THRESHOLD = 10.0f;
	constexpr f32 MIN_VELOCITY_RESTITUTION = 0.0f;
	constexpr f32 MAX_VELOCITY_RESTITUTION = 100.0f;
	constexpr f32 MIN_TIME_BEFORE_SLEEP = 0.0f;
	constexpr f32 MAX_TIME_BEFORE_SLEEP = 60.0f;
	constexpr f32 MIN_VELOCITY_SLEEP_THRESHOLD = 0.001f;
	constexpr f32 MAX_VELOCITY_SLEEP_THRESHOLD = 10.0f;

	/// Validates and clamps a floating-point physics setting to safe ranges
	/// \param value The value to validate
	/// \param minVal Minimum allowed value
	/// \param maxVal Maximum allowed value
	/// \param defaultVal Default value to use if clamping is needed
	/// \param settingName Name of the setting for logging
	/// \return Validated and potentially clamped value
	f32 ValidateAndClampFloat(f32 value, f32 minVal, f32 maxVal, f32 defaultVal, const char* settingName)
	{
		if (std::isnan(value) || std::isinf(value))
		{
			OLO_CORE_WARN("Physics validation: {} is NaN/Inf, using default value {}", settingName, defaultVal);
			return defaultVal;
		}
		
		if (value < minVal)
		{
			OLO_CORE_WARN("Physics validation: {} ({}) below minimum ({}), clamping to minimum", settingName, value, minVal);
			return minVal;
		}
		
		if (value > maxVal)
		{
			OLO_CORE_WARN("Physics validation: {} ({}) exceeds maximum ({}), clamping to maximum", settingName, value, maxVal);
			return maxVal;
		}
		
		return value;
	}

	/// Validates and clamps an unsigned integer physics setting to safe ranges
	/// \param value The value to validate
	/// \param minVal Minimum allowed value
	/// \param maxVal Maximum allowed value
	/// \param defaultVal Default value to use if clamping is needed
	/// \param settingName Name of the setting for logging
	/// \return Validated and potentially clamped value
	u32 ValidateAndClampUInt(u32 value, u32 minVal, u32 maxVal, u32 defaultVal, const char* settingName)
	{
		if (value < minVal)
		{
			OLO_CORE_WARN("Physics validation: {} ({}) below minimum ({}), clamping to minimum", settingName, value, minVal);
			return minVal;
		}
		
		if (value > maxVal)
		{
			OLO_CORE_WARN("Physics validation: {} ({}) exceeds maximum ({}), clamping to maximum", settingName, value, maxVal);
			return maxVal;
		}
		
		return value;
	}

	/// Validates gravity vector magnitude and clamps if necessary
	/// \param gravity The gravity vector to validate
	/// \param settingName Name of the setting for logging
	/// \return Validated gravity vector
	glm::vec3 ValidateGravity(const glm::vec3& gravity, const char* settingName)
	{
		f32 magnitude = glm::length(gravity);
		
		if (std::isnan(magnitude) || std::isinf(magnitude))
		{
			OLO_CORE_WARN("Physics validation: {} has NaN/Inf components, using default (0, -9.81, 0)", settingName);
			return glm::vec3(0.0f, -9.81f, 0.0f);
		}
		
		if (magnitude > MAX_GRAVITY_MAGNITUDE)
		{
			glm::vec3 normalized = magnitude > 0.0f ? gravity / magnitude : glm::vec3(0.0f, -1.0f, 0.0f);
			glm::vec3 clamped = normalized * MAX_GRAVITY_MAGNITUDE;
			OLO_CORE_WARN("Physics validation: {} magnitude ({}) exceeds maximum ({}), clamping to maximum", 
				settingName, magnitude, MAX_GRAVITY_MAGNITUDE);
			return clamped;
		}
		
		return gravity;
	}

	/// Validates an entire PhysicsSettings struct and returns a validated copy
	/// \param settings The settings to validate
	/// \return Validated copy of the settings
	OloEngine::PhysicsSettings ValidatePhysicsSettings(const OloEngine::PhysicsSettings& settings)
	{
		OloEngine::PhysicsSettings validated = settings;
		
		// Validate core simulation settings
		validated.m_FixedTimestep = ValidateAndClampFloat(settings.m_FixedTimestep, 
			MIN_FIXED_TIMESTEP, MAX_FIXED_TIMESTEP, 1.0f / 60.0f, "FixedTimestep");
		validated.m_Gravity = ValidateGravity(settings.m_Gravity, "Gravity");
		
		// Validate solver settings
		validated.m_PositionSolverIterations = ValidateAndClampUInt(settings.m_PositionSolverIterations,
			MIN_SOLVER_ITERATIONS, MAX_SOLVER_ITERATIONS, 2u, "PositionSolverIterations");
		validated.m_VelocitySolverIterations = ValidateAndClampUInt(settings.m_VelocitySolverIterations,
			MIN_SOLVER_ITERATIONS, MAX_SOLVER_ITERATIONS, 10u, "VelocitySolverIterations");
		
		// Validate system limits
		validated.m_MaxBodies = ValidateAndClampUInt(settings.m_MaxBodies,
			MIN_MAX_BODIES, MAX_MAX_BODIES, 65536u, "MaxBodies");
		validated.m_MaxBodyPairs = ValidateAndClampUInt(settings.m_MaxBodyPairs,
			MIN_MAX_PAIRS, MAX_MAX_PAIRS, 65536u, "MaxBodyPairs");
		validated.m_MaxContactConstraints = ValidateAndClampUInt(settings.m_MaxContactConstraints,
			MIN_MAX_CONTACTS, MAX_MAX_CONTACTS, 10240u, "MaxContactConstraints");
		
		// Validate advanced Jolt settings
		validated.m_Baumgarte = ValidateAndClampFloat(settings.m_Baumgarte,
			MIN_BAUMGARTE, MAX_BAUMGARTE, 0.2f, "Baumgarte");
		validated.m_SpeculativeContactDistance = ValidateAndClampFloat(settings.m_SpeculativeContactDistance,
			MIN_CONTACT_DISTANCE, MAX_CONTACT_DISTANCE, 0.02f, "SpeculativeContactDistance");
		validated.m_PenetrationSlop = ValidateAndClampFloat(settings.m_PenetrationSlop,
			MIN_SLOP, MAX_SLOP, 0.05f, "PenetrationSlop");
		validated.m_LinearCastThreshold = ValidateAndClampFloat(settings.m_LinearCastThreshold,
			MIN_CAST_THRESHOLD, MAX_CAST_THRESHOLD, 0.75f, "LinearCastThreshold");
		validated.m_MinVelocityForRestitution = ValidateAndClampFloat(settings.m_MinVelocityForRestitution,
			MIN_VELOCITY_RESTITUTION, MAX_VELOCITY_RESTITUTION, 1.0f, "MinVelocityForRestitution");
		validated.m_TimeBeforeSleep = ValidateAndClampFloat(settings.m_TimeBeforeSleep,
			MIN_TIME_BEFORE_SLEEP, MAX_TIME_BEFORE_SLEEP, 0.5f, "TimeBeforeSleep");
		validated.m_PointVelocitySleepThreshold = ValidateAndClampFloat(settings.m_PointVelocitySleepThreshold,
			MIN_VELOCITY_SLEEP_THRESHOLD, MAX_VELOCITY_SLEEP_THRESHOLD, 0.03f, "PointVelocitySleepThreshold");
		
		// Boolean settings don't need validation (any bool value is valid)
		
		return validated;
	}
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

			// Validate physics settings before serialization
			const auto& rawPhysicsSettings = Physics3DSystem::GetSettings();
			const auto& physicsSettings = ValidatePhysicsSettings(rawPhysicsSettings);

			// Core simulation settings with range documentation
			out << YAML::Comment("Simulation timestep in seconds (range: 0.0033-0.1, default: 0.0167 for 60Hz)");
			out << YAML::Key << "FixedTimestep" << YAML::Value << physicsSettings.m_FixedTimestep;
			
			out << YAML::Comment("Gravity vector in m/s² (magnitude limit: 100.0, default: [0, -9.81, 0])");
			out << YAML::Key << "Gravity" << YAML::Value << physicsSettings.m_Gravity;
			
			// Solver iteration settings with range documentation
			out << YAML::Comment("Position solver iterations per step (range: 1-50, default: 2)");
			out << YAML::Key << "PositionSolverIterations" << YAML::Value << physicsSettings.m_PositionSolverIterations;
			
			out << YAML::Comment("Velocity solver iterations per step (range: 1-50, default: 10)");
			out << YAML::Key << "VelocitySolverIterations" << YAML::Value << physicsSettings.m_VelocitySolverIterations;
			
			// System limits with range documentation
			out << YAML::Comment("Maximum physics bodies in simulation (range: 100-1000000, default: 65536)");
			out << YAML::Key << "MaxBodies" << YAML::Value << physicsSettings.m_MaxBodies;
			
			out << YAML::Comment("Maximum body pairs for collision detection (range: 100-1000000, default: 65536)");
			out << YAML::Key << "MaxBodyPairs" << YAML::Value << physicsSettings.m_MaxBodyPairs;
			
			out << YAML::Comment("Maximum contact constraints (range: 100-100000, default: 10240)");
			out << YAML::Key << "MaxContactConstraints" << YAML::Value << physicsSettings.m_MaxContactConstraints;

			// Debug and capture settings (booleans don't need validation)
			out << YAML::Comment("Enable physics capture during play mode (off by default for production)");
			out << YAML::Key << "CaptureOnPlay" << YAML::Value << physicsSettings.m_CaptureOnPlay;
			
			out << YAML::Comment("Physics capture method (0: DebugToFile, 1: LiveDebug)");
			out << YAML::Key << "CaptureMethod" << YAML::Value << static_cast<i32>(physicsSettings.m_CaptureMethod);

			// Advanced Jolt settings with range documentation
			out << YAML::Comment("Baumgarte stabilization factor (range: 0.01-1.0, default: 0.2)");
			out << YAML::Key << "Baumgarte" << YAML::Value << physicsSettings.m_Baumgarte;
			
			out << YAML::Comment("Speculative contact distance in meters (range: 0.001-1.0, default: 0.02)");
			out << YAML::Key << "SpeculativeContactDistance" << YAML::Value << physicsSettings.m_SpeculativeContactDistance;
			
			out << YAML::Comment("Penetration slop tolerance in meters (range: 0.001-0.5, default: 0.05)");
			out << YAML::Key << "PenetrationSlop" << YAML::Value << physicsSettings.m_PenetrationSlop;
			
			out << YAML::Comment("Linear cast threshold factor (range: 0.1-10.0, default: 0.75)");
			out << YAML::Key << "LinearCastThreshold" << YAML::Value << physicsSettings.m_LinearCastThreshold;
			
			out << YAML::Comment("Minimum velocity for restitution in m/s (range: 0.0-100.0, default: 1.0)");
			out << YAML::Key << "MinVelocityForRestitution" << YAML::Value << physicsSettings.m_MinVelocityForRestitution;
			
			out << YAML::Comment("Time before bodies sleep in seconds (range: 0.0-60.0, default: 0.5)");
			out << YAML::Key << "TimeBeforeSleep" << YAML::Value << physicsSettings.m_TimeBeforeSleep;
			
			out << YAML::Comment("Point velocity sleep threshold in m/s (range: 0.001-10.0, default: 0.03)");
			out << YAML::Key << "PointVelocitySleepThreshold" << YAML::Value << physicsSettings.m_PointVelocitySleepThreshold;

			// Boolean optimization settings (no validation needed)
			out << YAML::Comment("Enable deterministic simulation for reproducible results");
			out << YAML::Key << "DeterministicSimulation" << YAML::Value << physicsSettings.m_DeterministicSimulation;
			
			out << YAML::Comment("Enable constraint warm starting for better convergence");
			out << YAML::Key << "ConstraintWarmStart" << YAML::Value << physicsSettings.m_ConstraintWarmStart;
			
			out << YAML::Comment("Use body pair contact cache for performance");
			out << YAML::Key << "UseBodyPairContactCache" << YAML::Value << physicsSettings.m_UseBodyPairContactCache;
			
			out << YAML::Comment("Use manifold reduction for contact optimization");
			out << YAML::Key << "UseManifoldReduction" << YAML::Value << physicsSettings.m_UseManifoldReduction;
			
			out << YAML::Comment("Use large island splitter for complex scenes");
			out << YAML::Key << "UseLargeIslandSplitter" << YAML::Value << physicsSettings.m_UseLargeIslandSplitter;
			
			out << YAML::Comment("Allow bodies to sleep when inactive");
			out << YAML::Key << "AllowSleeping" << YAML::Value << physicsSettings.m_AllowSleeping;
			// Physics layers serialization
			if (PhysicsLayerManager::GetLayerCount() > 1)
			{
				out << YAML::Key << "Layers";
				out << YAML::Value << YAML::BeginSeq;
				
				// Reusable vector to avoid allocations in the loop
				std::vector<PhysicsLayer> collidingLayers;
				
				for (const auto& layer : PhysicsLayerManager::GetLayers())
				{
					out << YAML::BeginMap;
					out << YAML::Key << "LayerID" << YAML::Value << layer.m_LayerID;
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
		bool physicsValid = true;
		if (physicsNode)
		{
			// Track applied physics fields for validation
			u32 appliedPhysicsFields = 0;
			const u32 expectedPhysicsFields = 22; // Total number of physics settings fields
			                                      // (4 basic + 3 limits + 2 debug + 7 advanced + 6 boolean)

			auto& physicsSettings = Physics3DSystem::GetSettings();

			// Basic simulation settings
			if (physicsNode["FixedTimestep"].IsDefined()) {
				physicsSettings.m_FixedTimestep = physicsNode["FixedTimestep"].as<f32>(physicsSettings.m_FixedTimestep);
				appliedPhysicsFields++;
			}
			if (physicsNode["Gravity"].IsDefined()) {
				physicsSettings.m_Gravity = physicsNode["Gravity"].as<glm::vec3>(physicsSettings.m_Gravity);
				appliedPhysicsFields++;
			}
			if (physicsNode["PositionSolverIterations"].IsDefined()) {
				physicsSettings.m_PositionSolverIterations = physicsNode["PositionSolverIterations"].as<u32>(physicsSettings.m_PositionSolverIterations);
				appliedPhysicsFields++;
			}
			if (physicsNode["VelocitySolverIterations"].IsDefined()) {
				physicsSettings.m_VelocitySolverIterations = physicsNode["VelocitySolverIterations"].as<u32>(physicsSettings.m_VelocitySolverIterations);
				appliedPhysicsFields++;
			}
			if (physicsNode["MaxBodies"].IsDefined()) {
				physicsSettings.m_MaxBodies = physicsNode["MaxBodies"].as<u32>(physicsSettings.m_MaxBodies);
				appliedPhysicsFields++;
			}
			if (physicsNode["MaxBodyPairs"].IsDefined()) {
				physicsSettings.m_MaxBodyPairs = physicsNode["MaxBodyPairs"].as<u32>(physicsSettings.m_MaxBodyPairs);
				appliedPhysicsFields++;
			}
			if (physicsNode["MaxContactConstraints"].IsDefined()) {
				physicsSettings.m_MaxContactConstraints = physicsNode["MaxContactConstraints"].as<u32>(physicsSettings.m_MaxContactConstraints);
				appliedPhysicsFields++;
			}

			// Debug settings
			if (physicsNode["CaptureOnPlay"].IsDefined()) {
				physicsSettings.m_CaptureOnPlay = physicsNode["CaptureOnPlay"].as<bool>(physicsSettings.m_CaptureOnPlay);
				appliedPhysicsFields++;
			}
			if (physicsNode["CaptureMethod"].IsDefined()) {
				physicsSettings.m_CaptureMethod = static_cast<PhysicsDebugType>(physicsNode["CaptureMethod"].as<i32>(static_cast<i32>(physicsSettings.m_CaptureMethod)));
				appliedPhysicsFields++;
			}

			// Advanced Jolt settings
			if (physicsNode["Baumgarte"].IsDefined()) {
				physicsSettings.m_Baumgarte = physicsNode["Baumgarte"].as<f32>(physicsSettings.m_Baumgarte);
				appliedPhysicsFields++;
			}
			if (physicsNode["SpeculativeContactDistance"].IsDefined()) {
				physicsSettings.m_SpeculativeContactDistance = physicsNode["SpeculativeContactDistance"].as<f32>(physicsSettings.m_SpeculativeContactDistance);
				appliedPhysicsFields++;
			}
			if (physicsNode["PenetrationSlop"].IsDefined()) {
				physicsSettings.m_PenetrationSlop = physicsNode["PenetrationSlop"].as<f32>(physicsSettings.m_PenetrationSlop);
				appliedPhysicsFields++;
			}
			if (physicsNode["LinearCastThreshold"].IsDefined()) {
				physicsSettings.m_LinearCastThreshold = physicsNode["LinearCastThreshold"].as<f32>(physicsSettings.m_LinearCastThreshold);
				appliedPhysicsFields++;
			}
			if (physicsNode["MinVelocityForRestitution"].IsDefined()) {
				physicsSettings.m_MinVelocityForRestitution = physicsNode["MinVelocityForRestitution"].as<f32>(physicsSettings.m_MinVelocityForRestitution);
				appliedPhysicsFields++;
			}
			if (physicsNode["TimeBeforeSleep"].IsDefined()) {
				physicsSettings.m_TimeBeforeSleep = physicsNode["TimeBeforeSleep"].as<f32>(physicsSettings.m_TimeBeforeSleep);
				appliedPhysicsFields++;
			}
			if (physicsNode["PointVelocitySleepThreshold"].IsDefined()) {
				physicsSettings.m_PointVelocitySleepThreshold = physicsNode["PointVelocitySleepThreshold"].as<f32>(physicsSettings.m_PointVelocitySleepThreshold);
				appliedPhysicsFields++;
			}

			// Boolean settings
			if (physicsNode["DeterministicSimulation"].IsDefined()) {
				physicsSettings.m_DeterministicSimulation = physicsNode["DeterministicSimulation"].as<bool>(physicsSettings.m_DeterministicSimulation);
				appliedPhysicsFields++;
			}
			if (physicsNode["ConstraintWarmStart"].IsDefined()) {
				physicsSettings.m_ConstraintWarmStart = physicsNode["ConstraintWarmStart"].as<bool>(physicsSettings.m_ConstraintWarmStart);
				appliedPhysicsFields++;
			}
			if (physicsNode["UseBodyPairContactCache"].IsDefined()) {
				physicsSettings.m_UseBodyPairContactCache = physicsNode["UseBodyPairContactCache"].as<bool>(physicsSettings.m_UseBodyPairContactCache);
				appliedPhysicsFields++;
			}
			if (physicsNode["UseManifoldReduction"].IsDefined()) {
				physicsSettings.m_UseManifoldReduction = physicsNode["UseManifoldReduction"].as<bool>(physicsSettings.m_UseManifoldReduction);
				appliedPhysicsFields++;
			}
			if (physicsNode["UseLargeIslandSplitter"].IsDefined()) {
				physicsSettings.m_UseLargeIslandSplitter = physicsNode["UseLargeIslandSplitter"].as<bool>(physicsSettings.m_UseLargeIslandSplitter);
				appliedPhysicsFields++;
			}
			if (physicsNode["AllowSleeping"].IsDefined()) {
				physicsSettings.m_AllowSleeping = physicsNode["AllowSleeping"].as<bool>(physicsSettings.m_AllowSleeping);
				appliedPhysicsFields++;
			}

			// Physics layers deserialization
			auto physicsLayers = physicsNode["Layers"];
			if (physicsLayers)
			{
				// Clear existing layers
				PhysicsLayerManager::ClearLayers();
				
				// Ensure the default layer exists after clearing
				// Note: Default layer ID is not guaranteed to be any specific value
				u32 defaultLayerId = PhysicsLayerManager::AddLayer("Default", true);
				if (defaultLayerId == INVALID_LAYER_ID)
				{
					OLO_CORE_ERROR("Physics deserialization: Failed to recreate default layer");
					physicsValid = false;
				}
				
				std::vector<std::pair<u32, YAML::Node>> layersToProcess; // Store layers for collision setup
				
				for (auto layer : physicsLayers)
				{
					const std::string layerName = layer["Name"].as<std::string>();
					
					// Skip if this is the default layer (already created)
					if (layerName == "Default")
					{
						// Still store it for collision processing
						layersToProcess.emplace_back(defaultLayerId, layer);
						continue;
					}
					
					u32 layerId = PhysicsLayerManager::AddLayer(layerName, false);
					if (layerId == INVALID_LAYER_ID)
					{
						OLO_CORE_ERROR("Physics deserialization: Failed to add layer '{}' - may have hit layer limit", layerName);
						continue; // Skip this layer but continue processing others
					}
					
					// Store successful layer for collision setup
					layersToProcess.emplace_back(layerId, layer);
				}
				
				// Process collision settings for all successfully created layers
				for (const auto& [layerId, layerNode] : layersToProcess)
				{
					PhysicsLayer layerInfo = PhysicsLayerManager::GetLayer(layerId);
					if (!layerInfo.IsValid())
					{
						OLO_CORE_WARN("Physics deserialization: Skipping collision setup for invalid layer ID {}", layerId);
						continue;
					}
					
					PhysicsLayerManager::SetLayerSelfCollision(layerId, layerNode["CollidesWithSelf"].as<bool>(true));

					auto collidesWith = layerNode["CollidesWith"];
					if (collidesWith)
					{
						for (auto collisionLayer : collidesWith)
						{
							const std::string otherLayerName = collisionLayer["Name"].as<std::string>();
							const auto otherLayer = PhysicsLayerManager::GetLayer(otherLayerName);
							if (otherLayer.IsValid())
							{
								PhysicsLayerManager::SetLayerCollision(layerInfo.m_LayerID, otherLayer.m_LayerID, true);
							}
							else
							{
								OLO_CORE_WARN("Physics deserialization: Layer '{}' references non-existent collision layer '{}'", 
									layerInfo.m_Name, otherLayerName);
							}
						}
					}
				}
			}

			// Log physics deserialization summary
			if (appliedPhysicsFields == 0)
			{
				OLO_CORE_WARN("Physics settings: No valid fields found in project file");
				physicsValid = false;
			}
			else if (appliedPhysicsFields < expectedPhysicsFields)
			{
				OLO_CORE_WARN("Physics settings: Applied {}/{} fields - some settings may use defaults", appliedPhysicsFields, expectedPhysicsFields);
			}
			else
			{
				OLO_CORE_INFO("Physics settings: Successfully loaded {}/{} fields", appliedPhysicsFields, expectedPhysicsFields);
			}

			// Validate loaded physics settings before applying
			const auto validatedSettings = ValidatePhysicsSettings(physicsSettings);
			Physics3DSystem::SetSettings(validatedSettings);
		}
		
		// Combine physics validity with overall project validity
		allValid = allValid && physicsValid;
		
		return allValid;
	}
}
