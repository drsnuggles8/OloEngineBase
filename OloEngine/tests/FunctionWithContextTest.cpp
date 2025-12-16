#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Templates/FunctionWithContext.h"

using namespace OloEngine;

// ============================================================================
// TFunctionWithContext Tests
// ============================================================================

TEST(FunctionWithContextTest, DefaultConstruction)
{
    TFunctionWithContext<void()> NullFunc;
    EXPECT_FALSE(NullFunc);
    EXPECT_EQ(NullFunc.GetFunction(), nullptr);
    EXPECT_EQ(NullFunc.GetContext(), nullptr);
}

TEST(FunctionWithContextTest, NullptrConstruction)
{
    TFunctionWithContext<void()> NullFunc(nullptr);
    EXPECT_FALSE(NullFunc);
}

TEST(FunctionWithContextTest, LambdaConstruction)
{
    bool WasCalled = false;
    TFunctionWithContext<void()> Func = [&WasCalled]() { WasCalled = true; };
    
    EXPECT_TRUE(Func);
    EXPECT_NE(Func.GetFunction(), nullptr);
    EXPECT_NE(Func.GetContext(), nullptr);
    
    Func();
    EXPECT_TRUE(WasCalled);
}

TEST(FunctionWithContextTest, LambdaWithReturnValue)
{
    int Value = 42;
    TFunctionWithContext<int()> Func = [&Value]() { return Value * 2; };
    
    EXPECT_TRUE(Func);
    EXPECT_EQ(Func(), 84);
}

TEST(FunctionWithContextTest, LambdaWithArguments)
{
    int Sum = 0;
    TFunctionWithContext<void(int, int)> Func = [&Sum](int A, int B) { Sum = A + B; };
    
    EXPECT_TRUE(Func);
    Func(10, 20);
    EXPECT_EQ(Sum, 30);
}

TEST(FunctionWithContextTest, LambdaWithArgumentsAndReturn)
{
    TFunctionWithContext<int(int, int)> Func = [](int A, int B) { return A + B; };
    
    EXPECT_TRUE(Func);
    EXPECT_EQ(Func(5, 7), 12);
}

TEST(FunctionWithContextTest, ExplicitFunctionPointerConstruction)
{
    static int GlobalValue = 0;
    auto SetValue = [](void* Context, int Value) {
        GlobalValue = Value + *static_cast<int*>(Context);
    };
    
    int Offset = 100;
    TFunctionWithContext<void(int)> Func(+SetValue, &Offset);
    
    EXPECT_TRUE(Func);
    Func(5);
    EXPECT_EQ(GlobalValue, 105);
}

TEST(FunctionWithContextTest, Assignment)
{
    int CallCount = 0;
    TFunctionWithContext<void()> Func = [&CallCount]() { CallCount = 1; };
    Func();
    EXPECT_EQ(CallCount, 1);
    
    // Reassign to different lambda
    Func = [&CallCount]() { CallCount = 2; };
    Func();
    EXPECT_EQ(CallCount, 2);
}

TEST(FunctionWithContextTest, GetFunctionAndContext)
{
    int Value = 42;
    TFunctionWithContext<int()> Func = [&Value]() { return Value; };
    
    // Manually invoke using extracted function and context
    auto FunctionPtr = Func.GetFunction();
    auto Context = Func.GetContext();
    
    EXPECT_NE(FunctionPtr, nullptr);
    EXPECT_NE(Context, nullptr);
    EXPECT_EQ(FunctionPtr(Context), 42);
}

TEST(FunctionWithContextTest, MultipleArgTypes)
{
    std::string Result;
    TFunctionWithContext<void(const char*, int, float)> Func = 
        [&Result](const char* Str, int I, float F) {
            Result = std::string(Str) + "_" + std::to_string(I) + "_" + std::to_string(static_cast<int>(F));
        };
    
    Func("test", 42, 3.14f);
    EXPECT_EQ(Result, "test_42_3");
}

// ============================================================================
// Usage Pattern: Simulating ParkingLot-style API
// ============================================================================

namespace TestParkingLotStyle
{
    // Internal implementation functions that take raw function pointers
    static void InternalCall(void (*Func)(void*), void* Context)
    {
        if (Func)
        {
            Func(Context);
        }
    }

    static int InternalCallWithReturn(int (*Func)(void*, int), void* Context, int Value)
    {
        if (Func)
        {
            return Func(Context, Value);
        }
        return 0;
    }

    // Public API using TFunctionWithContext
    static void PublicCall(TFunctionWithContext<void()> Func)
    {
        InternalCall(Func.GetFunction(), Func.GetContext());
    }

    static int PublicCallWithReturn(TFunctionWithContext<int(int)> Func, int Value)
    {
        return InternalCallWithReturn(Func.GetFunction(), Func.GetContext(), Value);
    }
}

TEST(FunctionWithContextTest, ParkingLotStyleUsageVoid)
{
    bool WasCalled = false;
    TestParkingLotStyle::PublicCall([&WasCalled]() { WasCalled = true; });
    EXPECT_TRUE(WasCalled);
}

TEST(FunctionWithContextTest, ParkingLotStyleUsageWithReturn)
{
    int Multiplier = 10;
    int Result = TestParkingLotStyle::PublicCallWithReturn(
        [&Multiplier](int Value) { return Value * Multiplier; }, 
        5
    );
    EXPECT_EQ(Result, 50);
}
