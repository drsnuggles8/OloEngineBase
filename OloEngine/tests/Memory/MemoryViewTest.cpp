/**
 * @file MemoryViewTest.cpp
 * @brief Unit tests for FMemoryView and FMutableMemoryView
 *
 * Ported from UE5.7's Memory/MemoryViewTest.cpp
 * Tests cover: Construction, slicing, comparison, iteration
 */

#include <gtest/gtest.h>

#include "OloEngine/Memory/MemoryView.h"
#include "OloEngine/Core/Base.h"

#include <array>
#include <vector>
#include <cstring>

using namespace OloEngine;

// ============================================================================
// FMemoryView Tests
// ============================================================================

class MemoryViewTest : public ::testing::Test
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(MemoryViewTest, DefaultConstruction)
{
    FMemoryView View;

    EXPECT_EQ(View.GetData(), nullptr);
    EXPECT_EQ(View.GetSize(), 0u);
    EXPECT_TRUE(View.IsEmpty());
}

TEST_F(MemoryViewTest, ConstructFromPointerAndSize)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMemoryView View(Data, sizeof(Data));

    EXPECT_EQ(View.GetData(), Data);
    EXPECT_EQ(View.GetSize(), 5u);
    EXPECT_FALSE(View.IsEmpty());
}

TEST_F(MemoryViewTest, ConstructFromArray)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMemoryView View = MakeMemoryView(Data);

    EXPECT_EQ(View.GetData(), Data);
    EXPECT_EQ(View.GetSize(), 5u);
}

TEST_F(MemoryViewTest, ConstructFromStdArray)
{
    std::array<u8, 5> Data = { 1, 2, 3, 4, 5 };
    FMemoryView View = MakeMemoryView(Data);

    EXPECT_EQ(View.GetData(), Data.data());
    EXPECT_EQ(View.GetSize(), 5u);
}

TEST_F(MemoryViewTest, ConstructFromVector)
{
    std::vector<u8> Data = { 1, 2, 3, 4, 5 };
    FMemoryView View = MakeMemoryView(Data);

    EXPECT_EQ(View.GetData(), Data.data());
    EXPECT_EQ(View.GetSize(), 5u);
}

TEST_F(MemoryViewTest, LeftSlice)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMemoryView View(Data, sizeof(Data));

    FMemoryView Left = View.Left(3);

    EXPECT_EQ(Left.GetData(), Data);
    EXPECT_EQ(Left.GetSize(), 3u);
}

TEST_F(MemoryViewTest, RightSlice)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMemoryView View(Data, sizeof(Data));

    FMemoryView Right = View.Right(3);

    EXPECT_EQ(Right.GetData(), Data + 2);
    EXPECT_EQ(Right.GetSize(), 3u);
}

TEST_F(MemoryViewTest, MidSlice)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMemoryView View(Data, sizeof(Data));

    FMemoryView Mid = View.Mid(1, 3);

    EXPECT_EQ(Mid.GetData(), Data + 1);
    EXPECT_EQ(Mid.GetSize(), 3u);
}

TEST_F(MemoryViewTest, LeftChop)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMemoryView View(Data, sizeof(Data));

    FMemoryView Chopped = View.LeftChop(2);

    EXPECT_EQ(Chopped.GetData(), Data);
    EXPECT_EQ(Chopped.GetSize(), 3u);
}

TEST_F(MemoryViewTest, RightChop)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMemoryView View(Data, sizeof(Data));

    FMemoryView Chopped = View.RightChop(2);

    EXPECT_EQ(Chopped.GetData(), Data + 2);
    EXPECT_EQ(Chopped.GetSize(), 3u);
}

