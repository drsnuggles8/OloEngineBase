#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Precipitation/PrecipitationEmitter.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Particle/GPUParticleData.h"

#include <glm/glm.hpp>

#include <chrono>
#include <numeric>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

// =============================================================================
// Emission Generation Benchmark
// =============================================================================

TEST(PrecipitationBenchmark, EmissionGeneration1k)
{
	glm::vec3 cameraPos(0.0f, 50.0f, 0.0f);
	PrecipitationSettings settings;
	settings.Enabled = true;
	settings.BaseEmissionRate = 1000;

	auto start = std::chrono::high_resolution_clock::now();

	auto particles = PrecipitationEmitter::GenerateSnowParticles(
		cameraPos, cameraPos, settings, 1.0f,
		PrecipitationLayer::NearField, glm::vec3(1.0f, 0.0f, 0.0f), 10.0f, 1.0f);

	auto end = std::chrono::high_resolution_clock::now();
	auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

	// Should complete in under 500ms even in Debug builds on slow machines
	EXPECT_LT(durationUs, 500000);
	EXPECT_GT(particles.size(), 0u);
}

TEST(PrecipitationBenchmark, EmissionGeneration10k)
{
	glm::vec3 cameraPos(0.0f, 50.0f, 0.0f);
	PrecipitationSettings settings;
	settings.Enabled = true;
	settings.BaseEmissionRate = 10000;

	auto start = std::chrono::high_resolution_clock::now();

	auto particles = PrecipitationEmitter::GenerateSnowParticles(
		cameraPos, cameraPos, settings, 1.0f,
		PrecipitationLayer::NearField, glm::vec3(1.0f, 0.0f, 0.0f), 10.0f, 1.0f);

	auto end = std::chrono::high_resolution_clock::now();
	auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

	// Should complete in under 5s even in Debug builds on slow machines
	EXPECT_LT(durationUs, 5000000);
	EXPECT_GT(particles.size(), 0u);
}

// =============================================================================
// Intensity Interpolation Benchmark
// =============================================================================

TEST(PrecipitationBenchmark, IntensityInterpolation10kIterations)
{
	f32 current = 0.0f;
	f32 target = 1.0f;
	f32 speed = 2.0f;
	f32 dt = 0.016f;

	auto start = std::chrono::high_resolution_clock::now();

	for (int i = 0; i < 10000; ++i)
	{
		f32 lerpFactor = std::clamp(speed * dt, 0.0f, 1.0f);
		current = std::lerp(current, target, lerpFactor);
	}

	auto end = std::chrono::high_resolution_clock::now();
	auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

	// 10k interpolations should be sub-millisecond
	EXPECT_LT(durationUs, 1000);
	EXPECT_NEAR(current, target, 0.001f);
}

// =============================================================================
// Memory Footprint Tests
// =============================================================================

TEST(PrecipitationBenchmark, GPUParticleIs96Bytes)
{
	EXPECT_EQ(sizeof(GPUParticle), 96u);
}

TEST(PrecipitationBenchmark, MaxSSBOSizeNearField)
{
	// 100k particles * 96 bytes = 9.6 MB — well within typical GL_MAX_SHADER_STORAGE_BLOCK_SIZE (128 MB+)
	constexpr u32 maxNear = 100000;
	constexpr u64 sizeBytes = static_cast<u64>(maxNear) * sizeof(GPUParticle);
	EXPECT_LT(sizeBytes, 128ULL * 1024 * 1024); // Under 128 MB
	EXPECT_EQ(sizeBytes, 9600000ULL); // Exactly 9.6 MB
}

TEST(PrecipitationBenchmark, MaxSSBOSizeFarField)
{
	// 200k particles * 96 bytes = 19.2 MB
	constexpr u32 maxFar = 200000;
	constexpr u64 sizeBytes = static_cast<u64>(maxFar) * sizeof(GPUParticle);
	EXPECT_LT(sizeBytes, 128ULL * 1024 * 1024); // Under 128 MB
	EXPECT_EQ(sizeBytes, 19200000ULL); // Exactly 19.2 MB
}

TEST(PrecipitationBenchmark, TotalMemoryFootprintReasonable)
{
	// Combined SSBO memory: near (9.6 MB) + far (19.2 MB) = 28.8 MB
	// Plus additional buffers (alive indices, counters, etc.) roughly 2x
	// Total should be under 100 MB
	constexpr u64 nearBytes = 100000ULL * sizeof(GPUParticle);
	constexpr u64 farBytes = 200000ULL * sizeof(GPUParticle);
	constexpr u64 overheadFactor = 3; // Conservative: particles + indices + counters + staging

	u64 totalEstimate = (nearBytes + farBytes) * overheadFactor;
	EXPECT_LT(totalEstimate, 100ULL * 1024 * 1024); // Under 100 MB
}

// =============================================================================
// Throughput Test
// =============================================================================

TEST(PrecipitationBenchmark, EmissionRateCalculationThroughput)
{
	constexpr int iterations = 100000;

	auto start = std::chrono::high_resolution_clock::now();

	f32 totalRate = 0.0f;
	for (int i = 0; i < iterations; ++i)
	{
		f32 intensity = static_cast<f32>(i) / static_cast<f32>(iterations);
		totalRate += PrecipitationEmitter::CalculateEmissionRate(4000, intensity);
	}

	auto end = std::chrono::high_resolution_clock::now();
	auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

	// 100k calculations should be sub-10ms even in Debug
	EXPECT_LT(durationUs, 10000);
	EXPECT_GT(totalRate, 0.0f); // Prevent optimization
}
