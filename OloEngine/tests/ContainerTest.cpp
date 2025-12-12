/**
 * @file ContainerTest.cpp
 * @brief Unit tests for TBitArray, TSparseArray, TSet, TMap containers
 */

#include <gtest/gtest.h>
#include "OloEngine/Containers/BitArray.h"
#include "OloEngine/Containers/SparseArray.h"
#include "OloEngine/Containers/Set.h"
#include "OloEngine/Containers/Map.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include <string>
#include <vector>

using namespace OloEngine;

// ============================================================================
// TBitArray Tests
// ============================================================================

class TBitArrayTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TBitArrayTest, DefaultConstruction)
{
    TBitArray<> BitArray;
    EXPECT_EQ(BitArray.Num(), 0);
    EXPECT_TRUE(BitArray.IsEmpty());
}

TEST_F(TBitArrayTest, InitWithValue)
{
    TBitArray<> BitArray(true, 64);
    EXPECT_EQ(BitArray.Num(), 64);
    EXPECT_FALSE(BitArray.IsEmpty());
    
    // All bits should be true
    for (i32 i = 0; i < 64; ++i)
    {
        EXPECT_TRUE(BitArray[i]) << "Bit " << i << " should be true";
    }
}

TEST_F(TBitArrayTest, AddBits)
{
    TBitArray<> BitArray;
    
    i32 Idx0 = BitArray.Add(true);
    i32 Idx1 = BitArray.Add(false);
    i32 Idx2 = BitArray.Add(true);
    
    EXPECT_EQ(Idx0, 0);
    EXPECT_EQ(Idx1, 1);
    EXPECT_EQ(Idx2, 2);
    EXPECT_EQ(BitArray.Num(), 3);
    
    EXPECT_TRUE(BitArray[0]);
    EXPECT_FALSE(BitArray[1]);
    EXPECT_TRUE(BitArray[2]);
}

TEST_F(TBitArrayTest, SetAndClear)
{
    TBitArray<> BitArray(false, 32);
    
    BitArray[0] = true;
    BitArray[15] = true;
    BitArray[31] = true;
    
    EXPECT_TRUE(BitArray[0]);
    EXPECT_TRUE(BitArray[15]);
    EXPECT_TRUE(BitArray[31]);
    EXPECT_FALSE(BitArray[1]);
    EXPECT_FALSE(BitArray[14]);
    
    BitArray[15] = false;
    EXPECT_FALSE(BitArray[15]);
}

TEST_F(TBitArrayTest, FindFirstSetBit)
{
    TBitArray<> BitArray(false, 64);
    
    // No bits set
    EXPECT_EQ(BitArray.Find(true), INDEX_NONE);
    
    // Set bit 42
    BitArray[42] = true;
    EXPECT_EQ(BitArray.Find(true), 42);
    
    // Set bit 10 (earlier)
    BitArray[10] = true;
    EXPECT_EQ(BitArray.Find(true), 10);
}

TEST_F(TBitArrayTest, FindFirstZeroBit)
{
    TBitArray<> BitArray(true, 32);
    
    // All bits set
    EXPECT_EQ(BitArray.Find(false), INDEX_NONE);
    
    // Clear bit 20
    BitArray[20] = false;
    EXPECT_EQ(BitArray.Find(false), 20);
}

TEST_F(TBitArrayTest, FindAndSetFirstZeroBit)
{
    TBitArray<> BitArray(false, 8);
    
    // Set all bits via FindAndSetFirstZeroBit
    for (i32 i = 0; i < 8; ++i)
    {
        i32 Idx = BitArray.FindAndSetFirstZeroBit();
        EXPECT_EQ(Idx, i);
    }
    
    // All bits set, should return INDEX_NONE
    EXPECT_EQ(BitArray.FindAndSetFirstZeroBit(), INDEX_NONE);
}

TEST_F(TBitArrayTest, SetRange)
{
    TBitArray<> BitArray(false, 64);
    
    BitArray.SetRange(10, 20, true);
    
    for (i32 i = 0; i < 64; ++i)
    {
        if (i >= 10 && i < 30)
        {
            EXPECT_TRUE(BitArray[i]) << "Bit " << i << " should be true";
        }
        else
        {
            EXPECT_FALSE(BitArray[i]) << "Bit " << i << " should be false";
        }
    }
}

