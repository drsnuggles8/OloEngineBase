#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Templates/UnrealTypeTraits.h"

using namespace OloEngine;

// ============================================================================
// TFormatSpecifier Tests
// ============================================================================

TEST(TypeTraitsTest, TFormatSpecifier_ReturnsCorrectFormatForIntegers)
{
    EXPECT_STREQ(TFormatSpecifier<u8>::GetFormatSpecifier(), "%u");
    EXPECT_STREQ(TFormatSpecifier<u16>::GetFormatSpecifier(), "%u");
    EXPECT_STREQ(TFormatSpecifier<u32>::GetFormatSpecifier(), "%u");
    EXPECT_STREQ(TFormatSpecifier<u64>::GetFormatSpecifier(), "%llu");

    EXPECT_STREQ(TFormatSpecifier<i8>::GetFormatSpecifier(), "%d");
    EXPECT_STREQ(TFormatSpecifier<i16>::GetFormatSpecifier(), "%d");
    EXPECT_STREQ(TFormatSpecifier<i32>::GetFormatSpecifier(), "%d");
    EXPECT_STREQ(TFormatSpecifier<i64>::GetFormatSpecifier(), "%lld");
}

TEST(TypeTraitsTest, TFormatSpecifier_ReturnsCorrectFormatForFloats)
{
    EXPECT_STREQ(TFormatSpecifier<f32>::GetFormatSpecifier(), "%f");
    EXPECT_STREQ(TFormatSpecifier<f64>::GetFormatSpecifier(), "%f");
    EXPECT_STREQ(TFormatSpecifier<long double>::GetFormatSpecifier(), "%f");
}

TEST(TypeTraitsTest, TFormatSpecifier_ReturnsCorrectFormatForBool)
{
    EXPECT_STREQ(TFormatSpecifier<bool>::GetFormatSpecifier(), "%i");
}

// ============================================================================
// TNameOf Tests
// ============================================================================

TEST(TypeTraitsTest, TNameOf_ReturnsCorrectNameForIntegers)
{
    EXPECT_STREQ(TNameOf<u8>::GetName(), "u8");
    EXPECT_STREQ(TNameOf<u16>::GetName(), "u16");
    EXPECT_STREQ(TNameOf<u32>::GetName(), "u32");
    EXPECT_STREQ(TNameOf<u64>::GetName(), "u64");

    EXPECT_STREQ(TNameOf<i8>::GetName(), "i8");
    EXPECT_STREQ(TNameOf<i16>::GetName(), "i16");
    EXPECT_STREQ(TNameOf<i32>::GetName(), "i32");
    EXPECT_STREQ(TNameOf<i64>::GetName(), "i64");
}

TEST(TypeTraitsTest, TNameOf_ReturnsCorrectNameForFloats)
{
    EXPECT_STREQ(TNameOf<f32>::GetName(), "f32");
    EXPECT_STREQ(TNameOf<f64>::GetName(), "f64");
}

// ============================================================================
// TNthTypeFromParameterPack Tests
// ============================================================================

TEST(TypeTraitsTest, TNthTypeFromParameterPack_ReturnsCorrectType)
{
    static_assert(std::is_same_v<TNthTypeFromParameterPack_T<0, int, float, double>, int>);
    static_assert(std::is_same_v<TNthTypeFromParameterPack_T<1, int, float, double>, float>);
    static_assert(std::is_same_v<TNthTypeFromParameterPack_T<2, int, float, double>, double>);

    // Just verify it compiles - static_asserts above do the real check
    SUCCEED();
}

// ============================================================================
// TIsFundamentalType Tests
// ============================================================================

TEST(TypeTraitsTest, TIsFundamentalType_TrueForArithmetic)
{
    EXPECT_TRUE(TIsFundamentalType<int>::Value);
    EXPECT_TRUE(TIsFundamentalType<float>::Value);
    EXPECT_TRUE(TIsFundamentalType<double>::Value);
    EXPECT_TRUE(TIsFundamentalType<char>::Value);
    EXPECT_TRUE(TIsFundamentalType<bool>::Value);
}

TEST(TypeTraitsTest, TIsFundamentalType_TrueForVoid)
{
    EXPECT_TRUE(TIsFundamentalType<void>::Value);
}

TEST(TypeTraitsTest, TIsFundamentalType_FalseForClassTypes)
{
    struct TestStruct
    {
    };
    EXPECT_FALSE(TIsFundamentalType<TestStruct>::Value);
    EXPECT_FALSE(TIsFundamentalType<std::string>::Value);
}

