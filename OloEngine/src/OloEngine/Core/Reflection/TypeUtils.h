#pragma once
#include "OloEnginePCH.h"

#include <array>
#include <iostream>
#include <tuple>
#include <type_traits>
#include <vector>
#include <utility>

namespace OloEngine::Core::Reflection
{

    //==============================================================================
    /// Member pointer type extraction utilities
    namespace MemberPointer
    {
        namespace Impl
        {
            template<typename T>
            struct ReturnTypeFunction;

            // Non-const member functions
            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...)>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) noexcept>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) &>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) &&>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) & noexcept>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) && noexcept>
            {
                using Type = Return;
            };

            // Const member functions
            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const noexcept>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const&>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const&&>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const & noexcept>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const && noexcept>
            {
                using Type = Return;
            };

            // Volatile member functions
            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) volatile>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) volatile noexcept>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) volatile&>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) volatile&&>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) volatile & noexcept>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) volatile && noexcept>
            {
                using Type = Return;
            };

            // Const volatile member functions
            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const volatile>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const volatile noexcept>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const volatile&>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const volatile&&>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const volatile & noexcept>
            {
                using Type = Return;
            };

            template<typename Object, typename Return, typename... Args>
            struct ReturnTypeFunction<Return (Object::*)(Args...) const volatile && noexcept>
            {
                using Type = Return;
            };

            template<typename T>
            struct ReturnTypeObject;

            template<typename Object, typename Return>
            struct ReturnTypeObject<Return Object::*>
            {
                using Type = Return;
            };
        } // namespace Impl

        template<typename T>
        struct ReturnType;

        template<typename T>
        struct ReturnType : std::enable_if_t<std::is_member_pointer_v<T>,
                                             std::conditional_t<std::is_member_object_pointer_v<T>,
                                                                Impl::ReturnTypeObject<T>,
                                                                Impl::ReturnTypeFunction<T>>>
        {
        };
    } // namespace MemberPointer

    //==============================================================================
    /// Check if a type is an instantiation of a specific class template
    template<template<typename...> class Template, typename T>
    struct IsSpecializationOf : std::false_type
    {
    };

    template<template<typename...> class Template, typename... Args>
    struct IsSpecializationOf<Template, Template<Args...>> : std::true_type
    {
    };

    template<template<typename...> class Template, typename T>
    constexpr bool IsSpecializationOf_v = IsSpecializationOf<Template, T>::value;

    //==============================================================================
    /// Detect if a template has been explicitly specialized by checking for expected members
    /// This works by attempting to access a member that only exists in specializations
    template<typename T, typename = void>
    struct IsSpecialized : std::false_type
    {
    };

    /// SFINAE-based partial specialization that detects the ReflectionSpecializationTag marker
    template<typename T>
    struct IsSpecialized<T, std::void_t<typename T::ReflectionSpecializationTag>> : std::true_type
    {
    };

    template<typename T>
    constexpr bool IsSpecialized_v = IsSpecialized<T>::value;

    //==============================================================================
    /// Type filtering utilities
    struct FilterVoidAlt
    {
    };

    template<class T, class Alternative = FilterVoidAlt>
    struct FilterVoid
    {
        using Type = std::conditional_t<std::is_void_v<T>, Alternative, T>;
    };

    template<class T>
    using FilterVoid_t = typename FilterVoid<T>::Type;

    //==============================================================================
    /// Array detection utilities
    namespace ArrayImpl
    {
        template<typename T>
        struct IsArrayImpl : std::false_type
        {
        };
        template<typename T, sizet N>
        struct IsArrayImpl<std::array<T, N>> : std::true_type
        {
        };
        template<typename... Args>
        struct IsArrayImpl<std::vector<Args...>> : std::true_type
        {
        };
    } // namespace ArrayImpl

    template<typename T>
    struct IsArray
    {
        static constexpr bool s_Value = ArrayImpl::IsArrayImpl<std::decay_t<T>>::value;
    };

    template<typename T>
    inline constexpr bool IsArray_v = IsArray<T>::s_Value;

    //==============================================================================
    /// Streaming detection utilities (for debugging/serialization)
    template<class T>
    class IsStreamable
    {
        // Match if streaming is supported
        template<class TT>
        static constexpr auto Test(int) -> decltype(std::declval<std::ostream&>() << std::declval<TT>(), std::true_type());

        // Match if streaming is not supported
        template<class>
        static constexpr auto Test(...) -> std::false_type;

      public:
        // Check return value from the matching "test" overload
        static constexpr bool s_Value = decltype(Test<T>(0))::value;
    };

    template<class T>
    inline constexpr bool IsStreamable_v = IsStreamable<T>::s_Value;

    //==============================================================================
    /// Nth element extraction from parameter pack
    template<auto I, typename... Args>
    constexpr decltype(auto) NthElement(Args&&... args)
    {
        static_assert(I < sizeof...(Args), "Index out of bounds for parameter pack");
        return std::get<I>(std::forward_as_tuple(std::forward<Args>(args)...));
    }

} // namespace OloEngine::Core::Reflection
