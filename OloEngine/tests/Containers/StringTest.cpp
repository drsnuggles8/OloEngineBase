// OLO_TEST_LAYER: unit
//
// FString — the trivially-relocatable string ported from Unreal Engine.
//
// The headline test here is StoredInTArraySurvivesReallocation. FString exists
// because TArray relocates elements BITWISE (ResizeGrow -> ResizeAllocation ->
// FMemory::Realloc on the raw byte buffer), and libstdc++'s std::string cannot
// survive that: under SSO its internal pointer points into its own inline
// buffer, so relocating leaves that pointer aimed at the element's old address
// and the destructor frees a non-heap pointer —
//
//     free(): invalid pointer
//
// — which is exactly how AssetSceneLoad aborted on the Linux GPU runner via
// ~TArray<Submesh>. FString is a single TArray<char> member with no SSO, so its
// pointer always targets a separate heap block and byte-copying it is safe.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"

#include <string>
#include <string_view>

namespace OloEngine::Tests
{
    namespace
    {
        TEST(FStringTest, DefaultConstructedIsEmpty)
        {
            const FString s;
            EXPECT_TRUE(s.IsEmpty());
            EXPECT_EQ(s.Len(), 0);
            // Must still yield a valid null-terminated buffer.
            EXPECT_STREQ(*s, "");
        }

        TEST(FStringTest, ConstructsFromCStringAndStdString)
        {
            const FString a("hello");
            EXPECT_EQ(a.Len(), 5);
            EXPECT_STREQ(*a, "hello");
            EXPECT_FALSE(a.IsEmpty());

            const FString b(std::string("world"));
            EXPECT_STREQ(*b, "world");

            // Pointer + explicit length (does not require null termination).
            const FString c("abcdef", 3);
            EXPECT_EQ(c.Len(), 3);
            EXPECT_STREQ(*c, "abc");
        }

        TEST(FStringTest, EmbeddedLengthNotBufferLength)
        {
            // The storage invariant: Data holds the characters PLUS a null
            // terminator, so Len() must never be the raw array count.
            const FString s("abc");
            EXPECT_EQ(s.Len(), 3);
            EXPECT_EQ(s.GetCharArray().Num(), 4);
        }

        TEST(FStringTest, AppendAndConcatenate)
        {
            FString s("foo");
            s += "bar";
            EXPECT_STREQ(*s, "foobar");

            s.AppendChar('!');
            EXPECT_STREQ(*s, "foobar!");
            EXPECT_EQ(s.Len(), 7);

            const FString joined = FString("a") + FString("b") + "c";
            EXPECT_STREQ(*joined, "abc");
        }

        TEST(FStringTest, AppendToEmptyStartsClean)
        {
            // Appending to a default-constructed string must not read the
            // (absent) terminator slot.
            FString s;
            s += "x";
            EXPECT_STREQ(*s, "x");
            EXPECT_EQ(s.Len(), 1);
        }

        TEST(FStringTest, ComparisonRespectsCase)
        {
            const FString a("Hello");
            EXPECT_TRUE(a.Equals(FString("Hello")));
            EXPECT_FALSE(a.Equals(FString("hello")));
            EXPECT_TRUE(a.Equals(FString("hello"), FString::ESearchCase::IgnoreCase));
            EXPECT_TRUE(a == "Hello");
        }

        TEST(FStringTest, SearchOperations)
        {
            const FString s("the quick brown fox");
            EXPECT_EQ(s.Find("quick"), 4);
            EXPECT_EQ(s.Find("QUICK"), FString::InvalidIndex);
            EXPECT_EQ(s.Find("QUICK", FString::ESearchCase::IgnoreCase), 4);
            EXPECT_EQ(s.Find("absent"), FString::InvalidIndex);
            EXPECT_TRUE(s.Contains("brown"));
            EXPECT_TRUE(s.StartsWith("the"));
            EXPECT_TRUE(s.EndsWith("fox"));
            EXPECT_FALSE(s.StartsWith("fox"));

            i32 idx = 0;
            EXPECT_TRUE(s.FindChar('q', idx));
            EXPECT_EQ(idx, 4);
        }

        TEST(FStringTest, Substrings)
        {
            const FString s("abcdef");
            EXPECT_STREQ(*s.Left(3), "abc");
            EXPECT_STREQ(*s.Right(2), "ef");
            EXPECT_STREQ(*s.Mid(2, 2), "cd");
            EXPECT_STREQ(*s.LeftChop(2), "abcd");
            EXPECT_STREQ(*s.RightChop(4), "ef");
            // Out-of-range must clamp, not read past the buffer.
            EXPECT_STREQ(*s.Left(100), "abcdef");
            EXPECT_STREQ(*s.Mid(100), "");
        }

        TEST(FStringTest, CaseAndTrimming)
        {
            EXPECT_STREQ(*FString("MiXeD").ToUpper(), "MIXED");
            EXPECT_STREQ(*FString("MiXeD").ToLower(), "mixed");
            EXPECT_STREQ(*FString("  pad  ").TrimStartAndEnd(), "pad");
            EXPECT_STREQ(*FString("\t x \n").TrimStartAndEnd(), "x");
        }

