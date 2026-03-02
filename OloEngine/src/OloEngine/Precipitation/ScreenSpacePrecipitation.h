#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"

#include <glm/glm.hpp>

#include <array>

namespace OloEngine
{
	// @brief CPU-managed lens impact for screen-space precipitation.
	struct LensImpact
	{
		glm::vec2 ScreenUV{0.0f};    // Position on screen [0,1]²
		f32 BirthTime = 0.0f;        // Time when spawned (accumulated time)
		f32 Lifetime = 0.0f;         // Total lifetime in seconds
		f32 Size = 0.0f;             // Radius in UV space
		f32 Rotation = 0.0f;         // Rotation angle in radians
		bool Active = false;         // Currently alive
	};

	// @brief GPU-ready data for lens impacts (uploaded as SSBO or uniform array).
	//        Each impact is 4 floats for efficient packing.
	struct LensImpactGPUData
	{
		glm::vec4 PositionAndSize{0.0f};   // xy = screenUV, z = size, w = rotation
		glm::vec4 TimeParams{0.0f};        // x = age/lifetime (0-1), y = 1-age/lifetime (fade), z = unused, w = active flag
	};

	// @brief Manages screen-space precipitation effects: directional snow streaks
	//        during wind gusts and lens snowflake impacts on the camera.
	//
	//        This class handles the CPU-side state (lens impact ring buffer, streak
	//        parameter calculation). The actual rendering is done by PostProcess_Precipitation.glsl
	//        in the PostProcessRenderPass chain.
	class ScreenSpacePrecipitation
	{
	public:
		static constexpr u32 MAX_LENS_IMPACTS = 16;

		static void Init();
		static void Shutdown();
		[[nodiscard]] static bool IsInitialized();

		// @brief Update lens impacts: spawn new, age existing, remove dead.
		// @param settings Precipitation settings
		// @param intensity Current smoothed intensity [0,1]
		// @param windDirScreen 2D wind direction projected onto screen
		// @param windSpeed Current wind speed
		// @param dt Delta time
		static void Update(const PrecipitationSettings& settings,
		                   f32 intensity,
		                   const glm::vec2& windDirScreen,
		                   f32 windSpeed,
		                   f32 dt);

		// @brief Get the GPU-ready lens impact array for uploading to the shader.
		[[nodiscard]] static std::array<LensImpactGPUData, MAX_LENS_IMPACTS> GetLensImpactGPUData();

		// @brief Get streak parameters for the shader.
		// @return vec4(streakDirectionXY, streakIntensity, streakLength)
		[[nodiscard]] static glm::vec4 GetStreakParams();

		// @brief Get the number of active lens impacts.
		[[nodiscard]] static u32 GetActiveLensImpactCount();

		static void Reset();

	private:
		struct ScreenSpacePrecipitationData
		{
			std::array<LensImpact, MAX_LENS_IMPACTS> m_LensImpacts{};
			u32 m_NextImpactSlot = 0;      // Ring buffer write position
			f32 m_AccumulatedTime = 0.0f;
			f32 m_ImpactSpawnAccumulator = 0.0f;  // Fractional impact spawn tracking

			// Cached streak parameters
			glm::vec2 m_StreakDirection{0.0f, -1.0f};
			f32 m_StreakIntensity = 0.0f;
			f32 m_StreakLength = 0.0f;

			bool m_Initialized = false;
		};

		static ScreenSpacePrecipitationData s_Data;
	};
} // namespace OloEngine
