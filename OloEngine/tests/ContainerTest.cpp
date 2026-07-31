// @brief Unit tests for TBitArray, TSparseArray, TSet, TMap containers

#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Containers/BitArray.h"
#include "OloEngine/Containers/SparseArray.h"
#include "OloEngine/Containers/Set.h"
#include "OloEngine/Containers/Map.h"
#include "OloEngine/Containers/String.h"
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
    for (auto it = arr.CreateIterator(); it; ++it)
        sum += *it;
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
    // FString, not std::string. TMap stores its pairs in a TSparseArray, which
    // relocates bitwise, and libstdc++'s std::string does not survive that (see
    // the TMapRelocation cases below). The relocatability guard flags it, and
    // the guard is right -- a std::string value here was always unsound, it
    // just happened not to grow enough in this smoke test to corrupt.
    TMap<i32, FString> map;
    map.Add(1, FString("one"));
    map.Add(2, FString("two"));
    auto* found = map.Find(1);
    ASSERT_NE(found, nullptr);
    EXPECT_TRUE(found->Equals(FString("one")));
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

// ============================================================================
// TArray regression tests
//
// Both of these were live defects found while giving Submesh a relocatable
// string type; neither is string-specific.
// ============================================================================

// The copy constructor called CopyToEmpty() on an array whose m_ArrayNum and
// m_ArrayMax were still indeterminate, and CopyToEmpty routed through
// ResizeAllocation(), which short-circuits on `if (NewMax != m_ArrayMax)`.
// Whenever the uninitialised garbage happened to equal the computed NewMax the
// allocation was SKIPPED, leaving GetData() null for the ConstructItems memcpy
// that immediately followed -- a segfault writing to address 0.
//
// Small element types are the ones that expose it: their quantised capacities
// are small numbers that plausibly appear in recycled stack memory, whereas
// this engine's larger element types produce values that rarely collide. Hence
// char here.
TEST(TArrayRegression, CopyConstructAllocatesForSmallElementTypes)
{
    using namespace OloEngine;

    // Repeat: the bug depends on the garbage in the destination's storage, so
    // a single copy can pass by luck. Reusing the same stack region across
    // iterations is what makes this reliable.
    for (i32 iteration = 0; iteration < 256; ++iteration)
    {
        TArray<char, TSizedDefaultAllocator<32>> source;
        for (char c : std::string_view("Cube"))
            source.Add(c);
        source.Add('\0');

        TArray<char, TSizedDefaultAllocator<32>> copy(source);

        ASSERT_NE(copy.GetData(), nullptr) << "copy ctor skipped its allocation on iteration " << iteration;
        ASSERT_EQ(copy.Num(), source.Num());
        EXPECT_STREQ(copy.GetData(), "Cube");
    }
}

// Copy-assignment passed m_ArrayMax as CopyToEmpty's third argument, which this
// port treats as ExtraSlack (`NewMax = Count + ExtraSlack`) rather than UE's
// PrevMax. Every assignment therefore requested Count + current capacity, so
// capacity grew without bound across repeated assignments to the same array.
TEST(TArrayRegression, CopyAssignmentDoesNotGrowCapacityWithoutBound)
{
    using namespace OloEngine;

    TArray<i32, TSizedDefaultAllocator<32>> source;
    for (i32 i = 0; i < 8; ++i)
        source.Add(i);

    TArray<i32, TSizedDefaultAllocator<32>> target;
    target = source;
    const auto capacityAfterFirst = target.Max();

    // Assign the same contents many times. Capacity must settle, not compound.
    for (i32 i = 0; i < 64; ++i)
        target = source;

    EXPECT_EQ(target.Num(), source.Num());
    EXPECT_EQ(target.Max(), capacityAfterFirst)
        << "capacity grew across repeated assignment (" << capacityAfterFirst << " -> " << target.Max() << ")";

    for (i32 i = 0; i < 8; ++i)
        EXPECT_EQ(target[i], i);
}

