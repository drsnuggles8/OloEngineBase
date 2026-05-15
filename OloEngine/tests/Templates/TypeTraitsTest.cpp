#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Templates/UnrealTypeTraits.h"

#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// TypeTraitsTest — compile-time checks for the UnrealTypeTraits header.
//
// Every contract here is enforceable at compile time. We use one TEST
// to register the file in gtest's per-binary inventory + give a clear
// failure target, but the real work happens in static_asserts above —
// the test body itself only calls SUCCEED(). See docs/testing.md
// §4.3 ("static-assert in disguise") for the prior anti-pattern: 18
// runtime TEST cases asserting EXPECT_STREQ on TFormatSpecifier<u8>
// when `static_assert(strcmp(...) == 0)` does the same work for free.
// ============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// constexpr-strcmp helper so the format-specifier / name lookups become
// compile-time facts. strcmp is constexpr-friendly since C++23 with MSVC,
// but rolling our own keeps this portable to clang-cl.
constexpr bool CStrEq(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr)
        return a == b;
    while (*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}

// ----- TFormatSpecifier -----
static_assert(CStrEq(TFormatSpecifier<u8>::GetFormatSpecifier(), "%u"));
static_assert(CStrEq(TFormatSpecifier<u16>::GetFormatSpecifier(), "%u"));
static_assert(CStrEq(TFormatSpecifier<u32>::GetFormatSpecifier(), "%u"));
static_assert(CStrEq(TFormatSpecifier<u64>::GetFormatSpecifier(), "%llu"));
static_assert(CStrEq(TFormatSpecifier<i8>::GetFormatSpecifier(), "%d"));
static_assert(CStrEq(TFormatSpecifier<i16>::GetFormatSpecifier(), "%d"));
static_assert(CStrEq(TFormatSpecifier<i32>::GetFormatSpecifier(), "%d"));
static_assert(CStrEq(TFormatSpecifier<i64>::GetFormatSpecifier(), "%lld"));
static_assert(CStrEq(TFormatSpecifier<f32>::GetFormatSpecifier(), "%f"));
static_assert(CStrEq(TFormatSpecifier<f64>::GetFormatSpecifier(), "%f"));
static_assert(CStrEq(TFormatSpecifier<long double>::GetFormatSpecifier(), "%f"));
static_assert(CStrEq(TFormatSpecifier<bool>::GetFormatSpecifier(), "%i"));

// TNameOf::GetName() returns a non-constexpr const char*, so it runs at
// TEST time below in AllNameOfsAreCorrect — kept as a single TEST to
// preserve the contract without proliferating registration overhead.

// ----- TNthTypeFromParameterPack -----
static_assert(std::is_same_v<TNthTypeFromParameterPack_T<0, int, float, double>, int>);
static_assert(std::is_same_v<TNthTypeFromParameterPack_T<1, int, float, double>, float>);
static_assert(std::is_same_v<TNthTypeFromParameterPack_T<2, int, float, double>, double>);

// ----- TIsFundamentalType -----
static_assert(TIsFundamentalType<int>::Value);
static_assert(TIsFundamentalType<float>::Value);
static_assert(TIsFundamentalType<double>::Value);
static_assert(TIsFundamentalType<char>::Value);
static_assert(TIsFundamentalType<bool>::Value);
static_assert(TIsFundamentalType<void>::Value);

namespace TypeTraitsTestDetail
{
    struct PlainStruct
    {
    };
    struct PODStruct
    {
        int x;
        float y;
    };
    struct LargeStruct
    {
        char data[1024];
    };
} // namespace TypeTraitsTestDetail
static_assert(!TIsFundamentalType<TypeTraitsTestDetail::PlainStruct>::Value);
static_assert(!TIsFundamentalType<std::string>::Value);

// ----- TIsFunction -----
static_assert(TIsFunction<void()>::Value);
static_assert(TIsFunction<int(float, double)>::Value);
static_assert(TIsFunction<void(int, int, int)>::Value);
static_assert(!TIsFunction<int>::Value);
static_assert(!TIsFunction<int*>::Value);
static_assert(!TIsFunction<int (*)(float)>::Value); // pointer-to-function, not function itself

// ----- TCallTraits -----
static_assert(std::is_same_v<TCallTraits<int>::ParamType, const int>);
static_assert(std::is_same_v<TCallTraits<float>::ParamType, const float>);
static_assert(std::is_same_v<TCallTraits<TypeTraitsTestDetail::LargeStruct>::ParamType,
                             const TypeTraitsTestDetail::LargeStruct&>);
static_assert(std::is_same_v<TCallTraits<int*>::ParamType, int*>);

// ----- TIsBitwiseConstructible -----
static_assert(TIsBitwiseConstructible<int, int>::Value);
static_assert(TIsBitwiseConstructible<float, float>::Value);
static_assert(TIsBitwiseConstructible<u32, i32>::Value);
static_assert(TIsBitwiseConstructible<i32, u32>::Value);
static_assert(TIsBitwiseConstructible<u64, i64>::Value);
static_assert(TIsBitwiseConstructible<i64, u64>::Value);
static_assert(TIsBitwiseConstructible<const int*, int*>::Value);

// ----- TIsZeroConstructType -----
static_assert(TIsZeroConstructType<int>::Value);
static_assert(TIsZeroConstructType<float>::Value);
static_assert(TIsZeroConstructType<int*>::Value);

// ----- TIsPODType -----
static_assert(TIsPODType<int>::Value);
static_assert(TIsPODType<TypeTraitsTestDetail::PODStruct>::Value);
static_assert(!TIsPODType<std::string>::Value);
static_assert(!TIsPODType<std::vector<int>>::Value);

// ----- Logical combinators (TAnd / TOr / TNot) -----
static_assert(TAnd<TIsArithmetic<int>, TIsArithmetic<float>>::Value);
static_assert(!TAnd<TIsArithmetic<int>, TIsPointer<int>>::Value);
static_assert(TOr<TIsArithmetic<int>, TIsPointer<int>>::Value);
static_assert(!TOr<TIsPointer<int>, TIsPointer<float>>::Value);
static_assert(TNot<TIsPointer<int>>::Value);
static_assert(!TNot<TIsArithmetic<int>>::Value);

TEST(TypeTraitsTest, AllChecksAreCompileTime)
{
    // The compile-time block above is the real test surface — if any
    // static_assert fires, the build fails before this body runs.
    SUCCEED();
}

TEST(TypeTraitsTest, AllNameOfsAreCorrect)
{
    // TNameOf::GetName is not constexpr; check the strings at runtime.
    EXPECT_STREQ(TNameOf<u8>::GetName(), "u8");
    EXPECT_STREQ(TNameOf<u16>::GetName(), "u16");
    EXPECT_STREQ(TNameOf<u32>::GetName(), "u32");
    EXPECT_STREQ(TNameOf<u64>::GetName(), "u64");
    EXPECT_STREQ(TNameOf<i8>::GetName(), "i8");
    EXPECT_STREQ(TNameOf<i16>::GetName(), "i16");
    EXPECT_STREQ(TNameOf<i32>::GetName(), "i32");
    EXPECT_STREQ(TNameOf<i64>::GetName(), "i64");
    EXPECT_STREQ(TNameOf<f32>::GetName(), "f32");
    EXPECT_STREQ(TNameOf<f64>::GetName(), "f64");
}
