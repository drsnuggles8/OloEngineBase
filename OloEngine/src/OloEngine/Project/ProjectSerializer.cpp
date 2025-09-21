#include "OloEnginePCH.h"

#include "OloEngine/Project/ProjectSerializer.h"
#include "OloEngine/Physics3D/Physics3DSystem.h"
#include "OloEngine/Physics3D/PhysicsLayer.h"

#include <fstream>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <type_traits>

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

			// Read each component into temporary variables for validation
			float x = node[0].as<float>();
			float y = node[1].as<float>();
			float z = node[2].as<float>();
			
			// Validate all components are finite (not NaN or infinity)
			if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
				return false;
			}
			
			// Only assign to output after all validation passes
			rhs.x = x;
			rhs.y = y;
			rhs.z = z;
			return true;
		}
	};
}

// Physics settings validation helpers
namespace {
	// Validation ranges for physics settings
	constexpr f32 s_MinFixedTimestep = 1.0f / 300.0f;  // 300 Hz max frequency
	constexpr f32 s_MaxFixedTimestep = 1.0f / 10.0f;   // 10 Hz min frequency
	constexpr f32 s_MaxGravityMagnitude = 100.0f;      // Reasonable max gravity magnitude
	constexpr u32 s_MinSolverIterations = 1u;
	constexpr u32 s_MaxSolverIterations = 50u;
	constexpr u32 s_MinMaxBodies = 100u;
	constexpr u32 s_MaxMaxBodies = 1000000u;
	constexpr u32 s_MinMaxPairs = 100u;
	constexpr u32 s_MaxMaxPairs = 1000000u;
	constexpr u32 s_MinMaxContacts = 100u;
	constexpr u32 s_MaxMaxContacts = 100000u;
	constexpr f32 s_MinBaumgarte = 0.01f;
	constexpr f32 s_MaxBaumgarte = 1.0f;
	constexpr f32 s_MinContactDistance = 0.001f;
	constexpr f32 s_MaxContactDistance = 1.0f;
	constexpr f32 s_MinSlop = 0.001f;
	constexpr f32 s_MaxSlop = 0.5f;
	constexpr f32 s_MinCastThreshold = 0.1f;
	constexpr f32 s_MaxCastThreshold = 10.0f;
	constexpr f32 s_MinVelocityRestitution = 0.0f;
	constexpr f32 s_MaxVelocityRestitution = 100.0f;
	constexpr f32 s_MinTimeBeforeSleep = 0.0f;
	constexpr f32 s_MaxTimeBeforeSleep = 60.0f;
	constexpr f32 s_MinVelocitySleepThreshold = 0.001f;
	constexpr f32 s_MaxVelocitySleepThreshold = 10.0f;

	/// Case-insensitive string comparison helper
	/// \param lhs First string to compare
	/// \param rhs Second string to compare
	/// \return True if strings are equal (case-insensitive), false otherwise
	bool iequals(const std::string& lhs, const std::string& rhs)
	{
		if (lhs.length() != rhs.length())
			return false;
		
		return std::equal(lhs.begin(), lhs.end(), rhs.begin(),
			[](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			});
	}

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
		
		if (magnitude > s_MaxGravityMagnitude)
		{
			glm::vec3 normalized = magnitude > 0.0f ? gravity / magnitude : glm::vec3(0.0f, -1.0f, 0.0f);
			glm::vec3 clamped = normalized * s_MaxGravityMagnitude;
			OLO_CORE_WARN("Physics validation: {} magnitude ({}) exceeds maximum ({}), clamping to maximum", 
				settingName, magnitude, s_MaxGravityMagnitude);
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
			s_MinFixedTimestep, s_MaxFixedTimestep, 1.0f / 60.0f, "FixedTimestep");
		validated.m_Gravity = ValidateGravity(settings.m_Gravity, "Gravity");
		
		// Validate solver settings
		validated.m_PositionSolverIterations = ValidateAndClampUInt(settings.m_PositionSolverIterations,
			s_MinSolverIterations, s_MaxSolverIterations, 2u, "PositionSolverIterations");
		validated.m_VelocitySolverIterations = ValidateAndClampUInt(settings.m_VelocitySolverIterations,
			s_MinSolverIterations, s_MaxSolverIterations, 10u, "VelocitySolverIterations");
		
		// Validate system limits
		validated.m_MaxBodies = ValidateAndClampUInt(settings.m_MaxBodies,
			s_MinMaxBodies, s_MaxMaxBodies, 65536u, "MaxBodies");
		validated.m_MaxBodyPairs = ValidateAndClampUInt(settings.m_MaxBodyPairs,
			s_MinMaxPairs, s_MaxMaxPairs, 65536u, "MaxBodyPairs");
		validated.m_MaxContactConstraints = ValidateAndClampUInt(settings.m_MaxContactConstraints,
			s_MinMaxContacts, s_MaxMaxContacts, 10240u, "MaxContactConstraints");
		