TEST_F(TBitArrayTest, RemoveAt)
{
    TBitArray<> BitArray;
    BitArray.Add(true);   // 0
    BitArray.Add(false);  // 1
    BitArray.Add(true);   // 2
    BitArray.Add(false);  // 3
    BitArray.Add(true);   // 4
    
    EXPECT_EQ(BitArray.Num(), 5);
    
    // Remove middle element (index 2)
    BitArray.RemoveAt(2);
    
    EXPECT_EQ(BitArray.Num(), 4);
    EXPECT_TRUE(BitArray[0]);   // was 0
    EXPECT_FALSE(BitArray[1]);  // was 1
    EXPECT_FALSE(BitArray[2]);  // was 3
    EXPECT_TRUE(BitArray[3]);   // was 4
}

TEST_F(TBitArrayTest, CountSetBits)
{
    TBitArray<> BitArray(false, 64);
    
    BitArray[0] = true;
    BitArray[31] = true;
    BitArray[32] = true;
    BitArray[63] = true;
    
    EXPECT_EQ(BitArray.CountSetBits(), 4);
}

TEST_F(TBitArrayTest, Empty)
{
    TBitArray<> BitArray(true, 100);
    EXPECT_EQ(BitArray.Num(), 100);
    
    BitArray.Empty();
    EXPECT_EQ(BitArray.Num(), 0);
    EXPECT_TRUE(BitArray.IsEmpty());
}

// ============================================================================
// TSparseArray Tests
// ============================================================================

class TSparseArrayTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TSparseArrayTest, DefaultConstruction)
{
    TSparseArray<i32> Array;
    EXPECT_EQ(Array.Num(), 0);
    EXPECT_TRUE(Array.IsEmpty());
}

TEST_F(TSparseArrayTest, AddElements)
{
    TSparseArray<i32> Array;
    
    i32 Idx0 = Array.Add(10);
    i32 Idx1 = Array.Add(20);
    i32 Idx2 = Array.Add(30);
    
    EXPECT_EQ(Idx0, 0);
    EXPECT_EQ(Idx1, 1);
    EXPECT_EQ(Idx2, 2);
    EXPECT_EQ(Array.Num(), 3);
    
    EXPECT_EQ(Array[0], 10);
    EXPECT_EQ(Array[1], 20);
    EXPECT_EQ(Array[2], 30);
}

TEST_F(TSparseArrayTest, AddUninitialized)
{
    TSparseArray<i32> Array;
    
    FSparseArrayAllocationInfo Info = Array.AddUninitialized();
    new (Info) i32(42);
    
    EXPECT_EQ(Array.Num(), 1);
    EXPECT_EQ(Array[Info.Index], 42);
}

TEST_F(TSparseArrayTest, RemoveAt)
{
    TSparseArray<i32> Array;
    Array.Add(10);  // 0
    Array.Add(20);  // 1
    Array.Add(30);  // 2
    
    Array.RemoveAt(1);
    
    EXPECT_EQ(Array.Num(), 2);
    EXPECT_TRUE(Array.IsAllocated(0));
    EXPECT_FALSE(Array.IsAllocated(1));
    EXPECT_TRUE(Array.IsAllocated(2));
    
    EXPECT_EQ(Array[0], 10);
    EXPECT_EQ(Array[2], 30);
}

TEST_F(TSparseArrayTest, RemoveAndReuse)
{
    TSparseArray<i32> Array;
    Array.Add(10);  // 0
    Array.Add(20);  // 1
    Array.Add(30);  // 2
    
    Array.RemoveAt(1);
    
    // New element should reuse index 1
    i32 NewIdx = Array.Add(40);
    EXPECT_EQ(NewIdx, 1);
    EXPECT_EQ(Array[1], 40);
    EXPECT_EQ(Array.Num(), 3);
}

TEST_F(TSparseArrayTest, FreeListOrder)
{
    TSparseArray<i32> Array;
    Array.Add(10);  // 0
    Array.Add(20);  // 1
    Array.Add(30);  // 2
    Array.Add(40);  // 3
    
    // Remove in specific order
    Array.RemoveAt(1);
    Array.RemoveAt(3);
    Array.RemoveAt(0);
    
    // Add back - should get indices in reverse order of removal (LIFO)
    i32 Idx1 = Array.Add(50);
    i32 Idx2 = Array.Add(60);
    i32 Idx3 = Array.Add(70);
    
    EXPECT_EQ(Idx1, 0);  // Most recently removed
    EXPECT_EQ(Idx2, 3);
    EXPECT_EQ(Idx3, 1);  // First removed
}

