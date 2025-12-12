#pragma once

/**
 * @file Invoke.h
 * @brief Template utilities for invoking callables (functions, member functions, functors)
 * 
 * Ported from Unreal Engine's Templates/Invoke.h
 */

#include <type_traits>
#include <functional>

namespace OloEngine
{
    /**
     * Invokes a callable with the given arguments.
     * This is a simple wrapper around std::invoke for compatibility.
     */
    template <typename CallableType, typename... ArgTypes>
    constexpr auto Invoke(CallableType&& Callable, ArgTypes&&... Args)
        -> decltype(std::invoke(std::forward<CallableType>(Callable), std::forward<ArgTypes>(Args)...))
    {
        return std::invoke(std::forward<CallableType>(Callable), std::forward<ArgTypes>(Args)...);
    }

} // namespace OloEngine
