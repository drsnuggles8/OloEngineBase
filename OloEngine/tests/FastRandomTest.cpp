#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/FastRandom.h"
#include <unordered_set>
#include <cmath>

using namespace OloEngine;

//==============================================================================
// Basic Functionality Tests
//==============================================================================

TEST(FastRandomTest, LCGBasicGeneration)
{
    FastRandomLCG rng(12345);
    
    // Generate some values to verify it's working
    u32 val1 = rng.GetUInt32();
    u32 val2 = rng.GetUInt32();
    u32 val3 = rng.GetUInt32();
    
    // Values should be different
    EXPECT_NE(val1, val2);
    EXPECT_NE(val2, val3);
    EXPECT_NE(val1, val3);
}

TEST(FastRandomTest, PCG32BasicGeneration)
{
    FastRandomPCG rng(12345);
    
    u32 val1 = rng.GetUInt32();
    u32 val2 = rng.GetUInt32();
    u32 val3 = rng.GetUInt32();
    
    EXPECT_NE(val1, val2);
    EXPECT_NE(val2, val3);
    EXPECT_NE(val1, val3);
}

TEST(FastRandomTest, SplitMix64BasicGeneration)
{
    FastRandomSplitMix rng(12345);
    
    u64 val1 = rng.GetUInt64();
    u64 val2 = rng.GetUInt64();
    u64 val3 = rng.GetUInt64();
    
    EXPECT_NE(val1, val2);
    EXPECT_NE(val2, val3);
    EXPECT_NE(val1, val3);
}

TEST(FastRandomTest, Xoshiro256ppBasicGeneration)
{
    FastRandomXoshiro rng(12345);
    
    u64 val1 = rng.GetUInt64();
    u64 val2 = rng.GetUInt64();
    u64 val3 = rng.GetUInt64();
    
    EXPECT_NE(val1, val2);
    EXPECT_NE(val2, val3);
    EXPECT_NE(val1, val3);
}

//==============================================================================
// Reproducibility Tests (same seed = same sequence)
//==============================================================================

TEST(FastRandomTest, ReproducibilityLCG)
{
    FastRandomLCG rng1(42);
    FastRandomLCG rng2(42);
    
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_EQ(rng1.GetUInt32(), rng2.GetUInt32());
    }
}

TEST(FastRandomTest, ReproducibilityPCG32)
{
    FastRandomPCG rng1(42);
    FastRandomPCG rng2(42);
    
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_EQ(rng1.GetUInt32(), rng2.GetUInt32());
    }
}

TEST(FastRandomTest, ReproducibilitySplitMix64)
{
    FastRandomSplitMix rng1(42);
    FastRandomSplitMix rng2(42);
    
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_EQ(rng1.GetUInt64(), rng2.GetUInt64());
    }
}

TEST(FastRandomTest, ReproducibilityXoshiro256pp)
{
    FastRandomXoshiro rng1(42);
    FastRandomXoshiro rng2(42);
    
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_EQ(rng1.GetUInt64(), rng2.GetUInt64());
    }
}

//==============================================================================
// Type Coverage Tests - 8-bit types
//==============================================================================

TEST(FastRandomTest, Int8Generation)
{
    FastRandomPCG rng(12345);
    
    std::unordered_set<i8> values;
    for (int i = 0; i < 1000; ++i)
    {
        i8 val = rng.GetInt8();
        values.insert(val);
    }
    
    // Should generate diverse values (at least 50 unique values out of 256 possible)
    EXPECT_GT(values.size(), 50u);
}

TEST(FastRandomTest, UInt8Generation)
{
    FastRandomPCG rng(12345);
    
    std::unordered_set<u8> values;
    for (int i = 0; i < 1000; ++i)
    {
        u8 val = rng.GetUInt8();
        values.insert(val);
    }
    
    EXPECT_GT(values.size(), 50u);
}

