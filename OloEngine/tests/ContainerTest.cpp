// @brief Unit tests for TBitArray, TSparseArray, TSet, TMap containers

#include <gtest/gtest.h>
#include "OloEngine/Containers/BitArray.h"
#include "OloEngine/Containers/SparseArray.h"
#include "OloEngine/Containers/Set.h"
#include "OloEngine/Containers/Map.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include <string>

// ============================================================================
// ContainerTest -- wire-up smoke for the UE-ported container primitives,
// plus integration tests for our OloEngine glue (TArray::Append from view,
// type traits, ElementType deduction).
//
// The previous version of this file had 73 TEST_F cases -- one per
// container x per method (Add / Remove / Find / Contains / iteration /
// copy-construct / move-construct / equality operator / etc.). Per
// docs/testing.md section 4.8 (vendor port re-test), the
// UE-ported containers (TBitArray, TSparseArray, TSet, TMap, TMultiMap)
// have upstream test coverage; re-proving their contract here adds no
// defence because we don't edit the ported code in ways that would
// regress them. The smoke tests below verify the headers compile + the
// constructor/Num path is plausible. Real coverage of OloEngine's
// container layer lives further down (TArrayTest::GenericAppend* and
// the type-trait checks), where we DO own the code paths.
// ============================================================================

using namespace OloEngine;

TEST(ContainerSmoke, TBitArrayBasicInitAndIndexing)
{
    TBitArray<> bits;
    EXPECT_EQ(bits.Num(), 0);
    bits.Init(true, 16);
    EXPECT_EQ(bits.Num(), 16);
    bits[5] = false;
    EXPECT_FALSE(bits[5]);
    EXPECT_TRUE(bits[0]);
}

TEST(ContainerSmoke, TSparseArrayAddAndIterate)
{
    TSparseArray<i32> arr;
    arr.Add(10);
    arr.Add(20);
    arr.Add(30);
    EXPECT_EQ(arr.Num(), 3);
    i32 sum = 0;
    for (auto it = arr.CreateIterator(); it; ++it) sum += *it;
    EXPECT_EQ(sum, 60);
}

TEST(ContainerSmoke, TSetAddDuplicateAndContains)
{
    TSet<i32> set;
    set.Add(1);
    set.Add(2);
    set.Add(1); // duplicate
    EXPECT_EQ(set.Num(), 2);
    EXPECT_TRUE(set.Contains(1));
    EXPECT_FALSE(set.Contains(99));
}

TEST(ContainerSmoke, TMapAddFindRemove)
{
    TMap<i32, std::string> map;
    map.Add(1, "one");
    map.Add(2, "two");
    auto* found = map.Find(1);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, "one");
    map.Remove(1);
    EXPECT_EQ(map.Find(1), nullptr);
    EXPECT_EQ(map.Num(), 1);
}

TEST(ContainerSmoke, TMultiMapAddDuplicateKeys)
{
    TMultiMap<i32, i32> map;
    map.Add(1, 10);
    map.Add(1, 20);
    map.Add(2, 30);
    EXPECT_EQ(map.Num(), 3);
    EXPECT_EQ(map.Num(1), 2);
}

// ============================================================================
// TArray Tests (including generic Append)
// ============================================================================

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/ArrayView.h"
#include <array>

TEST(TArrayTest, GenericAppendFromTArrayView)
{
    TArray<i32> Array;
    Array.Add(100);

    i32 RawData[] = { 200, 300, 400, 500 };
    TArrayView<i32> View(RawData, 4);
    Array.Append(View);

    EXPECT_EQ(Array.Num(), 5);
    EXPECT_EQ(Array[0], 100);
    EXPECT_EQ(Array[1], 200);
    EXPECT_EQ(Array[2], 300);
    EXPECT_EQ(Array[3], 400);
    EXPECT_EQ(Array[4], 500);
}

TEST(TArrayTest, GenericAppendFromConstTArrayView)
{
    TArray<i32> Array;
    Array.Add(10);

    const i32 RawData[] = { 20, 30, 40 };
    TConstArrayView<i32> View(RawData, 3);
    Array.Append(View);

    EXPECT_EQ(Array.Num(), 4);
    EXPECT_EQ(Array[0], 10);
    EXPECT_EQ(Array[1], 20);
    EXPECT_EQ(Array[2], 30);
    EXPECT_EQ(Array[3], 40);
}

TEST(TArrayTest, GenericAppendFromCArray)
{
    TArray<i32> Array;
    Array.Add(1);
    Array.Add(2);

    i32 CArr[] = { 3, 4, 5 };
    Array.Append(CArr);

    EXPECT_EQ(Array.Num(), 5);
    EXPECT_EQ(Array[0], 1);
    EXPECT_EQ(Array[1], 2);
    EXPECT_EQ(Array[2], 3);
    EXPECT_EQ(Array[3], 4);
    EXPECT_EQ(Array[4], 5);
}

TEST(TArrayTest, GenericAppendEmptyView)
{
    TArray<i32> Array;
    Array.Add(1);

    TArrayView<i32> EmptyView;
    Array.Append(EmptyView);

    EXPECT_EQ(Array.Num(), 1);
    EXPECT_EQ(Array[0], 1);
}

TEST(TArrayTest, AppendFromRawPointer)
{
    TArray<i32> Array;
    Array.Add(10);

    i32 RawData[] = { 20, 30, 40 };
    Array.Append(RawData, 3);

    EXPECT_EQ(Array.Num(), 4);
    EXPECT_EQ(Array[0], 10);
    EXPECT_EQ(Array[1], 20);
    EXPECT_EQ(Array[2], 30);
    EXPECT_EQ(Array[3], 40);
}

TEST(TArrayTest, TIsTArrayOrDerivedFromTArray)
{
    // TArray should be detected
    EXPECT_TRUE((Private::TIsTArrayOrDerivedFromTArray_V<TArray<i32>>));
    EXPECT_TRUE((Private::TIsTArrayOrDerivedFromTArray_V<const TArray<i32>>));

    // Non-TArray containers should not be detected
    EXPECT_FALSE((Private::TIsTArrayOrDerivedFromTArray_V<TArrayView<i32>>));
    EXPECT_FALSE((Private::TIsTArrayOrDerivedFromTArray_V<i32>));
}

TEST(TArrayTest, TArrayElementsAreCompatible)
{
    // Same type
    EXPECT_TRUE((Private::TArrayElementsAreCompatible_V<i32, i32>));

    // Constructible types
    EXPECT_TRUE((Private::TArrayElementsAreCompatible_V<f64, i32>));

    // Incompatible types
    EXPECT_FALSE((Private::TArrayElementsAreCompatible_V<std::string, i32>));
}

TEST(TArrayTest, TElementTypeWorks)
{
    // UE-style containers with ElementType
    EXPECT_TRUE((std::is_same_v<TElementType_T<TArray<i32>>, i32>));
    EXPECT_TRUE((std::is_same_v<TElementType_T<TArrayView<f64>>, f64>));

    // C arrays
    EXPECT_TRUE((std::is_same_v<TElementType_T<i32[5]>, i32>));

    // initializer_list
    EXPECT_TRUE((std::is_same_v<TElementType_T<std::initializer_list<i32>>, i32>));

    // STL containers with value_type
    EXPECT_TRUE((std::is_same_v<TElementType_T<std::vector<i32>>, i32>));
    EXPECT_TRUE((std::is_same_v<TElementType_T<std::array<f32, 3>>, f32>));
}
