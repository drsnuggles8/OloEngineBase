#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Test the Core utility systems: Hash, Identifier, and FastRandom
// These are fundamental building blocks for audio graph parameter handling
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/FastRandom.h"

TEST(CoreUtilitiesTest, HashSystemTest)
{
	// Test compile-time FNV hashing
	constexpr auto compiletimeHash1 = OloEngine::Hash::GenerateFNVHash("TestString");
	constexpr auto compiletimeHash2 = OloEngine::Hash::GenerateFNVHash("DifferentString");
	
	// Hashes should be different
	EXPECT_NE(compiletimeHash1, compiletimeHash2);
	
	// Same string should produce same hash
	constexpr auto compiletimeHash3 = OloEngine::Hash::GenerateFNVHash("TestString");
	EXPECT_EQ(compiletimeHash1, compiletimeHash3);
	
	// Test runtime CRC32 hashing
	auto runtimeHash1 = OloEngine::Hash::CRC32("TestString");
	auto runtimeHash2 = OloEngine::Hash::CRC32("DifferentString");
	
	EXPECT_NE(runtimeHash1, runtimeHash2);
	
	// Same string should produce same hash
	auto runtimeHash3 = OloEngine::Hash::CRC32("TestString");
	EXPECT_EQ(runtimeHash1, runtimeHash3);
}

TEST(CoreUtilitiesTest, IdentifierSystemTest)
{
	// Test Identifier creation and comparison
	constexpr auto param1 = OloEngine::Identifier("Volume");
	constexpr auto param2 = OloEngine::Identifier("Pitch");
	constexpr auto param3 = OloEngine::Identifier("Volume"); // Same as param1
	
	// Different identifiers should not be equal
	EXPECT_NE(param1, param2);
	
	// Same identifiers should be equal
	EXPECT_EQ(param1, param3);
	
	// Test DECLARE_IDENTIFIER macro
	DECLARE_IDENTIFIER(TestParam);
	auto testParam1 = TestParam;
	auto testParam2 = OloEngine::Identifier("TestParam");
	
	// These should be equal (same string)
	EXPECT_EQ(static_cast<uint32_t>(testParam1), static_cast<uint32_t>(testParam2));
}

TEST(CoreUtilitiesTest, FastRandomTest)
{
	// Test FastRandom with fixed seed for deterministic results
	OloEngine::FastRandom rng(12345);
	
	// Test float range generation
	auto randomFloat1 = rng.GetFloat32InRange(0.0f, 1.0f);
	auto randomFloat2 = rng.GetFloat32InRange(0.0f, 1.0f);
	
	// Values should be in range
	EXPECT_GE(randomFloat1, 0.0f);
	EXPECT_LE(randomFloat1, 1.0f);
	EXPECT_GE(randomFloat2, 0.0f);
	EXPECT_LE(randomFloat2, 1.0f);
	
	// Should generate different values (with high probability)
	EXPECT_NE(randomFloat1, randomFloat2);
	
	// Test integer range generation
	auto randomInt1 = rng.GetInt32InRange(1, 100);
	auto randomInt2 = rng.GetInt32InRange(1, 100);
	
	// Values should be in range
	EXPECT_GE(randomInt1, 1);
	EXPECT_LE(randomInt1, 100);
	EXPECT_GE(randomInt2, 1);
	EXPECT_LE(randomInt2, 100);
	
	// Test global random utils
	auto globalRandom = OloEngine::RandomUtils::Float32(0.0f, 10.0f);
	EXPECT_GE(globalRandom, 0.0f);
	EXPECT_LE(globalRandom, 10.0f);
}