TEST(FastRandomTest, Int8InRange)
{
    FastRandomPCG rng(12345);
    
    i8 low = -50;
    i8 high = 50;
    
    for (int i = 0; i < 100; ++i)
    {
        i8 val = rng.GetInt8InRange(low, high);
        EXPECT_GE(val, low);
        EXPECT_LE(val, high);
    }
}

TEST(FastRandomTest, UInt8InRange)
{
    FastRandomPCG rng(12345);
    
    u8 low = 10;
    u8 high = 200;
    
    for (int i = 0; i < 100; ++i)
    {
        u8 val = rng.GetUInt8InRange(low, high);
        EXPECT_GE(val, low);
        EXPECT_LE(val, high);
    }
}

//==============================================================================
// Type Coverage Tests - 16-bit types
//==============================================================================

TEST(FastRandomTest, Int16Generation)
{
    FastRandomPCG rng(12345);
    
    std::unordered_set<i16> values;
    for (int i = 0; i < 1000; ++i)
    {
        i16 val = rng.GetInt16();
        values.insert(val);
    }
    
    EXPECT_GT(values.size(), 500u);
}

TEST(FastRandomTest, UInt16Generation)
{
    FastRandomPCG rng(12345);
    
    std::unordered_set<u16> values;
    for (int i = 0; i < 1000; ++i)
    {
        u16 val = rng.GetUInt16();
        values.insert(val);
    }
    
    EXPECT_GT(values.size(), 500u);
}

TEST(FastRandomTest, Int16InRange)
{
    FastRandomPCG rng(12345);
    
    i16 low = -1000;
    i16 high = 1000;
    
    for (int i = 0; i < 100; ++i)
    {
        i16 val = rng.GetInt16InRange(low, high);
        EXPECT_GE(val, low);
        EXPECT_LE(val, high);
    }
}

TEST(FastRandomTest, UInt16InRange)
{
    FastRandomPCG rng(12345);
    
    u16 low = 1000;
    u16 high = 50000;
    
    for (int i = 0; i < 100; ++i)
    {
        u16 val = rng.GetUInt16InRange(low, high);
        EXPECT_GE(val, low);
        EXPECT_LE(val, high);
    }
}

//==============================================================================
// Type Coverage Tests - 32-bit types
//==============================================================================

TEST(FastRandomTest, Int32Generation)
{
    FastRandomPCG rng(12345);
    
    std::unordered_set<i32> values;
    for (int i = 0; i < 1000; ++i)
    {
        i32 val = rng.GetInt32();
        values.insert(val);
    }
    
    EXPECT_GT(values.size(), 990u);  // Should be nearly all unique
}

TEST(FastRandomTest, UInt32Generation)
{
    FastRandomPCG rng(12345);
    
    std::unordered_set<u32> values;
    for (int i = 0; i < 1000; ++i)
    {
        u32 val = rng.GetUInt32();
        values.insert(val);
    }
    
    EXPECT_GT(values.size(), 990u);
}

TEST(FastRandomTest, Int32InRange)
{
    FastRandomPCG rng(12345);
    
    i32 low = -1000000;
    i32 high = 1000000;
    
    for (int i = 0; i < 100; ++i)
    {
        i32 val = rng.GetInt32InRange(low, high);
        EXPECT_GE(val, low);
        EXPECT_LE(val, high);
    }
}

TEST(FastRandomTest, UInt32InRange)
{
    FastRandomPCG rng(12345);
    
    u32 low = 1000000;
    u32 high = 5000000;
    
    for (int i = 0; i < 100; ++i)
    {
        u32 val = rng.GetUInt32InRange(low, high);
        EXPECT_GE(val, low);
        EXPECT_LE(val, high);
    }
}

//==============================================================================
// Type Coverage Tests - 64-bit types
//==============================================================================

TEST(FastRandomTest, Int64Generation)
{
    FastRandomPCG rng(12345);
    
    std::unordered_set<i64> values;
    for (int i = 0; i < 1000; ++i)
    {
        i64 val = rng.GetInt64();
        values.insert(val);
    }
    
    EXPECT_GT(values.size(), 990u);
}