TEST_F(MemoryViewTest, Equality)
{
    u8 Data1[] = { 1, 2, 3, 4, 5 };
    u8 Data2[] = { 1, 2, 3, 4, 5 };
    u8 Data3[] = { 1, 2, 3, 4, 6 };

    FMemoryView View1(Data1, sizeof(Data1));
    FMemoryView View2(Data2, sizeof(Data2));
    FMemoryView View3(Data3, sizeof(Data3));

    EXPECT_TRUE(View1.EqualBytes(View2));
    EXPECT_FALSE(View1.EqualBytes(View3));
}

TEST_F(MemoryViewTest, CompareBytes)
{
    u8 Data1[] = { 1, 2, 3, 4, 5 };
    u8 Data2[] = { 1, 2, 3, 4, 5 };
    u8 Data3[] = { 1, 2, 3, 4, 6 };
    u8 Data4[] = { 1, 2, 3, 4, 4 };

    FMemoryView View1(Data1, sizeof(Data1));
    FMemoryView View2(Data2, sizeof(Data2));
    FMemoryView View3(Data3, sizeof(Data3));
    FMemoryView View4(Data4, sizeof(Data4));

    EXPECT_EQ(View1.CompareBytes(View2), 0);
    EXPECT_LT(View1.CompareBytes(View3), 0);
    EXPECT_GT(View1.CompareBytes(View4), 0);
}

TEST_F(MemoryViewTest, CompareBytesWithDifferentSizes)
{
    u8 Data1[] = { 1, 2, 3 };
    u8 Data2[] = { 1, 2, 3, 4, 5 };

    FMemoryView View1(Data1, sizeof(Data1));
    FMemoryView View2(Data2, sizeof(Data2));

    // Shorter view should be "less than" longer view with same prefix
    EXPECT_LT(View1.CompareBytes(View2), 0);
    EXPECT_GT(View2.CompareBytes(View1), 0);
}

// ============================================================================
// FMutableMemoryView Tests
// ============================================================================

class MutableMemoryViewTest : public ::testing::Test
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(MutableMemoryViewTest, DefaultConstruction)
{
    FMutableMemoryView View;

    EXPECT_EQ(View.GetData(), nullptr);
    EXPECT_EQ(View.GetSize(), 0u);
    EXPECT_TRUE(View.IsEmpty());
}

TEST_F(MutableMemoryViewTest, ConstructFromPointerAndSize)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMutableMemoryView View(Data, sizeof(Data));

    EXPECT_EQ(View.GetData(), Data);
    EXPECT_EQ(View.GetSize(), 5u);
    EXPECT_FALSE(View.IsEmpty());
}

TEST_F(MutableMemoryViewTest, ModifyData)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMutableMemoryView View(Data, sizeof(Data));

    static_cast<u8*>(View.GetData())[2] = 42;

    EXPECT_EQ(Data[2], 42);
}

TEST_F(MutableMemoryViewTest, CopyFrom)
{
    u8 Source[] = { 1, 2, 3, 4, 5 };
    u8 Dest[5] = { 0 };

    FMemoryView SourceView(Source, sizeof(Source));
    FMutableMemoryView DestView(Dest, sizeof(Dest));

    DestView.CopyFrom(SourceView);

    for (u64 i = 0; i < 5; ++i)
    {
        EXPECT_EQ(Dest[i], Source[i]);
    }
}

TEST_F(MutableMemoryViewTest, CopyFromPartial)
{
    u8 Source[] = { 1, 2, 3, 4, 5 };
    u8 Dest[3] = { 0 };

    FMemoryView SourceView(Source, sizeof(Source));
    FMutableMemoryView DestView(Dest, sizeof(Dest));

    // Copy only what fits
    DestView.CopyFrom(SourceView.Left(3));

    EXPECT_EQ(Dest[0], 1);
    EXPECT_EQ(Dest[1], 2);
    EXPECT_EQ(Dest[2], 3);
}

TEST_F(MutableMemoryViewTest, LeftSlice)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMutableMemoryView View(Data, sizeof(Data));

    FMutableMemoryView Left = View.Left(3);

    EXPECT_EQ(Left.GetData(), Data);
    EXPECT_EQ(Left.GetSize(), 3u);

    // Verify mutability
    static_cast<u8*>(Left.GetData())[0] = 42;
    EXPECT_EQ(Data[0], 42);
}

