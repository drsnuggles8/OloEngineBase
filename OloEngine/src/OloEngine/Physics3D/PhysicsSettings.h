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
		f32 m_FixedTimestep = 1.0f / 60.0f;
		glm::vec3 m_Gravity = { 0.0f, -9.81f, 0.0f };
		
		// Solver settings
		u32 m_PositionSolverIterations = 2;
		u32 m_VelocitySolverIterations = 10;
		
		// System limits
		u32 m_MaxBodies = 65536;
		u32 m_MaxBodyPairs = 65536;
		u32 m_MaxContactConstraints = 10240;
		
		// Debug and capture settings
		bool m_CaptureOnPlay = true;
		PhysicsDebugType m_CaptureMethod = PhysicsDebugType::DebugToFile;
		
		// Advanced Jolt-specific settings
		f32 m_Baumgarte = 0.2f;
		f32 m_SpeculativeContactDistance = 0.02f;
		f32 m_PenetrationSlop = 0.05f;
		f32 m_LinearCastThreshold = 0.75f;
		f32 m_MinVelocityForRestitution = 1.0f;
		f32 m_TimeBeforeSleep = 0.5f;
		f32 m_PointVelocitySleepThreshold = 0.03f;
		
		// Boolean physics options
		bool m_DeterministicSimulation = true;
		bool m_ConstraintWarmStart = true;
		bool m_UseBodyPairContactCache = true;
		bool m_UseManifoldReduction = true;
		bool m_UseLargeIslandSplitter = true;
		bool m_AllowSleeping = true;
		
		// Default values method for easy reset
		[[nodiscard]] static PhysicsSettings GetDefaults()
		{
			return PhysicsSettings{};
		}
	};

}