// ============================================================================
// TMap element relocation — a controlled three-case matrix.
//
// Material.h keeps a dozen uniform tables as TMap<std::string, T>. TMap is
// built on TSet -> TSparseArray -> TArray, and TArray relocates bitwise. The
// question is whether that relocation reaches the element payload and corrupts
// std::string keys the way it corrupted TArray<Submesh>.
//
// Three cases isolate the mechanism:
//   1. std::string keys, too few to grow   -> must pass (no relocation)
//   2. std::string keys, many (grows)      -> corrupts (DISABLED, see below)
//   3. FString keys, many (grows)          -> must pass (relocatable)
//
// Short keys throughout: only those live in std::string's SSO inline buffer and
// carry the self-referential pointer. Long keys point at separate heap blocks
// and survive relocation regardless, so a test using them proves nothing.
// ============================================================================

// Case 1 — control. Few enough entries that the backing storage never grows,
// so no element is ever relocated. If this failed, the problem would be
// something other than relocation.
TEST(TMapRelocation, StdStringKeysIntactWithoutGrowth)
{
    using namespace OloEngine;

    TMap<std::string, f32> uniforms;
    constexpr i32 kFew = 4;

    for (i32 i = 0; i < kFew; ++i)
        uniforms.Add("u" + std::to_string(i), static_cast<f32>(i));

    ASSERT_EQ(uniforms.Num(), kFew);
    for (i32 i = 0; i < kFew; ++i)
    {
        const f32* found = uniforms.Find("u" + std::to_string(i));
        ASSERT_NE(found, nullptr) << "key lost WITHOUT any growth — not a relocation problem";
        EXPECT_FLOAT_EQ(*found, static_cast<f32>(i));
    }
}

// Case 2 — the defect. DISABLED because it fails by design: std::string is not
// trivially relocatable, so it must not be used as a TMap key at all. Kept as
// executable documentation of why, and as the reproduction if anyone doubts it.
//
// Observed: keys u0..u3 found intact, u4 lost, then the run hung walking the
// corrupted map. Partial, scattered loss is the signature of relocation damage
// — a logic error in Add/Find would fail uniformly, not from the fifth key on.
//
// Run with --gtest_also_run_disabled_tests to see it fail.
TEST(TMapRelocation, DISABLED_StdStringKeysCorruptAcrossGrowth)
{
    using namespace OloEngine;

    TMap<std::string, f32> uniforms;
    constexpr i32 kCount = 512;

    for (i32 i = 0; i < kCount; ++i)
        uniforms.Add("u" + std::to_string(i), static_cast<f32>(i));

    for (i32 i = 0; i < kCount; ++i)
    {
        const std::string key = "u" + std::to_string(i);
        const f32* found = uniforms.Find(key);
        ASSERT_NE(found, nullptr) << "key '" << key << "' lost across TMap growth";
        EXPECT_FLOAT_EQ(*found, static_cast<f32>(i));
    }
}

// Case 3 — the fix. Same shape as case 2 but with a relocatable key type.
// This is what Material's uniform tables must use.
TEST(TMapRelocation, FStringKeysSurviveGrowth)
{
    using namespace OloEngine;

    TMap<FString, f32> uniforms;
    constexpr i32 kCount = 512;

    for (i32 i = 0; i < kCount; ++i)
        uniforms.Add(FString(("u" + std::to_string(i)).c_str()), static_cast<f32>(i));

    ASSERT_EQ(uniforms.Num(), kCount);
    for (i32 i = 0; i < kCount; ++i)
    {
        const FString key(("u" + std::to_string(i)).c_str());
        const f32* found = uniforms.Find(key);
        ASSERT_NE(found, nullptr) << "key '" << *key << "' lost across TMap growth";
        EXPECT_FLOAT_EQ(*found, static_cast<f32>(i));
    }
}

// Diagnostic: does the relocatability trait actually propagate through the
// types TMap is built from? The guard in ~TCompactSet checks the set's element
// type, which for TMap is TPair<Key, Value>.
TEST(TMapRelocation, TraitPropagatesThroughPair)
{
    using namespace OloEngine;
    EXPECT_FALSE(TIsTriviallyRelocatable_V<std::string>) << "std::string must be non-relocatable";
    EXPECT_FALSE((TIsTriviallyRelocatable_V<TPair<std::string, f32>>))
        << "TPair must inherit non-relocatability from its key -- if this is TRUE, "
           "the ~TCompactSet guard can never catch TMap<std::string, T>";
    EXPECT_TRUE((TIsTriviallyRelocatable_V<TPair<FString, f32>>)) << "FString pair should be relocatable";
}
