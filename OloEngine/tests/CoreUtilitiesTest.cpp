#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include <unordered_map>

// Core utility contracts: Hash, Identifier, Reflection TypeUtils.
// FastRandom block was retired here — FastRandomTest.cpp owns that
// contract end-to-end (per-algorithm reproducibility + pinned-output
// vector + range correctness). See docs/testing.md §4.7.
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/Reflection/TypeUtils.h"

namespace
{
    // Test types for IsSpecialized trait testing
    struct RegularType
    {
    };

    struct SpecializedType
    {
        using ReflectionSpecializationTag = void;
    };

    template<typename T>
    struct TemplateType
    {
    };

    template<typename T>
    struct SpecializedTemplateType
    {
        using ReflectionSpecializationTag = void;
    };
} // namespace

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

    // Test hash functor with std::unordered_map
    std::unordered_map<OloEngine::Identifier, int> identifierMap;
    identifierMap[param1] = 100;
    identifierMap[param2] = 200;

    // Verify hash-based lookup works
    EXPECT_EQ(identifierMap[param1], 100);
    EXPECT_EQ(identifierMap[param2], 200);
    EXPECT_EQ(identifierMap[param3], 100); // param3 == param1
}

TEST(CoreUtilitiesTest, IsSpecializedTest)
{
    using namespace OloEngine::Core::Reflection;

    // Test type without specialization marker
    EXPECT_FALSE(IsSpecialized_v<RegularType>);

    // Test type with specialization marker
    EXPECT_TRUE(IsSpecialized_v<SpecializedType>);

    // Test template type without marker
    EXPECT_FALSE(IsSpecialized_v<TemplateType<int>>);

    // Test template type with marker
    EXPECT_TRUE(IsSpecialized_v<SpecializedTemplateType<int>>);
}