		// Validate advanced Jolt settings
		validated.m_Baumgarte = ValidateAndClampFloat(settings.m_Baumgarte,
			s_MinBaumgarte, s_MaxBaumgarte, 0.2f, "Baumgarte");
		validated.m_SpeculativeContactDistance = ValidateAndClampFloat(settings.m_SpeculativeContactDistance,
			s_MinContactDistance, s_MaxContactDistance, 0.02f, "SpeculativeContactDistance");
		validated.m_PenetrationSlop = ValidateAndClampFloat(settings.m_PenetrationSlop,
			s_MinSlop, s_MaxSlop, 0.05f, "PenetrationSlop");
		validated.m_LinearCastThreshold = ValidateAndClampFloat(settings.m_LinearCastThreshold,
			s_MinCastThreshold, s_MaxCastThreshold, 0.75f, "LinearCastThreshold");
		validated.m_MinVelocityForRestitution = ValidateAndClampFloat(settings.m_MinVelocityForRestitution,
			s_MinVelocityRestitution, s_MaxVelocityRestitution, 1.0f, "MinVelocityForRestitution");
		validated.m_TimeBeforeSleep = ValidateAndClampFloat(settings.m_TimeBeforeSleep,
			s_MinTimeBeforeSleep, s_MaxTimeBeforeSleep, 0.5f, "TimeBeforeSleep");
		validated.m_PointVelocitySleepThreshold = ValidateAndClampFloat(settings.m_PointVelocitySleepThreshold,
			s_MinVelocitySleepThreshold, s_MaxVelocitySleepThreshold, 0.03f, "PointVelocitySleepThreshold");
		
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
			if (PhysicsLayerManager::GetLayerCount() > 0)
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
			// Physics field counts organized by category for maintainability
			constexpr u32 basicSimulationFields = 4;     // FixedTimestep, Gravity, PositionSolverIterations, VelocitySolverIterations
			constexpr u32 systemLimitFields = 3;         // MaxBodies, MaxBodyPairs, MaxContactConstraints
			constexpr u32 debugFields = 2;               // CaptureOnPlay, CaptureMethod
			constexpr u32 advancedJoltFields = 7;        // Baumgarte through PointVelocitySleepThreshold
			constexpr u32 booleanOptimizationFields = 6; // DeterministicSimulation through AllowSleeping
			
			// Compute total expected fields automatically
			constexpr u32 expectedPhysicsFields = basicSimulationFields + systemLimitFields + debugFields + 
			                                      advancedJoltFields + booleanOptimizationFields;
			
			// Track applied physics fields for validation
			u32 appliedPhysicsFields = 0;

			auto& physicsSettings = Physics3DSystem::GetSettings();

			// Helper lambda to reduce duplication in physics field deserialization
			auto deserializeField = [&](const char* fieldName, auto& targetField) {
				if (physicsNode[fieldName].IsDefined()) {
					using FieldType = std::decay_t<decltype(targetField)>;
					targetField = physicsNode[fieldName].as<FieldType>(targetField);
					appliedPhysicsFields++;
				}
			};

			// Basic simulation settings
			deserializeField("FixedTimestep", physicsSettings.m_FixedTimestep);
			deserializeField("Gravity", physicsSettings.m_Gravity);
			deserializeField("PositionSolverIterations", physicsSettings.m_PositionSolverIterations);
			deserializeField("VelocitySolverIterations", physicsSettings.m_VelocitySolverIterations);
			deserializeField("MaxBodies", physicsSettings.m_MaxBodies);
			deserializeField("MaxBodyPairs", physicsSettings.m_MaxBodyPairs);
			deserializeField("MaxContactConstraints", physicsSettings.m_MaxContactConstraints);

			// Debug settings
			deserializeField("CaptureOnPlay", physicsSettings.m_CaptureOnPlay);
			// CaptureMethod requires special handling due to enum cast
			if (physicsNode["CaptureMethod"].IsDefined()) {
				physicsSettings.m_CaptureMethod = static_cast<PhysicsDebugType>(physicsNode["CaptureMethod"].as<i32>(static_cast<i32>(physicsSettings.m_CaptureMethod)));
				appliedPhysicsFields++;
			}

			// Advanced Jolt settings
			deserializeField("Baumgarte", physicsSettings.m_Baumgarte);
			deserializeField("SpeculativeContactDistance", physicsSettings.m_SpeculativeContactDistance);
			deserializeField("PenetrationSlop", physicsSettings.m_PenetrationSlop);
			deserializeField("LinearCastThreshold", physicsSettings.m_LinearCastThreshold);
			deserializeField("MinVelocityForRestitution", physicsSettings.m_MinVelocityForRestitution);
			deserializeField("TimeBeforeSleep", physicsSettings.m_TimeBeforeSleep);
			deserializeField("PointVelocitySleepThreshold", physicsSettings.m_PointVelocitySleepThreshold);

			// Boolean settings
			deserializeField("DeterministicSimulation", physicsSettings.m_DeterministicSimulation);
			deserializeField("ConstraintWarmStart", physicsSettings.m_ConstraintWarmStart);
			deserializeField("UseBodyPairContactCache", physicsSettings.m_UseBodyPairContactCache);
			deserializeField("UseManifoldReduction", physicsSettings.m_UseManifoldReduction);
			deserializeField("UseLargeIslandSplitter", physicsSettings.m_UseLargeIslandSplitter);
			deserializeField("AllowSleeping", physicsSettings.m_AllowSleeping);

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
					
					// Skip if this is the default layer (already created) - case-insensitive comparison
					if (iequals(layerName, "Default"))
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
		
		// Handle physics deserialization status separately from project load success
		if (!physicsValid)
		{
			OLO_CORE_ERROR("Physics settings deserialization failed - initializing physics with safe defaults");
			
			// Initialize physics with safe default settings to ensure system stability
			const auto defaultSettings = PhysicsSettings::GetDefaults();
			Physics3DSystem::SetSettings(defaultSettings);
			
			// Note: physicsValid failure does not prevent project load - we gracefully degrade
			// Callers can check project validity vs physics degradation separately if needed
		}
		
		return allValid;
	}
}