TEST_F(TSparseArrayTest, Iteration)
{
    TSparseArray<i32> Array;
    Array.Add(10);  // 0
    Array.Add(20);  // 1
    Array.Add(30);  // 2
    Array.RemoveAt(1);
    
    std::vector<i32> Values;
    for (const i32& Value : Array)
    {
        Values.push_back(Value);
    }
    
    EXPECT_EQ(static_cast<i32>(Values.size()), 2);
    EXPECT_EQ(Values[0], 10);
    EXPECT_EQ(Values[1], 30);
}

TEST_F(TSparseArrayTest, Reserve)
{
    TSparseArray<i32> Array;
    Array.Reserve(100);
    
    EXPECT_GE(Array.Max(), 100);
    EXPECT_EQ(Array.Num(), 0);
}

TEST_F(TSparseArrayTest, Compact)
{
    TSparseArray<i32> Array;
    Array.Add(10);  // 0
    Array.Add(20);  // 1
    Array.Add(30);  // 2
    Array.RemoveAt(1);
    
    EXPECT_EQ(Array.GetMaxIndex(), 3);
    
    Array.Compact();
    
    EXPECT_EQ(Array.Num(), 2);
    EXPECT_EQ(Array.GetMaxIndex(), 2);
}

TEST_F(TSparseArrayTest, Empty)
{
    TSparseArray<i32> Array;
    Array.Add(10);
    Array.Add(20);
    Array.Add(30);
    
    Array.Empty();
    
    EXPECT_EQ(Array.Num(), 0);
    EXPECT_TRUE(Array.IsEmpty());
}

// ============================================================================
// TSet Tests
// ============================================================================

class TSetTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TSetTest, DefaultConstruction)
{
    TSet<i32> Set;
    EXPECT_EQ(Set.Num(), 0);
    EXPECT_TRUE(Set.IsEmpty());
}

TEST_F(TSetTest, AddElements)
{
    TSet<i32> Set;
    
    Set.Add(10);
    Set.Add(20);
    Set.Add(30);
    
    EXPECT_EQ(Set.Num(), 3);
    EXPECT_TRUE(Set.Contains(10));
    EXPECT_TRUE(Set.Contains(20));
    EXPECT_TRUE(Set.Contains(30));
    EXPECT_FALSE(Set.Contains(40));
}

TEST_F(TSetTest, AddDuplicate)
{
    TSet<i32> Set;
    
    bool bIsAlreadyInSet = false;
    Set.Add(10, &bIsAlreadyInSet);
    EXPECT_FALSE(bIsAlreadyInSet);
    
    Set.Add(10, &bIsAlreadyInSet);
    EXPECT_TRUE(bIsAlreadyInSet);
    
    EXPECT_EQ(Set.Num(), 1);
}

TEST_F(TSetTest, Find)
{
    TSet<i32> Set;
    Set.Add(10);
    Set.Add(20);
    Set.Add(30);
    
    i32* Found = Set.Find(20);
    EXPECT_NE(Found, nullptr);
    EXPECT_EQ(*Found, 20);
    
    i32* NotFound = Set.Find(40);
    EXPECT_EQ(NotFound, nullptr);
}

TEST_F(TSetTest, FindOrAdd)
{
    TSet<i32> Set;
    
    // Add via FindOrAdd
    bool bIsAlreadyInSet = false;
    i32& Val1 = Set.FindOrAdd(10, &bIsAlreadyInSet);
    EXPECT_FALSE(bIsAlreadyInSet);
    EXPECT_EQ(Val1, 10);
    
    // Find existing
    i32& Val2 = Set.FindOrAdd(10, &bIsAlreadyInSet);
    EXPECT_TRUE(bIsAlreadyInSet);
    EXPECT_EQ(&Val1, &Val2);  // Same reference
}

TEST_F(TSetTest, Remove)
{
    TSet<i32> Set;
    Set.Add(10);
    Set.Add(20);
    Set.Add(30);
    
    i32 NumRemoved = Set.Remove(20);
    EXPECT_EQ(NumRemoved, 1);
    EXPECT_EQ(Set.Num(), 2);
    EXPECT_FALSE(Set.Contains(20));
    EXPECT_TRUE(Set.Contains(10));
    EXPECT_TRUE(Set.Contains(30));
}

