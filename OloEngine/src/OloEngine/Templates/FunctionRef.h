#pragma once

/**
 * @file FunctionRef.h
 * @brief Function reference and callable object wrappers
 * 
 * Provides three types of callable wrappers:
 * - TFunctionRef<T> : Non-owning reference to a callable (zero-copy, zero-alloc)
 * - TFunction<T> : Owning copy of a callable (can be stored/returned)
 * - TUniqueFunction<T> : Move-only owning callable (supports non-copyable functors)
 * 
 * Ported from Unreal Engine's Templates/Function.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/Invoke.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include "OloEngine/Misc/IntrusiveUnsetOptionalState.h"

#include <type_traits>
#include <utility>
#include <new>
#include <memory>

namespace OloEngine
{
    // Forward declarations
    template <typename FuncType>
    class TFunctionRef;

    template <typename FuncType>
    class TFunction;

    template <typename FuncType>
    class TUniqueFunction;

    // ========================================================================
    // Type Traits
    // ========================================================================

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

    /**
     * @struct TIsTFunction
     * @brief Type trait to check if T is a TFunction
     */
    template <typename T>
    struct TIsTFunction : std::false_type {};

    template <typename T>
    struct TIsTFunction<TFunction<T>> : std::true_type {};

    template <typename T>
    struct TIsTFunction<const T> : TIsTFunction<T> {};

    template <typename T>
    struct TIsTFunction<volatile T> : TIsTFunction<T> {};

    template <typename T>
    struct TIsTFunction<const volatile T> : TIsTFunction<T> {};

    template <typename T>
    inline constexpr bool TIsTFunction_v = TIsTFunction<T>::value;

    /**
     * @struct TIsTUniqueFunction
     * @brief Type trait to check if T is a TUniqueFunction
     */
    template <typename T>
    struct TIsTUniqueFunction : std::false_type {};

    template <typename T>
    struct TIsTUniqueFunction<TUniqueFunction<T>> : std::true_type {};

    template <typename T>
    struct TIsTUniqueFunction<const T> : TIsTUniqueFunction<T> {};

    template <typename T>
    struct TIsTUniqueFunction<volatile T> : TIsTUniqueFunction<T> {};

    template <typename T>
    struct TIsTUniqueFunction<const volatile T> : TIsTUniqueFunction<T> {};

    template <typename T>
    inline constexpr bool TIsTUniqueFunction_v = TIsTUniqueFunction<T>::value;

    // ========================================================================
    // Private Implementation
    // ========================================================================

    namespace Private
    {
        /**
         * @brief Virtual base for owning function storage
         */
        struct IFunctionOwnedObject
        {
            virtual ~IFunctionOwnedObject() = default;
            virtual void* GetCallable() = 0;
        };

        /**
         * @brief Wrapper for storing an owned callable object
         */
        template <typename FunctorType>
        struct TFunctionOwnedObject : IFunctionOwnedObject
        {
            FunctorType Callable;

            template <typename InFunctorType>
            explicit TFunctionOwnedObject(InFunctorType&& InCallable)
                : Callable(std::forward<InFunctorType>(InCallable))
            {
            }

            void* GetCallable() override
            {
                return &Callable;
            }
        };

        /**
         * @brief Generic function invocation caller
         */
        template <typename FunctorType, typename Ret, typename... ParamTypes>
        struct TFunctionRefCaller
        {
            static Ret Call(void* Obj, ParamTypes... Params)
            {
                if constexpr (std::is_void_v<Ret>)
                {
                    ::Invoke(*static_cast<FunctorType*>(Obj), std::forward<ParamTypes>(Params)...);
                }
                else
                {
                    return ::Invoke(*static_cast<FunctorType*>(Obj), std::forward<ParamTypes>(Params)...);
                }
            }
        };

        /**
         * @brief Base class for all function types
         */
        template <typename Ret, typename... ParamTypes>
        struct TFunctionRefBase
        {
            using CallerType = Ret(*)(void*, ParamTypes...);

            CallerType Callable = nullptr;
            void* Ptr = nullptr;

            OLO_FINLINE Ret Invoke(ParamTypes... Params) const
            {
                return Callable(Ptr, std::forward<ParamTypes>(Params)...);
            }
        };
    }

    // ========================================================================
    // TFunctionRef - Non-owning reference
    // ========================================================================

    /**
     * @class TFunctionRef
     * @brief A non-owning reference to a callable object
     *
     * Use TFunctionRef for function parameters where you don't need ownership.
     * The callable must outlive the TFunctionRef.
     *
     * Example:
     * @code
     * void ProcessItems(TFunctionRef<void(int)> Callback)
     * {
     *     for (int i = 0; i < 10; ++i)
     *         Callback(i);
     * }
     *
     * ProcessItems([](int x) { ... });
     * @endcode
     */
    template <typename Ret, typename... ParamTypes>
    class TFunctionRef<Ret(ParamTypes...)> : private Private::TFunctionRefBase<Ret, ParamTypes...>
    {
    public:
        using typename Private::TFunctionRefBase<Ret, ParamTypes...>::CallerType;

        /**
         * @brief Constructor from any callable
         */
        template <typename FunctorType,
                  typename = std::enable_if_t<
                      !TIsTFunctionRef_v<std::decay_t<FunctorType>> &&
                      std::is_invocable_r_v<Ret, std::decay_t<FunctorType>, ParamTypes...>
                  >>
        TFunctionRef(FunctorType&& InFunc)
        {
            this->Callable = &Private::TFunctionRefCaller<std::remove_reference_t<FunctorType>, Ret, ParamTypes...>::Call;
            this->Ptr = const_cast<void*>(static_cast<const void*>(std::addressof(InFunc)));
        }

        TFunctionRef(const TFunctionRef&) = default;
        TFunctionRef& operator=(const TFunctionRef&) = delete;
        ~TFunctionRef() = default;

        /**
         * @brief Invoke the referenced callable
         */
        OLO_FINLINE Ret operator()(ParamTypes... Params) const
        {
            return this->Invoke(std::forward<ParamTypes>(Params)...);
        }
    };

    // ========================================================================
    // TFunction - Owning copyable callable
    // ========================================================================

    /**
     * @class TFunction
     * @brief An owning, copyable wrapper for a callable object
     * 
     * Use TFunction when you need to store or return a callable. Unlike
     * TFunctionRef, the callable is owned by the TFunction.
     * 
     * Unlike TUniqueFunction, TFunction copies the callable, so it requires
     * the callable to be copyable (e.g., function pointers, copyable lambdas).
     * 
     * Example:
     * @code
     * TFunction<void(int)> Callback = [](int x) { ... };
     * Callback(42);
     * @endcode
     */
    template <typename Ret, typename... ParamTypes>
    class TFunction<Ret(ParamTypes...)> : private Private::TFunctionRefBase<Ret, ParamTypes...>
    {
    public:
        using typename Private::TFunctionRefBase<Ret, ParamTypes...>::CallerType;

        /**
         * @brief Default constructor (empty)
         */
        TFunction() = default;

        /**
         * @brief Constructor from a callable
         */
        template <typename FunctorType,
                  typename = std::enable_if_t<
                      !TIsTFunction_v<std::decay_t<FunctorType>> &&
                      !TIsTUniqueFunction_v<std::decay_t<FunctorType>> &&
                      std::is_invocable_r_v<Ret, std::decay_t<FunctorType>, ParamTypes...> &&
                      std::is_copy_constructible_v<std::decay_t<FunctorType>>
                  >>
        TFunction(FunctorType&& InFunc)
        {
            BindFunction(std::forward<FunctorType>(InFunc));
        }

        TFunction(const TFunction& Other)
        {
            if (Other.m_OwnedObject)
            {
                m_OwnedObject = std::make_unique<Private::TFunctionOwnedObject<std::remove_cvref_t<decltype(*Other.m_OwnedObject)>>>(
                    *Other.m_OwnedObject
                );
                this->Ptr = m_OwnedObject->GetCallable();
            }
            this->Callable = Other.Callable;
        }

        TFunction& operator=(const TFunction& Other)
        {
            TFunction Temp(Other);
            Swap(*this, Temp);
            return *this;
        }

        TFunction(TFunction&&) = default;
        TFunction& operator=(TFunction&&) = default;
        ~TFunction() = default;

        /**
         * @brief Invoke the callable
         */
        OLO_FINLINE Ret operator()(ParamTypes... Params) const
        {
            return this->Invoke(std::forward<ParamTypes>(Params)...);
        }

        /**
         * @brief Check if callable is set
         */
        OLO_FINLINE explicit operator bool() const
        {
            return this->Callable != nullptr;
        }

    private:
        template <typename FunctorType>
        void BindFunction(FunctorType&& InFunc)
        {
            if constexpr (std::is_function_v<std::remove_pointer_t<FunctorType>>)
            {
                // Function pointer - store as pointer
                this->Callable = &Private::TFunctionRefCaller<std::decay_t<FunctorType>, Ret, ParamTypes...>::Call;
                this->Ptr = const_cast<void*>(static_cast<const void*>(std::addressof(InFunc)));
            }
            else
            {
                // Functor/lambda - store in owned object
                using OwnedType = Private::TFunctionOwnedObject<std::remove_cvref_t<FunctorType>>;
                m_OwnedObject = std::make_unique<OwnedType>(std::forward<FunctorType>(InFunc));
                this->Callable = &Private::TFunctionRefCaller<std::remove_cvref_t<FunctorType>, Ret, ParamTypes...>::Call;
                this->Ptr = m_OwnedObject->GetCallable();
            }
        }

        friend void Swap(TFunction& A, TFunction& B)
        {
            std::swap(A.m_OwnedObject, B.m_OwnedObject);
            std::swap(A.Callable, B.Callable);
            std::swap(A.Ptr, B.Ptr);
        }

        std::unique_ptr<Private::IFunctionOwnedObject> m_OwnedObject;
    };

    // ========================================================================
    // TUniqueFunction - Move-only owning callable
    // ========================================================================

    /**
     * @class TUniqueFunction
     * @brief A move-only, owning wrapper for a callable object
     * 
     * Use TUniqueFunction when you need to store a callable that is not
     * copyable (e.g., move-only lambdas, std::unique_ptr captures).
     * 
     * Unlike TFunction, TUniqueFunction does not require the callable to
     * be copyable, but the TUniqueFunction itself cannot be copied.
     * 
     * Example:
     * @code
     * TUniqueFunction<void(int)> Callback = [ptr = std::make_unique<int>(42)](int x) { ... };
     * Callback(10);
     * // Cannot copy Callback - it's move-only
     * @endcode
     */
    template <typename Ret, typename... ParamTypes>
    class TUniqueFunction<Ret(ParamTypes...)> : private Private::TFunctionRefBase<Ret, ParamTypes...>
    {
    public:
        using typename Private::TFunctionRefBase<Ret, ParamTypes...>::CallerType;

        /**
         * @brief Default constructor (empty)
         */
        TUniqueFunction() = default;

        /**
         * @brief Constructor from a callable
         */
        template <typename FunctorType,
                  typename = std::enable_if_t<
                      !TIsTUniqueFunction_v<std::decay_t<FunctorType>> &&
                      std::is_invocable_r_v<Ret, std::decay_t<FunctorType>, ParamTypes...>
                  >>
        TUniqueFunction(FunctorType&& InFunc)
        {
            BindFunction(std::forward<FunctorType>(InFunc));
        }

        TUniqueFunction(const TUniqueFunction&) = delete;
        TUniqueFunction& operator=(const TUniqueFunction&) = delete;
        TUniqueFunction(TUniqueFunction&&) = default;
        TUniqueFunction& operator=(TUniqueFunction&&) = default;
        ~TUniqueFunction() = default;

        /**
         * @brief Invoke the callable
         */
        OLO_FINLINE Ret operator()(ParamTypes... Params) const
        {
            return this->Invoke(std::forward<ParamTypes>(Params)...);
        }

        /**
         * @brief Check if callable is set
         */
        OLO_FINLINE explicit operator bool() const
        {
            return this->Callable != nullptr;
        }

    private:
        template <typename FunctorType>
        void BindFunction(FunctorType&& InFunc)
        {
            using OwnedType = Private::TFunctionOwnedObject<std::remove_cvref_t<FunctorType>>;
            m_OwnedObject = std::make_unique<OwnedType>(std::forward<FunctorType>(InFunc));
            this->Callable = &Private::TFunctionRefCaller<std::remove_cvref_t<FunctorType>, Ret, ParamTypes...>::Call;
            this->Ptr = m_OwnedObject->GetCallable();
        }

        std::unique_ptr<Private::IFunctionOwnedObject> m_OwnedObject;
    };

    // ========================================================================
    // Deduction Guides
    // ========================================================================

    template <typename Ret, typename... Args>
    TFunctionRef(Ret(*)(Args...)) -> TFunctionRef<Ret(Args...)>;

    template <typename Ret, typename... Args>
    TFunction(Ret(*)(Args...)) -> TFunction<Ret(Args...)>;

    template <typename Ret, typename... Args>
    TUniqueFunction(Ret(*)(Args...)) -> TUniqueFunction<Ret(Args...)>;

} // namespace OloEngine
