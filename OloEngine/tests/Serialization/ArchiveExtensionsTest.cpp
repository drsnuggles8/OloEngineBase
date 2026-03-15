#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Serialization/ArchiveExtensions.h"

using namespace OloEngine;

// ============================================================================
// GLM Vector Round-Trip Tests
// ============================================================================

TEST(ArchiveExtensionsTest, Vec2Roundtrip)
{
    glm::vec2 original{ 1.5f, -2.75f };

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsSaveGame = true;
    writer << original;

    glm::vec2 loaded{};
    FMemoryReader reader(buffer);
    reader.ArIsSaveGame = true;
    reader << loaded;

    EXPECT_FLOAT_EQ(loaded.x, original.x);
    EXPECT_FLOAT_EQ(loaded.y, original.y);
    EXPECT_FALSE(reader.IsError());
}

TEST(ArchiveExtensionsTest, Vec3Roundtrip)
{
    glm::vec3 original{ 10.0f, -20.0f, 30.5f };

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    glm::vec3 loaded{};
    FMemoryReader reader(buffer);
    reader << loaded;

    EXPECT_FLOAT_EQ(loaded.x, original.x);
    EXPECT_FLOAT_EQ(loaded.y, original.y);
    EXPECT_FLOAT_EQ(loaded.z, original.z);
    EXPECT_FALSE(reader.IsError());
}

TEST(ArchiveExtensionsTest, Vec4Roundtrip)
{
    glm::vec4 original{ 0.1f, 0.2f, 0.3f, 1.0f };

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    glm::vec4 loaded{};
    FMemoryReader reader(buffer);
    reader << loaded;

    EXPECT_FLOAT_EQ(loaded.x, original.x);
    EXPECT_FLOAT_EQ(loaded.y, original.y);
    EXPECT_FLOAT_EQ(loaded.z, original.z);
    EXPECT_FLOAT_EQ(loaded.w, original.w);
    EXPECT_FALSE(reader.IsError());
}

TEST(ArchiveExtensionsTest, IVec3Roundtrip)
{
    glm::ivec3 original{ 4, -8, 16 };

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    glm::ivec3 loaded{};
    FMemoryReader reader(buffer);
    reader << loaded;

    EXPECT_EQ(loaded.x, original.x);
    EXPECT_EQ(loaded.y, original.y);
    EXPECT_EQ(loaded.z, original.z);
    EXPECT_FALSE(reader.IsError());
}

// ============================================================================
// GLM Matrix Round-Trip Tests
// ============================================================================

TEST(ArchiveExtensionsTest, Mat3Roundtrip)
{
    glm::mat3 original(1.0f);
    original[0][1] = 2.0f;
    original[1][2] = -3.5f;
    original[2][0] = 7.25f;

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    glm::mat3 loaded{};
    FMemoryReader reader(buffer);
    reader << loaded;

    for (i32 col = 0; col < 3; ++col)
    {
        for (i32 row = 0; row < 3; ++row)
        {
            EXPECT_FLOAT_EQ(loaded[col][row], original[col][row]);
        }
    }
    EXPECT_FALSE(reader.IsError());
}

TEST(ArchiveExtensionsTest, Mat4Roundtrip)
{
    glm::mat4 original(1.0f);
    original[0][3] = 100.0f;
    original[3][0] = -50.0f;
    original[2][2] = 3.14f;

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    glm::mat4 loaded{};
    FMemoryReader reader(buffer);
    reader << loaded;

    for (i32 col = 0; col < 4; ++col)
    {
        for (i32 row = 0; row < 4; ++row)
        {
            EXPECT_FLOAT_EQ(loaded[col][row], original[col][row]);
        }
    }
    EXPECT_FALSE(reader.IsError());
}

// ============================================================================
// GLM Quaternion Round-Trip Test
// ============================================================================

TEST(ArchiveExtensionsTest, QuatRoundtrip)
{
    glm::quat original{ 0.707f, 0.0f, 0.707f, 0.0f };

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    glm::quat loaded{};
    FMemoryReader reader(buffer);
    reader << loaded;

    EXPECT_FLOAT_EQ(loaded.x, original.x);
    EXPECT_FLOAT_EQ(loaded.y, original.y);
    EXPECT_FLOAT_EQ(loaded.z, original.z);
    EXPECT_FLOAT_EQ(loaded.w, original.w);
    EXPECT_FALSE(reader.IsError());
}

// ============================================================================
// UUID Round-Trip Test
// ============================================================================

TEST(ArchiveExtensionsTest, UUIDRoundtrip)
{
    UUID original; // random UUID

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    UUID loaded{};
    FMemoryReader reader(buffer);
    reader << loaded;

    EXPECT_EQ(static_cast<u64>(loaded), static_cast<u64>(original));
    EXPECT_FALSE(reader.IsError());
}

