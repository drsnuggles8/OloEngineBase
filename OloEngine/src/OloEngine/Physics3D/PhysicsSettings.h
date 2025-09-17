#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>

namespace OloEngine {

	enum class PhysicsDebugType
	{
		DebugToFile = 0,
		LiveDebug
	};

	struct PhysicsSettings
	{
		// Simulation settings
		f32 FixedTimestep = 1.0f / 60.0f;
		glm::vec3 Gravity = { 0.0f, -9.81f, 0.0f };
		
		// Solver settings
		u32 PositionSolverIterations = 2;
		u32 VelocitySolverIterations = 10;
		
		// System limits
		u32 MaxBodies = 65536;
		u32 MaxBodyPairs = 65536;
		u32 MaxContactConstraints = 10240;
		
		// Debug and capture settings
		bool CaptureOnPlay = true;
		PhysicsDebugType CaptureMethod = PhysicsDebugType::DebugToFile;
		
		// Advanced Jolt-specific settings
		f32 Baumgarte = 0.2f;
		f32 SpeculativeContactDistance = 0.02f;
		f32 PenetrationSlop = 0.05f;
		f32 LinearCastThreshold = 0.75f;
		f32 MinVelocityForRestitution = 1.0f;
		f32 TimeBeforeSleep = 0.5f;
		f32 PointVelocitySleepThreshold = 0.03f;
		
		// Boolean physics options
		bool DeterministicSimulation = true;
		bool ConstraintWarmStart = true;
		bool UseBodyPairContactCache = true;
		bool UseManifoldReduction = true;
		bool UseLargeIslandSplitter = true;
		bool AllowSleeping = true;
		
		// Default values method for easy reset
		static PhysicsSettings GetDefaults()
		{
			return PhysicsSettings{};
		}
	};

}