#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Templates/FunctionWithContext.h"

// ============================================================================
// FunctionWithContextTest — TFunctionWithContext is a 2-pointer callable
// wrapper used to bridge stateless-API entry points (e.g. ParkingLot) to
// captures from C++ lambdas without heap allocation. The contract is:
//
//   1. Default-constructed wrapper is null (no function, no context).
//   2. Constructing from a capturing lambda binds the callable and
//      captures-as-context such that invoking it runs the lambda body.
//   3. Reassignment replaces the bound callable; the previous one is
//      released.
//   4. GetFunction()/GetContext() round-trip through the stateless API
//      shape that motivated the wrapper.
//
// The previous version of this file had 11 tests covering void/int return
// shapes, 1/2/3-arg permutations, function-pointer construction, and a
// duplicate ParkingLot demo. Per docs/testing.md §4.7 the
// type-permutation padding adds no defence — the wrapper's behaviour is
// invariant in arity/return-type, those are template parameters. The
// four tests below pin the four contract clauses above, no more.
// ============================================================================

using namespace OloEngine;

TEST(FunctionWithContext, DefaultConstructedIsNullAndExposesNullSlots)
{
    TFunctionWithContext<void()> nullFunc;
    EXPECT_FALSE(nullFunc);
    EXPECT_EQ(nullFunc.GetFunction(), nullptr);
    EXPECT_EQ(nullFunc.GetContext(), nullptr);

    TFunctionWithContext<void()> nullFuncFromNullptr(nullptr);
    EXPECT_FALSE(nullFuncFromNullptr);
}

TEST(FunctionWithContext, LambdaBoundCallableInvokesCaptureBody)
{
    bool wasCalled = false;
    auto lambda = [&wasCalled](int a, int b)
    { wasCalled = (a + b == 12); };

    TFunctionWithContext<void(int, int)> func = lambda;
    EXPECT_TRUE(func);
    EXPECT_NE(func.GetFunction(), nullptr);
    EXPECT_NE(func.GetContext(), nullptr);

    func(5, 7);
    EXPECT_TRUE(wasCalled);
}

TEST(FunctionWithContext, ReassignmentReplacesBoundCallable)
{
    int counter = 0;
    auto first = [&counter]()
    { counter = 1; };
    auto second = [&counter]()
    { counter = 2; };

    TFunctionWithContext<void()> func = first;
    func();
    EXPECT_EQ(counter, 1);

    func = second;
    func();
    EXPECT_EQ(counter, 2)
        << "reassignment didn't replace the bound callable — the wrapper "
           "is still calling the previous lambda's context.";
}

// Demonstrates the motivating use case: a stateless internal entry point
// that takes (function pointer, void* context) — the shape ParkingLot uses
// — being driven by a lambda-with-captures via TFunctionWithContext. If
// this contract breaks, every ParkingLot-style API silently invokes the
// wrong context.
namespace
{
    int InvokeStateless(int (*fn)(void*, int), void* ctx, int arg)
    {
        return fn ? fn(ctx, arg) : 0;
    }
} // namespace

TEST(FunctionWithContext, RoundTripsThroughStatelessInvocationAPI)
{
    int multiplier = 10;
    // The lambda must be a named lvalue: TFunctionWithContext is a
    // *non-owning* wrapper (see the header comment on the class), so the
    // captured-lambda's storage has to outlive every call through
    // GetContext(). Initialising the wrapper from a temporary rvalue
    // dangles the context pointer to a stack slot that dies at the end of
    // the full-expression — ASan stack-use-after-scope.
    auto lambda = [&multiplier](int v)
    { return v * multiplier; };
    TFunctionWithContext<int(int)> func = lambda;

    const int result = InvokeStateless(func.GetFunction(), func.GetContext(), 5);
    EXPECT_EQ(result, 50)
        << "GetFunction/GetContext round-trip lost the lambda's capture — "
           "the wrapper is not the right shape for stateless-API bridges.";
}