TEST_F(TSetTest, RemoveNonExistent)
{
    TSet<i32> Set;
    Set.Add(10);
    
    i32 NumRemoved = Set.Remove(999);
    EXPECT_EQ(NumRemoved, 0);
    EXPECT_EQ(Set.Num(), 1);
}

TEST_F(TSetTest, Iteration)
{
    TSet<i32> Set;
    Set.Add(10);
    Set.Add(20);
    Set.Add(30);
    
    std::vector<i32> Values;
    for (const i32& Value : Set)
    {
        Values.push_back(Value);
    }
    
    EXPECT_EQ(static_cast<i32>(Values.size()), 3);
    // Order preserved
    EXPECT_EQ(Values[0], 10);
    EXPECT_EQ(Values[1], 20);
    EXPECT_EQ(Values[2], 30);
}

TEST_F(TSetTest, InitializerList)
{
    TSet<i32> Set = { 1, 2, 3, 4, 5 };
    
    EXPECT_EQ(Set.Num(), 5);
    for (i32 i = 1; i <= 5; ++i)
    {
        EXPECT_TRUE(Set.Contains(i));
    }
}

TEST_F(TSetTest, CopyConstruction)
{
    TSet<i32> Set1 = { 10, 20, 30 };
    TSet<i32> Set2(Set1);
    
    EXPECT_EQ(Set2.Num(), 3);
    EXPECT_TRUE(Set2.Contains(10));
    EXPECT_TRUE(Set2.Contains(20));
    EXPECT_TRUE(Set2.Contains(30));
    
    // Modify original, copy should be unchanged
    Set1.Add(40);
    EXPECT_FALSE(Set2.Contains(40));
}

TEST_F(TSetTest, MoveConstruction)
{
    TSet<i32> Set1 = { 10, 20, 30 };
    TSet<i32> Set2(MoveTemp(Set1));
    
    EXPECT_EQ(Set2.Num(), 3);
    EXPECT_TRUE(Set2.Contains(10));
    EXPECT_TRUE(Set2.Contains(20));
    EXPECT_TRUE(Set2.Contains(30));
}

TEST_F(TSetTest, UnionOperation)
{
    TSet<i32> Set1 = { 1, 2, 3 };
    TSet<i32> Set2 = { 3, 4, 5 };
    
    TSet<i32> Union = Set1.Union(Set2);
    
    EXPECT_EQ(Union.Num(), 5);
    for (i32 i = 1; i <= 5; ++i)
    {
        EXPECT_TRUE(Union.Contains(i));
    }
}

TEST_F(TSetTest, IntersectOperation)
{
    TSet<i32> Set1 = { 1, 2, 3, 4 };
    TSet<i32> Set2 = { 3, 4, 5, 6 };
    
    TSet<i32> Intersect = Set1.Intersect(Set2);
    
    EXPECT_EQ(Intersect.Num(), 2);
    EXPECT_TRUE(Intersect.Contains(3));
    EXPECT_TRUE(Intersect.Contains(4));
}

TEST_F(TSetTest, DifferenceOperation)
{
    TSet<i32> Set1 = { 1, 2, 3, 4 };
    TSet<i32> Set2 = { 3, 4, 5, 6 };
    
    TSet<i32> Diff = Set1.Difference(Set2);
    
    EXPECT_EQ(Diff.Num(), 2);
    EXPECT_TRUE(Diff.Contains(1));
    EXPECT_TRUE(Diff.Contains(2));
}

TEST_F(TSetTest, LegacyComparison)
{
    TSet<i32> Set1 = { 1, 2, 3 };
    TSet<i32> Set2 = { 1, 2, 3 };
    TSet<i32> Set3 = { 1, 2, 4 };
    
    // Use LegacyCompareEqual since operator== is deleted
    EXPECT_TRUE(LegacyCompareEqual(Set1, Set2));
    EXPECT_FALSE(LegacyCompareEqual(Set1, Set3));
    EXPECT_TRUE(LegacyCompareNotEqual(Set1, Set3));
}

TEST_F(TSetTest, Empty)
{
    TSet<i32> Set = { 1, 2, 3, 4, 5 };
    EXPECT_EQ(Set.Num(), 5);
    
    Set.Empty();
    EXPECT_EQ(Set.Num(), 0);
    EXPECT_TRUE(Set.IsEmpty());
}