TEST(FastRandomTest, UInt64Generation)
{
    FastRandomPCG rng(12345);
    
    std::unordered_set<u64> values;
    for (int i = 0; i < 1000; ++i)
    {
        u64 val = rng.GetUInt64();
        values.insert(val);
    }
    
    EXPECT_GT(values.size(), 990u);
}

TEST(FastRandomTest, Int64InRange)
{
    FastRandomPCG rng(12345);
    
    i64 low = -1000000000000LL;
    i64 high = 1000000000000LL;
    
    for (int i = 0; i < 100; ++i)
    {
        i64 val = rng.GetInt64InRange(low, high);
        EXPECT_GE(val, low);
        EXPECT_LE(val, high);
    }
}

TEST(FastRandomTest, UInt64InRange)
{
    FastRandomPCG rng(12345);
    
    u64 low = 1000000000000ULL;
    u64 high = 5000000000000ULL;
    
    for (int i = 0; i < 100; ++i)
    {
        u64 val = rng.GetUInt64InRange(low, high);
        EXPECT_GE(val, low);
        EXPECT_LE(val, high);
    }
}

//==============================================================================
// Type Coverage Tests - Floating point types
//==============================================================================

TEST(FastRandomTest, Float32Generation)
{
    FastRandomPCG rng(12345);
    
    for (int i = 0; i < 100; ++i)
    {
        f32 val = rng.GetFloat32();
        EXPECT_GE(val, 0.0f);
        EXPECT_LT(val, 1.0f);
    }
}

TEST(FastRandomTest, Float64Generation)
{
    FastRandomPCG rng(12345);
    
    for (int i = 0; i < 100; ++i)
    {
        f64 val = rng.GetFloat64();
        EXPECT_GE(val, 0.0);
        EXPECT_LT(val, 1.0);
    }
}

TEST(FastRandomTest, Float32InRange)
{
    FastRandomPCG rng(12345);
    
    f32 low = -100.5f;
    f32 high = 100.5f;
    
    for (int i = 0; i < 100; ++i)
    {
        f32 val = rng.GetFloat32InRange(low, high);
        EXPECT_GE(val, low);
        EXPECT_LE(val, high);
    }
}

TEST(FastRandomTest, Float64InRange)
{
    FastRandomPCG rng(12345);
    
    f64 low = -1000.5;
    f64 high = 1000.5;
    
    for (int i = 0; i < 100; ++i)
    {
        f64 val = rng.GetFloat64InRange(low, high);
        EXPECT_GE(val, low);
        EXPECT_LE(val, high);
    }
}

//==============================================================================
// Utility Function Tests
//==============================================================================

TEST(FastRandomTest, BoolGeneration)
{
    FastRandomPCG rng(12345);
    
    int trueCount = 0;
    int falseCount = 0;
    
    for (int i = 0; i < 1000; ++i)
    {
        if (rng.GetBool())
            trueCount++;
        else
            falseCount++;
    }
    
    // Should be roughly balanced (within 40-60% range)
    EXPECT_GT(trueCount, 300);
    EXPECT_LT(trueCount, 700);
    EXPECT_GT(falseCount, 300);
    EXPECT_LT(falseCount, 700);
}

TEST(FastRandomTest, NormalizedFloat)
{
    FastRandomPCG rng(12345);
    
    for (int i = 0; i < 100; ++i)
    {
        f32 val = rng.GetNormalizedFloat();
        EXPECT_GE(val, 0.0f);
        EXPECT_LT(val, 1.0f);
    }
}

TEST(FastRandomTest, BipolarFloat)
{
    FastRandomPCG rng(12345);
    
    for (int i = 0; i < 100; ++i)
    {
        f32 val = rng.GetBipolarFloat();
        EXPECT_GE(val, -1.0f);
        EXPECT_LE(val, 1.0f);
    }
}