TEST_F(MutableMemoryViewTest, RightSlice)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMutableMemoryView View(Data, sizeof(Data));

    FMutableMemoryView Right = View.Right(3);

    EXPECT_EQ(Right.GetData(), Data + 2);
    EXPECT_EQ(Right.GetSize(), 3u);
}

TEST_F(MutableMemoryViewTest, MidSlice)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMutableMemoryView View(Data, sizeof(Data));

    FMutableMemoryView Mid = View.Mid(1, 3);

    EXPECT_EQ(Mid.GetData(), Data + 1);
    EXPECT_EQ(Mid.GetSize(), 3u);
}

TEST_F(MutableMemoryViewTest, ConversionToImmutable)
{
    u8 Data[] = { 1, 2, 3, 4, 5 };
    FMutableMemoryView MutableView(Data, sizeof(Data));

    FMemoryView ImmutableView = MutableView;

    EXPECT_EQ(ImmutableView.GetData(), Data);
    EXPECT_EQ(ImmutableView.GetSize(), 5u);
}

// ============================================================================
// Memory View Edge Cases
// ============================================================================

class MemoryViewEdgeCasesTest : public ::testing::Test
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(MemoryViewEdgeCasesTest, EmptySlices)
{
    u8 Data[] = { 1, 2, 3 };
    FMemoryView View(Data, sizeof(Data));

    FMemoryView Left0 = View.Left(0);
    EXPECT_TRUE(Left0.IsEmpty());
    EXPECT_EQ(Left0.GetData(), Data);

    FMemoryView Right0 = View.Right(0);
    EXPECT_TRUE(Right0.IsEmpty());
}

TEST_F(MemoryViewEdgeCasesTest, SliceEntireView)
{
    u8 Data[] = { 1, 2, 3 };
    FMemoryView View(Data, sizeof(Data));

    FMemoryView LeftAll = View.Left(3);
    EXPECT_EQ(LeftAll.GetSize(), 3u);

    FMemoryView RightAll = View.Right(3);
    EXPECT_EQ(RightAll.GetSize(), 3u);
}

TEST_F(MemoryViewEdgeCasesTest, EmptyViewOperations)
{
    FMemoryView Empty;

    EXPECT_TRUE(Empty.Left(0).IsEmpty());
    EXPECT_TRUE(Empty.Right(0).IsEmpty());
    EXPECT_TRUE(Empty.Mid(0, 0).IsEmpty());

    // Two empty views should be equal
    FMemoryView Empty2;
    EXPECT_TRUE(Empty.EqualBytes(Empty2));
    EXPECT_EQ(Empty.CompareBytes(Empty2), 0);
}

TEST_F(MemoryViewEdgeCasesTest, SingleByteView)
{
    u8 Data = 42;
    FMemoryView View(&Data, 1);

    EXPECT_EQ(View.GetSize(), 1u);
    EXPECT_FALSE(View.IsEmpty());

    FMemoryView Left = View.Left(1);
    EXPECT_EQ(Left.GetSize(), 1u);
}

TEST_F(MemoryViewEdgeCasesTest, LargeView)
{
    constexpr u64 Size = 1024 * 1024; // 1MB
    std::vector<u8> Data(Size);

    // Fill with pattern
    for (u64 i = 0; i < Size; ++i)
    {
        Data[i] = static_cast<u8>(i & 0xFF);
    }

    FMemoryView View(Data.data(), Data.size());

    EXPECT_EQ(View.GetSize(), Size);

    // Test slicing
    FMemoryView First1K = View.Left(1024);
    FMemoryView Last1K = View.Right(1024);

    EXPECT_EQ(First1K.GetSize(), 1024u);
    EXPECT_EQ(Last1K.GetSize(), 1024u);
}