TEST_F(TSetTest, Reserve)
{
    TSet<i32> Set;
    Set.Reserve(100);
    
    for (i32 i = 0; i < 100; ++i)
    {
        Set.Add(i);
    }
    
    EXPECT_EQ(Set.Num(), 100);
}

TEST_F(TSetTest, Compact)
{
    TSet<i32> Set;
    for (i32 i = 0; i < 10; ++i)
    {
        Set.Add(i);
    }
    
    // Remove every other element
    for (i32 i = 0; i < 10; i += 2)
    {
        Set.Remove(i);
    }
    
    Set.Compact();
    
    EXPECT_EQ(Set.Num(), 5);
    EXPECT_EQ(Set.GetMaxIndex(), 5);
}

// ============================================================================
// TMap Tests
// ============================================================================

class TMapTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TMapTest, DefaultConstruction)
{
    TMap<i32, std::string> Map;
    EXPECT_EQ(Map.Num(), 0);
    EXPECT_TRUE(Map.IsEmpty());
}

TEST_F(TMapTest, AddElements)
{
    TMap<i32, std::string> Map;
    
    Map.Add(1, "one");
    Map.Add(2, "two");
    Map.Add(3, "three");
    
    EXPECT_EQ(Map.Num(), 3);
    EXPECT_TRUE(Map.Contains(1));
    EXPECT_TRUE(Map.Contains(2));
    EXPECT_TRUE(Map.Contains(3));
    EXPECT_FALSE(Map.Contains(4));
}

TEST_F(TMapTest, AddReplaceExisting)
{
    TMap<i32, std::string> Map;
    
    Map.Add(1, "one");
    Map.Add(1, "ONE");  // Should replace
    
    EXPECT_EQ(Map.Num(), 1);
    EXPECT_EQ(*Map.Find(1), "ONE");
}

TEST_F(TMapTest, Find)
{
    TMap<i32, std::string> Map;
    Map.Add(1, "one");
    Map.Add(2, "two");
    
    std::string* Found = Map.Find(2);
    EXPECT_NE(Found, nullptr);
    EXPECT_EQ(*Found, "two");
    
    std::string* NotFound = Map.Find(999);
    EXPECT_EQ(NotFound, nullptr);
}

TEST_F(TMapTest, FindOrAdd)
{
    TMap<i32, std::string> Map;
    
    // Add via FindOrAdd
    std::string& Val1 = Map.FindOrAdd(1);
    Val1 = "one";
    EXPECT_EQ(Map.Num(), 1);
    EXPECT_EQ(*Map.Find(1), "one");
    
    // Find existing
    std::string& Val2 = Map.FindOrAdd(1);
    EXPECT_EQ(&Val1, &Val2);
    EXPECT_EQ(Val2, "one");
}

TEST_F(TMapTest, FindOrAddWithValue)
{
    TMap<i32, std::string> Map;
    
    std::string& Val1 = Map.FindOrAdd(1, "one");
    EXPECT_EQ(Val1, "one");
    
    // Should return existing, not replace
    std::string& Val2 = Map.FindOrAdd(1, "ONE");
    EXPECT_EQ(Val2, "one");
}

TEST_F(TMapTest, FindChecked)
{
    TMap<i32, std::string> Map;
    Map.Add(1, "one");
    
    std::string& Value = Map.FindChecked(1);
    EXPECT_EQ(Value, "one");
}

TEST_F(TMapTest, FindRef)
{
    TMap<i32, std::string> Map;
    Map.Add(1, "one");
    
    EXPECT_EQ(Map.FindRef(1), "one");
    EXPECT_EQ(Map.FindRef(999), "");  // Default constructed
    EXPECT_EQ(Map.FindRef(999, "default"), "default");
}

TEST_F(TMapTest, FindKey)
{
    TMap<i32, std::string> Map;
    Map.Add(1, "one");
    Map.Add(2, "two");
    
    const i32* Key = Map.FindKey("two");
    EXPECT_NE(Key, nullptr);
    EXPECT_EQ(*Key, 2);
    
    const i32* NotFound = Map.FindKey("three");
    EXPECT_EQ(NotFound, nullptr);
}

TEST_F(TMapTest, Remove)
{
    TMap<i32, std::string> Map;
    Map.Add(1, "one");
    Map.Add(2, "two");
    Map.Add(3, "three");
    
    i32 NumRemoved = Map.Remove(2);
    EXPECT_EQ(NumRemoved, 1);
    EXPECT_EQ(Map.Num(), 2);
    EXPECT_FALSE(Map.Contains(2));
}