//==============================================================================
// Template GetInRange Tests
//==============================================================================

TEST(FastRandomTest, TemplateGetInRangeInt32)
{
    FastRandomPCG rng(12345);
    
    for (int i = 0; i < 100; ++i)
    {
        i32 val = rng.GetInRange<i32>(-1000, 1000);
        EXPECT_GE(val, -1000);
        EXPECT_LE(val, 1000);
    }
}

TEST(FastRandomTest, TemplateGetInRangeFloat32)
{
    FastRandomPCG rng(12345);
    
    for (int i = 0; i < 100; ++i)
    {
        f32 val = rng.GetInRange<f32>(-10.5f, 10.5f);
        EXPECT_GE(val, -10.5f);
        EXPECT_LE(val, 10.5f);
    }
}

TEST(FastRandomTest, TemplateGetInRangeInt64)
{
    FastRandomPCG rng(12345);
    
    for (int i = 0; i < 100; ++i)
    {
        i64 val = rng.GetInRange<i64>(-1000000000000LL, 1000000000000LL);
        EXPECT_GE(val, -1000000000000LL);
        EXPECT_LE(val, 1000000000000LL);
    }
}

//==============================================================================
// Edge Case Tests
//==============================================================================

TEST(FastRandomTest, RangeWithEqualBounds)
{
    FastRandomPCG rng(12345);
    
    EXPECT_EQ(rng.GetInt32InRange(42, 42), 42);
    EXPECT_EQ(rng.GetUInt32InRange(100, 100), 100u);
    EXPECT_FLOAT_EQ(rng.GetFloat32InRange(3.14f, 3.14f), 3.14f);
}

TEST(FastRandomTest, RangeWithSwappedBounds)
{
    FastRandomPCG rng(12345);
    
    // Float ranges automatically swap
    for (int i = 0; i < 10; ++i)
    {
        f32 val = rng.GetFloat32InRange(100.0f, 10.0f);
        EXPECT_GE(val, 10.0f);
        EXPECT_LE(val, 100.0f);
    }
}

TEST(FastRandomTest, SmallRanges)
{
    FastRandomPCG rng(12345);
    
    // Range of 0-1 for integers
    for (int i = 0; i < 100; ++i)
    {
        i32 val = rng.GetInt32InRange(0, 1);
        EXPECT_TRUE(val == 0 || val == 1);
    }
}

TEST(FastRandomTest, LargeRanges)
{
    FastRandomPCG rng(12345);
    
    // Full 64-bit range
    for (int i = 0; i < 10; ++i)
    {
        u64 val = rng.GetUInt64InRange(0, UINT64_MAX - 1);
        // Just verify it doesn't crash and produces valid values
        (void)val;  // Suppress unused warning
    }
}

//==============================================================================
// Seed Management Tests
//==============================================================================

TEST(FastRandomTest, SetSeedChangesSequence)
{
    FastRandomPCG rng(12345);
    
    u32 val1 = rng.GetUInt32();
    
    rng.SetSeed(12345);  // Reset to same seed
    u32 val2 = rng.GetUInt32();
    
    EXPECT_EQ(val1, val2);  // Should produce same first value
}

TEST(FastRandomTest, DifferentSeedsDifferentSequences)
{
    FastRandomPCG rng1(12345);
    FastRandomPCG rng2(54321);
    
    u32 val1 = rng1.GetUInt32();
    u32 val2 = rng2.GetUInt32();
    
    EXPECT_NE(val1, val2);  // Different seeds = different values
}

//==============================================================================
// RandomUtils Namespace Tests
//==============================================================================

TEST(FastRandomTest, GlobalRandomAccessible)
{
    // Should be able to call global random functions
    f32 val1 = RandomUtils::Float32();
    f32 val2 = RandomUtils::Float32(0.0f, 100.0f);
    i32 val3 = RandomUtils::Int32(-100, 100);
    bool val4 = RandomUtils::Bool();
    
    EXPECT_GE(val1, 0.0f);
    EXPECT_LT(val1, 1.0f);
    EXPECT_GE(val2, 0.0f);
    EXPECT_LE(val2, 100.0f);
    EXPECT_GE(val3, -100);
    EXPECT_LE(val3, 100);
    (void)val4;  // Just verify it compiles
}

