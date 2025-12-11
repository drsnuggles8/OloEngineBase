#pragma once

/**
 * @file FunctionRef.h
 * @brief Non-owning reference to a callable object
 * 
 * TFunctionRef is a lightweight, non-owning reference to any callable object.
 * Unlike std::function, it never allocates memory and has minimal overhead.
 * 
 * IMPORTANT: The callable must outlive the TFunctionRef. If you bind a lambda
 * and the lambda goes out of scope, the TFunctionRef becomes dangling.
 * 
 * Use TFunctionRef for:
 * - Function parameters where you don't need ownership
 * - Callbacks that are invoked immediately (not stored)
 * - High-performance scenarios where std::function overhead matters
 * 
 * Ported from Unreal Engine's TFunctionRef (simplified version)
 */

#include "OloEngine/Core/Base.h"

#include <type_traits>
#include <utility>

namespace OloEngine
{
    // Forward declaration
    template <typename FuncType>
    class TFunctionRef;

    /**
     * @struct TIsTFunctionRef
     * @brief Type trait to check if T is a TFunctionRef
     */
    template <typename T>
    struct TIsTFunctionRef : std::false_type {};

    template <typename T>
    struct TIsTFunctionRef<TFunctionRef<T>> : std::true_type {};

    template <typename T>
    struct TIsTFunctionRef<const T> : TIsTFunctionRef<T> {};

    template <typename T>
    struct TIsTFunctionRef<volatile T> : TIsTFunctionRef<T> {};

    template <typename T>
    struct TIsTFunctionRef<const volatile T> : TIsTFunctionRef<T> {};

    template <typename T>
    inline constexpr bool TIsTFunctionRef_v = TIsTFunctionRef<T>::value;

    namespace Private
    {
        /**
         * @brief Helper to invoke a callable and return the result
         */
        template <typename Functor, typename Ret, typename... ParamTypes>
        struct TFunctionRefCaller
        {
            static Ret Call(void* Obj, ParamTypes&... Params)
            {
                if constexpr (std::is_void_v<Ret>)
                {
                    (*static_cast<Functor*>(Obj))(std::forward<ParamTypes>(Params)...);
                }
                else
                {
                    return (*static_cast<Functor*>(Obj))(std::forward<ParamTypes>(Params)...);
                }
            }
        };

        /**
         * @brief Check if a callable is bound (not null)
         */
        template <typename T>
        OLO_FINLINE bool IsBound(const T& Func)
        {
            if constexpr (std::is_pointer_v<T> || std::is_member_pointer_v<T>)
            {
                return Func != nullptr;
            }
            else
            {
                // Generic callables are assumed to be bound
                return true;
            }
        }
    }

    /**
     * @class TFunctionRef
     * @brief A non-owning reference to a callable object
     * 
     * TFunctionRef<FuncType> where FuncType is a function signature like:
     *   TFunctionRef<int(float, const char*)>
     * 
     * Example usage:
     * @code
     * void ProcessItems(TFunctionRef<void(int)> Callback)
     * {
     *     for (int i = 0; i < 10; ++i)
     *         Callback(i);
     * }
     * 
     * // Call with lambda
     * ProcessItems([](int x) { std::cout << x << "\n"; });
     * 
     * // Call with function pointer
     * void MyFunc(int x);
     * ProcessItems(&MyFunc);
     * @endcode
     * 
     * @tparam Ret Return type
     * @tparam ParamTypes Parameter types
     */
    template <typename Ret, typename... ParamTypes>
    class TFunctionRef<Ret(ParamTypes...)>
    {
    public:
        /**
         * @brief Constructor from any callable
         * 
         * @tparam FunctorType Type of the callable (deduced)
         * @param InFunc The callable to reference (must outlive this TFunctionRef!)
         */
        template <typename FunctorType,
                  typename = std::enable_if_t<
                      !TIsTFunctionRef_v<std::decay_t<FunctorType>> &&
                      std::is_invocable_r_v<Ret, std::decay_t<FunctorType>, ParamTypes...>
                  >>
        TFunctionRef(FunctorType&& InFunc)
            : m_Callable(&Private::TFunctionRefCaller<std::remove_reference_t<FunctorType>, Ret, ParamTypes...>::Call)
            , m_Ptr(const_cast<void*>(static_cast<const void*>(&InFunc)))
        {
            OLO_CORE_ASSERT(Private::IsBound(InFunc), "Cannot bind a null callable to TFunctionRef");
        }

        // Default copy is fine - we're just copying pointers
        TFunctionRef(const TFunctionRef&) = default;
        TFunctionRef& operator=(const TFunctionRef&) = delete;  // Matches UE - no assignment
        ~TFunctionRef() = default;

        /**
         * @brief Invoke the referenced callable
         */
        Ret operator()(ParamTypes... Params) const
        {
            return m_Callable(m_Ptr, Params...);
        }

    private:
        // Function pointer type for the caller
        using CallerType = Ret(*)(void*, ParamTypes&...);

        CallerType m_Callable;  // Points to TFunctionRefCaller::Call instantiation
        void* m_Ptr;            // Points to the actual callable object
    };

    /**
     * @brief Deduction guide for TFunctionRef from function pointers
     */
    template <typename Ret, typename... Args>
    TFunctionRef(Ret(*)(Args...)) -> TFunctionRef<Ret(Args...)>;

} // namespace OloEngine
