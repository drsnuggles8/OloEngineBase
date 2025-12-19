#pragma once

// @file Invoke.h
// @brief Template utilities for invoking callables (functions, member functions, functors)
//
// Ported from Unreal Engine's Templates/Invoke.h

#include <type_traits>
#include <functional>

namespace OloEngine
{
    // @brief Invokes a callable with the given arguments.
    // @note This is a simple wrapper around std::invoke for compatibility.
    template<typename CallableType, typename... ArgTypes>
    constexpr auto Invoke(CallableType&& Callable, ArgTypes&&... Args)
        -> decltype(std::invoke(std::forward<CallableType>(Callable), std::forward<ArgTypes>(Args)...))
    {
        return std::invoke(std::forward<CallableType>(Callable), std::forward<ArgTypes>(Args)...);
    }

    // @brief Gets the result type of invoking a callable with the given arguments.
    template<typename CallableType, typename... ArgTypes>
    using TInvokeResult_T = std::invoke_result_t<CallableType, ArgTypes...>;

} // namespace OloEngine