TEST(FastRandomTest, AllGlobalConvenienceFunctions)
{
    // Test all new convenience functions
    i8 vi8 = RandomUtils::Int8(-50, 50);
    u8 vu8 = RandomUtils::UInt8(0, 200);
    i16 vi16 = RandomUtils::Int16(-1000, 1000);
    u16 vu16 = RandomUtils::UInt16(0, 50000);
    i32 vi32 = RandomUtils::Int32(-1000000, 1000000);
    u32 vu32 = RandomUtils::UInt32(0, 5000000);
    i64 vi64 = RandomUtils::Int64(-1000000000000LL, 1000000000000LL);
    u64 vu64 = RandomUtils::UInt64(0, 5000000000000ULL);
    f64 vf64 = RandomUtils::Float64(0.0, 100.0);
    
    EXPECT_GE(vi8, -50);
    EXPECT_LE(vi8, 50);
    EXPECT_LE(vu8, 200);
    EXPECT_GE(vi16, -1000);
    EXPECT_LE(vi16, 1000);
    EXPECT_LE(vu16, 50000);
    EXPECT_GE(vi32, -1000000);
    EXPECT_LE(vi32, 1000000);
    EXPECT_LE(vu32, 5000000u);
    EXPECT_GE(vi64, -1000000000000LL);
    EXPECT_LE(vi64, 1000000000000LL);
    EXPECT_LE(vu64, 5000000000000ULL);
    EXPECT_GE(vf64, 0.0);
    EXPECT_LE(vf64, 100.0);
}

//==============================================================================
// Statistical Distribution Tests (basic)
//==============================================================================

TEST(FastRandomTest, UniformDistributionInt32)
{
    FastRandomPCG rng(12345);
    
    const int buckets = 10;
    const int samples = 10000;
    int counts[buckets] = {0};
    
    for (int i = 0; i < samples; ++i)
    {
        i32 val = rng.GetInt32InRange(0, buckets - 1);
        counts[val]++;
    }
    
    // Each bucket should get roughly samples/buckets values
    // Allow for statistical variance (within 30% of expected)
    int expected = samples / buckets;
    for (int i = 0; i < buckets; ++i)
    {
        EXPECT_GT(counts[i], static_cast<int>(expected * 0.7));
        EXPECT_LT(counts[i], static_cast<int>(expected * 1.3));
    }
}

TEST(FastRandomTest, UniformDistributionFloat32)
{
    FastRandomPCG rng(12345);
    
    const int buckets = 10;
    const int samples = 10000;
    int counts[buckets] = {0};
    
    for (int i = 0; i < samples; ++i)
    {
        f32 val = rng.GetFloat32();
        int bucket = static_cast<int>(val * buckets);
        if (bucket >= 0 && bucket < buckets)
            counts[bucket]++;
    }
    
    int expected = samples / buckets;
    for (int i = 0; i < buckets; ++i)
    {
        EXPECT_GT(counts[i], static_cast<int>(expected * 0.7));
        EXPECT_LT(counts[i], static_cast<int>(expected * 1.3));
    }
}

//==============================================================================
// 64-bit Algorithm Specific Tests
//==============================================================================

//==============================================================================
// Performance Sanity Tests
//==============================================================================

TEST(FastRandomTest, PerformanceBaseline)
{
    FastRandomPCG rng(12345);
    
    // Just verify we can generate lots of numbers quickly without crashing
    const int iterations = 100000;
    
    for (int i = 0; i < iterations; ++i)
    {
        rng.GetUInt32();
    }
    
    // If we got here, performance is acceptable
    SUCCEED();
}