TEST_F(TMapTest, RemoveAndCopyValue)
{
    TMap<i32, std::string> Map;
    Map.Add(1, "one");
    Map.Add(2, "two");
    
    std::string RemovedValue;
    bool bRemoved = Map.RemoveAndCopyValue(2, RemovedValue);
    
    EXPECT_TRUE(bRemoved);
    EXPECT_EQ(RemovedValue, "two");
    EXPECT_EQ(Map.Num(), 1);
    EXPECT_FALSE(Map.Contains(2));
}

TEST_F(TMapTest, SubscriptOperator)
{
    TMap<i32, std::string> Map;
    Map.Add(1, "one");
    Map.Add(2, "two");
    
    EXPECT_EQ(Map[1], "one");
    EXPECT_EQ(Map[2], "two");
}

TEST_F(TMapTest, Iteration)
{
    TMap<i32, std::string> Map;
    Map.Add(1, "one");
    Map.Add(2, "two");
    Map.Add(3, "three");
    
    std::vector<std::pair<i32, std::string>> Pairs;
    for (const auto& Pair : Map)
    {
        Pairs.emplace_back(Pair.Key, Pair.Value);
    }
    
    EXPECT_EQ(static_cast<i32>(Pairs.size()), 3);
    EXPECT_EQ(Pairs[0].first, 1);
    EXPECT_EQ(Pairs[0].second, "one");
    EXPECT_EQ(Pairs[1].first, 2);
    EXPECT_EQ(Pairs[1].second, "two");
    EXPECT_EQ(Pairs[2].first, 3);
    EXPECT_EQ(Pairs[2].second, "three");
}

TEST_F(TMapTest, IteratorKeyValue)
{
    TMap<i32, std::string> Map;
    Map.Add(1, "one");
    Map.Add(2, "two");
    
    i32 Sum = 0;
    for (auto It = Map.CreateConstIterator(); It; ++It)
    {
        Sum += It.Key();
    }
    
    EXPECT_EQ(Sum, 3);
}

TEST_F(TMapTest, CopyConstruction)
{
    TMap<i32, std::string> Map1;
    Map1.Add(1, "one");
    Map1.Add(2, "two");
    
    TMap<i32, std::string> Map2(Map1);
    
    EXPECT_EQ(Map2.Num(), 2);
    EXPECT_EQ(*Map2.Find(1), "one");
    EXPECT_EQ(*Map2.Find(2), "two");
    
    // Modify original, copy unchanged
    Map1.Add(3, "three");
    EXPECT_FALSE(Map2.Contains(3));
}

TEST_F(TMapTest, MoveConstruction)
{
    TMap<i32, std::string> Map1;
    Map1.Add(1, "one");
    Map1.Add(2, "two");
    
    TMap<i32, std::string> Map2(MoveTemp(Map1));
    
    EXPECT_EQ(Map2.Num(), 2);
    EXPECT_EQ(*Map2.Find(1), "one");
    EXPECT_EQ(*Map2.Find(2), "two");
}

TEST_F(TMapTest, Append)
{
    TMap<i32, std::string> Map1;
    Map1.Add(1, "one");
    Map1.Add(2, "two");
    
    TMap<i32, std::string> Map2;
    Map2.Add(3, "three");
    Map2.Add(4, "four");
    
    Map1.Append(Map2);
    
    EXPECT_EQ(Map1.Num(), 4);
    EXPECT_TRUE(Map1.Contains(1));
    EXPECT_TRUE(Map1.Contains(2));
    EXPECT_TRUE(Map1.Contains(3));
    EXPECT_TRUE(Map1.Contains(4));
}

TEST_F(TMapTest, Empty)
{
    TMap<i32, std::string> Map;
    Map.Add(1, "one");
    Map.Add(2, "two");
    
    Map.Empty();
    
    EXPECT_EQ(Map.Num(), 0);
    EXPECT_TRUE(Map.IsEmpty());
}

TEST_F(TMapTest, Reserve)
{
    TMap<i32, std::string> Map;
    Map.Reserve(100);
    
    for (i32 i = 0; i < 100; ++i)
    {
        Map.Add(i, std::to_string(i));
    }
    
    EXPECT_EQ(Map.Num(), 100);
}