        // FString stores an explicit length, so the counted and string_view
        // constructors can hold a NUL in the middle. Comparison must respect
        // that: a terminator-driven strcmp stops at the first one and reported
        // strings that differ AFTER it as equal, which Equals' length guard
        // could not catch because the lengths matched.
        TEST(FStringTest, ComparisonIsLengthAwareAcrossEmbeddedNuls)
        {
            const FString a(std::string_view("a\0b", 3));
            const FString b(std::string_view("a\0c", 3));

            ASSERT_EQ(a.Len(), 3);
            ASSERT_EQ(b.Len(), 3);

            EXPECT_FALSE(a.Equals(b));
            EXPECT_FALSE(a == b);
            EXPECT_NE(a.Compare(b), 0);
            EXPECT_TRUE(a.Equals(FString(std::string_view("a\0b", 3))));

            // Case-insensitive must be length-aware too.
            const FString upper(std::string_view("A\0B", 3));
            EXPECT_TRUE(a.Equals(upper, FString::ESearchCase::IgnoreCase));
            EXPECT_FALSE(b.Equals(upper, FString::ESearchCase::IgnoreCase));

            // A prefix must not compare equal to the longer string, and must
            // sort before it.
            const FString prefix(std::string_view("a\0", 2));
            EXPECT_FALSE(prefix.Equals(a));
            EXPECT_LT(prefix.Compare(a), 0);
            EXPECT_GT(a.Compare(prefix), 0);
        }

        TEST(FStringTest, SplitFollowsUnrealSemantics)
        {
            FString left, right;
            const FString path("folder/file.txt");
            EXPECT_TRUE(path.Split("/", &left, &right));
            EXPECT_STREQ(*left, "folder");
            EXPECT_STREQ(*right, "file.txt");

            // No separator: returns false and leaves the outputs untouched.
            FString l2("untouched"), r2("untouched");
            EXPECT_FALSE(path.Split("|", &l2, &r2));
            EXPECT_STREQ(*l2, "untouched");
            EXPECT_STREQ(*r2, "untouched");
        }

        TEST(FStringTest, Printf)
        {
            EXPECT_STREQ(*FString::Printf("%d-%s", 42, "x"), "42-x");
            EXPECT_STREQ(*FString::FromInt(-7), "-7");
            // Longer than any small-buffer guess, to exercise the sizing pass.
            const FString big = FString::Printf("%0*d", 500, 1);
            EXPECT_EQ(big.Len(), 500);
        }

        TEST(FStringTest, StdInterop)
        {
            const FString s("round-trip");
            EXPECT_EQ(s.ToStdString(), std::string("round-trip"));
            EXPECT_EQ(s.ToView(), std::string_view("round-trip"));
            EXPECT_EQ(FString(s.ToStdString()).Len(), s.Len());
        }

        // ------------------------------------------------------------------
        // The reason this type exists.
        // ------------------------------------------------------------------

        TEST(FStringTest, IsMarkedTriviallyRelocatable)
        {
            static_assert(TIsTriviallyRelocatable_V<FString>,
                          "FString must be trivially relocatable — that is the whole point of it");
            static_assert(!TIsTriviallyRelocatable_V<std::string>,
                          "std::string must remain marked NON-relocatable (libstdc++ SSO self-pointer)");
            SUCCEED();
        }

        TEST(FStringTest, StoredInTArraySurvivesReallocation)
        {
            // Force many reallocations, so the elements are relocated bitwise
            // by FMemory::Realloc repeatedly. With std::string elements this is
            // precisely the sequence that corrupts the heap on libstdc++ and
            // aborts with "free(): invalid pointer" at array destruction.
            //
            // Deliberately SHORT strings: only those live in the SSO inline
            // buffer and carry the self-referential pointer. Long strings point
            // at a separate heap block and would survive relocation anyway, so
            // a test using long strings would pass even against a broken type.
            TArray<FString, TSizedDefaultAllocator<32>> arr;
            constexpr i32 kCount = 512;

            for (i32 i = 0; i < kCount; ++i)
                arr.Add(FString::Printf("s%d", i)); // 2-4 chars => SSO range

            ASSERT_EQ(arr.Num(), kCount);

            // Every element must still read back correctly after all that
            // relocation — a corrupted element typically shows up as garbage
            // content well before it shows up as a crash.
            for (i32 i = 0; i < kCount; ++i)
            {
                const FString expected = FString::Printf("s%d", i);
                EXPECT_TRUE(arr[i].Equals(expected)) << "element " << i << " corrupted by relocation";
            }

            // Destruction of `arr` at scope exit is where the invalid free
            // would fire; reaching the end of the test without aborting is
            // itself part of the assertion.
        }

        TEST(FStringTest, TArrayOfStringsSurvivesInsertAndRemove)
        {
            // The other relocation path: RelocateConstructItems memmoves during
            // insert/remove shifting, distinct from realloc-based growth.
            TArray<FString, TSizedDefaultAllocator<32>> arr;
            for (i32 i = 0; i < 32; ++i)
                arr.Add(FString::Printf("e%d", i));

            arr.Insert(FString("inserted"), 0);
            EXPECT_STREQ(*arr[0], "inserted");
            EXPECT_STREQ(*arr[1], "e0");

            arr.RemoveAt(0);
            EXPECT_STREQ(*arr[0], "e0");
            EXPECT_EQ(arr.Num(), 32);

            for (i32 i = 0; i < 32; ++i)
                EXPECT_TRUE(arr[i].Equals(FString::Printf("e%d", i))) << "element " << i << " corrupted by shift";
        }
    } // namespace
} // namespace OloEngine::Tests
