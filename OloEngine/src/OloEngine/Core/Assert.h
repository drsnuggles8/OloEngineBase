#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"
#include <filesystem>

#ifdef OLO_DEBUG
#define OLO_ENABLE_ASSERTS
#endif

#define OLO_ENABLE_VERIFY

#ifdef OLO_ENABLE_ASSERTS
// Use __VA_OPT__ only if compiler and C++ standard support it
#if __cplusplus < 202002L
#define OLO_CORE_ASSERT_MESSAGE_INTERNAL(...) ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Core, "Assertion Failed", ##__VA_ARGS__)
#define OLO_ASSERT_MESSAGE_INTERNAL(...) ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Client, "Assertion Failed", ##__VA_ARGS__)
#else
#define OLO_CORE_ASSERT_MESSAGE_INTERNAL(...) ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Core, "Assertion Failed" __VA_OPT__(, ) __VA_ARGS__)
#define OLO_ASSERT_MESSAGE_INTERNAL(...) ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Client, "Assertion Failed" __VA_OPT__(, ) __VA_ARGS__)
#endif

#define OLO_CORE_ASSERT(condition, ...)                    \
    do                                                     \
    {                                                      \
        if (!(condition))                                  \
        {                                                  \
            OLO_CORE_ASSERT_MESSAGE_INTERNAL(__VA_ARGS__); \
            OLO_DEBUGBREAK();                              \
        }                                                  \
    } while (0)
#define OLO_ASSERT(condition, ...)                    \
    do                                                \
    {                                                 \
        if (!(condition))                             \
        {                                             \
            OLO_ASSERT_MESSAGE_INTERNAL(__VA_ARGS__); \
            OLO_DEBUGBREAK();                         \
        }                                             \
    } while (0)
#else
#define OLO_CORE_ASSERT(condition, ...) ((void)(condition))
#define OLO_ASSERT(condition, ...) ((void)(condition))
#endif

#ifdef OLO_ENABLE_VERIFY
// Use __VA_OPT__ only if compiler and C++ standard support it
#if __cplusplus < 202002L
#define OLO_CORE_VERIFY_MESSAGE_INTERNAL(...) ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Core, "Verify Failed", ##__VA_ARGS__)
#define OLO_VERIFY_MESSAGE_INTERNAL(...) ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Client, "Verify Failed", ##__VA_ARGS__)
#else
#define OLO_CORE_VERIFY_MESSAGE_INTERNAL(...) ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Core, "Verify Failed" __VA_OPT__(, ) __VA_ARGS__)
#define OLO_VERIFY_MESSAGE_INTERNAL(...) ::OloEngine::Log::PrintAssertMessage(::OloEngine::Log::Type::Client, "Verify Failed" __VA_OPT__(, ) __VA_ARGS__)
#endif

#define OLO_CORE_VERIFY(condition, ...)                    \
    do                                                     \
    {                                                      \
        if (!(condition))                                  \
        {                                                  \
            OLO_CORE_VERIFY_MESSAGE_INTERNAL(__VA_ARGS__); \
            OLO_DEBUGBREAK();                              \
        }                                                  \
    } while (0)
#define OLO_VERIFY(condition, ...)                    \
    do                                                \
    {                                                 \
        if (!(condition))                             \
        {                                             \
            OLO_VERIFY_MESSAGE_INTERNAL(__VA_ARGS__); \
            OLO_DEBUGBREAK();                         \
        }                                             \
    } while (0)
#else
#define OLO_CORE_VERIFY(condition, ...) ((void)(condition))
#define OLO_VERIFY(condition, ...) ((void)(condition))
#endif

/**
 * OLO_VERIFY_SLOW / OLO_CORE_VERIFY_SLOW
 *
 * ALWAYS evaluates the expression (unlike OLO_ASSERT which may skip in release).
 * Only checks the result and triggers debugbreak in Debug/Development builds.
 * Use when the expression has side effects that must always execute.
 *
 * Equivalent to UE's verifySlow macro: expression is always evaluated,
 * but result checking is only done in DO_GUARD_SLOW builds.
 *
 * Example: OLO_VERIFY_SLOW(ExecuteTask() != nullptr);
 *          // ExecuteTask() is ALWAYS called, but only asserts in debug
 */
#if defined(OLO_DEBUG) || defined(OLO_RELEASE)
// Debug/Development: Evaluate expression and check result
#define OLO_CORE_VERIFY_SLOW(expr, ...)                    \
    do                                                     \
    {                                                      \
        if (!(expr))                                       \
        {                                                  \
            OLO_CORE_VERIFY_MESSAGE_INTERNAL(__VA_ARGS__); \
            OLO_DEBUGBREAK();                              \
        }                                                  \
    } while (0)
#define OLO_VERIFY_SLOW(expr, ...)                    \
    do                                                \
    {                                                 \
        if (!(expr))                                  \
        {                                             \
            OLO_VERIFY_MESSAGE_INTERNAL(__VA_ARGS__); \
            OLO_DEBUGBREAK();                         \
        }                                             \
    } while (0)
#else
// Dist/Shipping: Evaluate expression but ignore result (still has side effects)
#define OLO_CORE_VERIFY_SLOW(expr, ...) ((void)(expr))
#define OLO_VERIFY_SLOW(expr, ...) ((void)(expr))
#endif

/**
 * OLO_CHECK_SLOW / OLO_CORE_CHECK_SLOW (checkSlow equivalent)
 *
 * Only evaluates and checks the expression in Debug builds.
 * The expression is completely compiled out in Release/Dist.
 *
 * Equivalent to UE's checkSlow macro: expression is only evaluated in
 * DO_GUARD_SLOW builds (typically debug-only).
 *
 * Use when the check is expensive and the expression has no side effects.
 *
 * Example: OLO_CHECK_SLOW(ExpensiveValidation());
 *          // ExpensiveValidation() is NOT called in Release/Dist builds
 */
#ifdef OLO_DEBUG
#define OLO_CORE_CHECK_SLOW(expr, ...) OLO_CORE_ASSERT(expr, __VA_ARGS__)
#define OLO_CHECK_SLOW(expr, ...) OLO_ASSERT(expr, __VA_ARGS__)
#else
// Release/Dist: Expression is NOT evaluated - compiled out completely
#define OLO_CORE_CHECK_SLOW(expr, ...) \
    do                                 \
    {                                  \
    } while (0)
#define OLO_CHECK_SLOW(expr, ...) \
    do                            \
    {                             \
    } while (0)
#endif

// Alias for UE compatibility - maps checkSlow to OLO_CHECK_SLOW
#define checkSlow(expr) OLO_CORE_CHECK_SLOW(expr)