TEST_F(TMapTest, EqualityOperator)
{
    TMap<i32, std::string> Map1;
    Map1.Add(1, "one");
    Map1.Add(2, "two");
    
    TMap<i32, std::string> Map2;
    Map2.Add(1, "one");
    Map2.Add(2, "two");
    
    TMap<i32, std::string> Map3;
    Map3.Add(1, "one");
    Map3.Add(2, "TWO");
    
    EXPECT_TRUE(Map1 == Map2);
    EXPECT_FALSE(Map1 == Map3);
    EXPECT_TRUE(Map1 != Map3);
}

// ============================================================================
// TMultiMap Tests
// ============================================================================

class TMultiMapTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TMultiMapTest, DefaultConstruction)
{
    TMultiMap<i32, std::string> Map;
    EXPECT_EQ(Map.Num(), 0);
    EXPECT_TRUE(Map.IsEmpty());
}

TEST_F(TMultiMapTest, AddDuplicateKeys)
{
    TMultiMap<i32, std::string> Map;
    
    Map.Add(1, "one");
    Map.Add(1, "ONE");
    Map.Add(1, "1");
    Map.Add(2, "two");
    
    EXPECT_EQ(Map.Num(), 4);
}

TEST_F(TMultiMapTest, NumForKey)
{
    TMultiMap<i32, std::string> Map;
    
    Map.Add(1, "one");
    Map.Add(1, "ONE");
    Map.Add(1, "1");
    Map.Add(2, "two");
    
    EXPECT_EQ(Map.Num(1), 3);
    EXPECT_EQ(Map.Num(2), 1);
    EXPECT_EQ(Map.Num(999), 0);
}

TEST_F(TMultiMapTest, RemoveSingle)
{
    TMultiMap<i32, std::string> Map;
    
    Map.Add(1, "one");
    Map.Add(1, "ONE");
    Map.Add(1, "1");
    
    i32 NumRemoved = Map.RemoveSingle(1, "ONE");
    
    EXPECT_EQ(NumRemoved, 1);
    EXPECT_EQ(Map.Num(), 2);
}

// ============================================================================
// Stress Tests
// ============================================================================

TEST(ContainerStressTest, TSetManyElements)
{
    TSet<i32> Set;
    const i32 NumElements = 10000;
    
    for (i32 i = 0; i < NumElements; ++i)
    {
        Set.Add(i);
    }
    
    EXPECT_EQ(Set.Num(), NumElements);
    
    for (i32 i = 0; i < NumElements; ++i)
    {
        EXPECT_TRUE(Set.Contains(i));
    }
    
    // Remove half
    for (i32 i = 0; i < NumElements; i += 2)
    {
        Set.Remove(i);
    }
    
    EXPECT_EQ(Set.Num(), NumElements / 2);
}

TEST(ContainerStressTest, TMapManyElements)
{
    TMap<i32, i32> Map;
    const i32 NumElements = 10000;
    
    for (i32 i = 0; i < NumElements; ++i)
    {
        Map.Add(i, i * 2);
    }
    
    EXPECT_EQ(Map.Num(), NumElements);
    
    for (i32 i = 0; i < NumElements; ++i)
    {
        i32* Value = Map.Find(i);
        EXPECT_NE(Value, nullptr);
        EXPECT_EQ(*Value, i * 2);
    }
}

TEST(ContainerStressTest, TSparseArrayManyAddRemove)
{
    TSparseArray<i32> Array;
    const i32 NumIterations = 1000;
    
    // Add many elements
    for (i32 i = 0; i < NumIterations; ++i)
    {
        Array.Add(i);
    }
    
    EXPECT_EQ(Array.Num(), NumIterations);
    
    // Remove all odd indices
    for (i32 i = 1; i < NumIterations; i += 2)
    {
        Array.RemoveAt(i);
    }
    
    EXPECT_EQ(Array.Num(), NumIterations / 2);
    
    // Re-add
    for (i32 i = 0; i < NumIterations / 2; ++i)
    {
        Array.Add(i + NumIterations);
    }
    
    EXPECT_EQ(Array.Num(), NumIterations);
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
    
    i32 RawData[] = {200, 300, 400, 500};
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
    
    const i32 RawData[] = {20, 30, 40};
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
    
    i32 CArr[] = {3, 4, 5};
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
    
    i32 RawData[] = {20, 30, 40};
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