TEST(ArchiveExtensionsTest, UUIDZeroRoundtrip)
{
    UUID original(0);

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    UUID loaded(12345);
    FMemoryReader reader(buffer);
    reader << loaded;

    EXPECT_EQ(static_cast<u64>(loaded), 0u);
    EXPECT_FALSE(reader.IsError());
}

// ============================================================================
// Container Round-Trip Tests
// ============================================================================

TEST(ArchiveExtensionsTest, VectorOfFloatsRoundtrip)
{
    std::vector<f32> original{ 1.0f, 2.5f, -3.14f, 0.0f, 999.99f };

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    std::vector<f32> loaded;
    FMemoryReader reader(buffer);
    reader << loaded;

    ASSERT_EQ(loaded.size(), original.size());
    for (sizet i = 0; i < original.size(); ++i)
    {
        EXPECT_FLOAT_EQ(loaded[i], original[i]);
    }
    EXPECT_FALSE(reader.IsError());
}

TEST(ArchiveExtensionsTest, EmptyVectorRoundtrip)
{
    std::vector<u32> original;

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    std::vector<u32> loaded{ 1, 2, 3 }; // Pre-fill to verify it clears
    FMemoryReader reader(buffer);
    reader << loaded;

    EXPECT_TRUE(loaded.empty());
    EXPECT_FALSE(reader.IsError());
}

TEST(ArchiveExtensionsTest, VectorOfVec3Roundtrip)
{
    std::vector<glm::vec3> original{
        { 1.0f, 2.0f, 3.0f },
        { -4.0f, 5.5f, -6.0f },
    };

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    std::vector<glm::vec3> loaded;
    FMemoryReader reader(buffer);
    reader << loaded;

    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_FLOAT_EQ(loaded[0].x, 1.0f);
    EXPECT_FLOAT_EQ(loaded[1].y, 5.5f);
    EXPECT_FALSE(reader.IsError());
}

TEST(ArchiveExtensionsTest, VectorOfStringsRoundtrip)
{
    std::vector<std::string> original{ "hello", "world", "", "test" };

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    std::vector<std::string> loaded;
    FMemoryReader reader(buffer);
    reader << loaded;

    ASSERT_EQ(loaded.size(), original.size());
    for (sizet i = 0; i < original.size(); ++i)
    {
        EXPECT_EQ(loaded[i], original[i]);
    }
    EXPECT_FALSE(reader.IsError());
}

TEST(ArchiveExtensionsTest, UnorderedMapRoundtrip)
{
    std::unordered_map<std::string, f32> original{
        { "speed", 10.0f },
        { "health", 100.0f },
        { "armor", 50.5f },
    };

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    std::unordered_map<std::string, f32> loaded;
    FMemoryReader reader(buffer);
    reader << loaded;

    ASSERT_EQ(loaded.size(), original.size());
    for (auto const& [key, value] : original)
    {
        ASSERT_TRUE(loaded.contains(key));
        EXPECT_FLOAT_EQ(loaded[key], value);
    }
    EXPECT_FALSE(reader.IsError());
}

TEST(ArchiveExtensionsTest, EmptyMapRoundtrip)
{
    std::unordered_map<u32, std::string> original;

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    std::unordered_map<u32, std::string> loaded;
    loaded[1] = "should be cleared";
    FMemoryReader reader(buffer);
    reader << loaded;

    EXPECT_TRUE(loaded.empty());
    EXPECT_FALSE(reader.IsError());
}

TEST(ArchiveExtensionsTest, VectorOfUUIDsRoundtrip)
{
    std::vector<UUID> original;
    original.emplace_back();
    original.emplace_back();
    original.emplace_back(0);

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original;

    std::vector<UUID> loaded;
    FMemoryReader reader(buffer);
    reader << loaded;

    ASSERT_EQ(loaded.size(), original.size());
    for (sizet i = 0; i < original.size(); ++i)
    {
        EXPECT_EQ(static_cast<u64>(loaded[i]), static_cast<u64>(original[i]));
    }
    EXPECT_FALSE(reader.IsError());
}

// ============================================================================
// Buffer Size Verification
// ============================================================================

TEST(ArchiveExtensionsTest, Vec3ProducesExpectedSize)
{
    glm::vec3 v{ 1.0f, 2.0f, 3.0f };
    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << v;

    EXPECT_EQ(buffer.size(), 3 * sizeof(f32));
}

TEST(ArchiveExtensionsTest, Mat4ProducesExpectedSize)
{
    glm::mat4 m(1.0f);
    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << m;

    EXPECT_EQ(buffer.size(), 16 * sizeof(f32));
}