// ============================================================================
// TIsFunction Tests
// ============================================================================

TEST(TypeTraitsTest, TIsFunction_TrueForFunctions)
{
    EXPECT_TRUE((TIsFunction<void()>::Value));
    EXPECT_TRUE((TIsFunction<int(float, double)>::Value));
    EXPECT_TRUE((TIsFunction<void(int, int, int)>::Value));
}

TEST(TypeTraitsTest, TIsFunction_FalseForNonFunctions)
{
    EXPECT_FALSE(TIsFunction<int>::Value);
    EXPECT_FALSE(TIsFunction<int*>::Value);
    EXPECT_FALSE((TIsFunction<int (*)(float)>::Value)); // Function pointer, not function
}

// ============================================================================
// TCallTraits Tests
// ============================================================================

TEST(TypeTraitsTest, TCallTraits_SmallPODPassedByValue)
{
    // Small POD types should be passed by value (const T)
    static_assert(std::is_same_v<TCallTraits<int>::ParamType, const int>);
    static_assert(std::is_same_v<TCallTraits<float>::ParamType, const float>);
    SUCCEED();
}

TEST(TypeTraitsTest, TCallTraits_LargeTypesPassedByReference)
{
    struct LargeStruct
    {
        char data[1024];
    };
    // Large types should be passed by const reference
    static_assert(std::is_same_v<TCallTraits<LargeStruct>::ParamType, const LargeStruct&>);
    SUCCEED();
}

TEST(TypeTraitsTest, TCallTraits_PointersPassedByValue)
{
    // Pointers should be passed by value
    static_assert(std::is_same_v<TCallTraits<int*>::ParamType, int*>);
    SUCCEED();
}

// ============================================================================
// TIsBitwiseConstructible Tests
// ============================================================================

TEST(TypeTraitsTest, TIsBitwiseConstructible_TrueForSameType)
{
    EXPECT_TRUE((TIsBitwiseConstructible<int, int>::Value));
    EXPECT_TRUE((TIsBitwiseConstructible<float, float>::Value));
}

TEST(TypeTraitsTest, TIsBitwiseConstructible_TrueForSignedUnsignedPairs)
{
    EXPECT_TRUE((TIsBitwiseConstructible<u32, i32>::Value));
    EXPECT_TRUE((TIsBitwiseConstructible<i32, u32>::Value));
    EXPECT_TRUE((TIsBitwiseConstructible<u64, i64>::Value));
    EXPECT_TRUE((TIsBitwiseConstructible<i64, u64>::Value));
}

TEST(TypeTraitsTest, TIsBitwiseConstructible_TrueForConstPointerFromNonConst)
{
    EXPECT_TRUE((TIsBitwiseConstructible<const int*, int*>::Value));
}

// ============================================================================
// Basic Type Trait Tests
// ============================================================================

TEST(TypeTraitsTest, TIsZeroConstructType_TrueForFundamentals)
{
    EXPECT_TRUE(TIsZeroConstructType<int>::Value);
    EXPECT_TRUE(TIsZeroConstructType<float>::Value);
    EXPECT_TRUE(TIsZeroConstructType<int*>::Value);
}

TEST(TypeTraitsTest, TIsPODType_TrueForPODs)
{
    struct PODStruct
    {
        int x;
        float y;
    };
    EXPECT_TRUE(TIsPODType<int>::Value);
    EXPECT_TRUE(TIsPODType<PODStruct>::Value);
}

TEST(TypeTraitsTest, TIsPODType_FalseForNonPODs)
{
    EXPECT_FALSE(TIsPODType<std::string>::Value);
    EXPECT_FALSE(TIsPODType<std::vector<int>>::Value);
}

TEST(TypeTraitsTest, LogicalCombinators_WorkCorrectly)
{
    // TAnd
    EXPECT_TRUE((TAnd<TIsArithmetic<int>, TIsArithmetic<float>>::Value));
    EXPECT_FALSE((TAnd<TIsArithmetic<int>, TIsPointer<int>>::Value));

    // TOr
    EXPECT_TRUE((TOr<TIsArithmetic<int>, TIsPointer<int>>::Value));
    EXPECT_FALSE((TOr<TIsPointer<int>, TIsPointer<float>>::Value));

    // TNot
    EXPECT_TRUE(TNot<TIsPointer<int>>::Value);
    EXPECT_FALSE(TNot<TIsArithmetic<int>>::Value);
}
