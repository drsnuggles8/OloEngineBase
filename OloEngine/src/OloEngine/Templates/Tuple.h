#pragma once

// @file Tuple.h
// @brief TTuple and TPair implementation for OloEngine
//
// A variadic tuple type similar to std::tuple, but with:
// - TPair as an alias for 2-element tuples with Key/Value member access
// - UE-style Get<Index>() and Get<Type>() accessors
// - ApplyAfter/ApplyBefore for functor application
// - MakeTuple, ForwardAsTuple, Tie, TransformTuple, VisitTupleElements
//
// Ported from Unreal Engine's Templates/Tuple.h

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Templates/IntegerSequence.h"
#include "OloEngine/Templates/Invoke.h"
#include <tuple>
#include <type_traits>

namespace OloEngine
{
    // Forward declarations
    template<typename... Types>
    struct TTuple;

    template<typename KeyType, typename ValueType>
    using TPair = TTuple<KeyType, ValueType>;

    template<typename... Types>
    constexpr TTuple<std::decay_t<Types>...> MakeTuple(Types&&... Args);

    // ========================================================================
    // Private implementation details
    // ========================================================================

    namespace Private::Tuple
    {
        enum EForwardingConstructor
        {
            ForwardingConstructor
        };
        enum EOtherTupleConstructor
        {
            OtherTupleConstructor
        };

        // Count occurrences of type T in parameter pack
        template<typename T, typename... Types>
        constexpr u32 TTypeCountInParameterPack_V = 0;

        template<typename T, typename U, typename... Types>
        constexpr u32 TTypeCountInParameterPack_V<T, U, Types...> = TTypeCountInParameterPack_V<T, Types...> + (std::is_same_v<T, U> ? 1 : 0);

        // ========================================================================
        // TTupleBaseElement - Storage for individual tuple elements
        // ========================================================================

        // General case: stores element as Value
        template<typename T, u32 Index, u32 TupleSize>
        struct TTupleBaseElement
        {
            template<typename ArgType>
            constexpr explicit TTupleBaseElement(EForwardingConstructor, ArgType&& Arg)
                : Value(Forward<ArgType>(Arg))
            {
            }

            constexpr TTupleBaseElement()
                : Value()
            {
            }

            TTupleBaseElement(TTupleBaseElement&&) = default;
            TTupleBaseElement(const TTupleBaseElement&) = default;
            TTupleBaseElement& operator=(TTupleBaseElement&&) = default;
            TTupleBaseElement& operator=(const TTupleBaseElement&) = default;

            T Value;
        };

        // Specialization for first element of 2-tuple: uses Key instead of Value (for TPair)
        template<typename T>
        struct TTupleBaseElement<T, 0, 2>
        {
            template<typename ArgType>
            constexpr explicit TTupleBaseElement(EForwardingConstructor, ArgType&& Arg)
                : Key(Forward<ArgType>(Arg))
            {
            }

            constexpr TTupleBaseElement()
                : Key()
            {
            }

            TTupleBaseElement(TTupleBaseElement&&) = default;
            TTupleBaseElement(const TTupleBaseElement&) = default;
            TTupleBaseElement& operator=(TTupleBaseElement&&) = default;
            TTupleBaseElement& operator=(const TTupleBaseElement&) = default;

            T Key;
        };

        // Specialization for second element of 2-tuple: already uses Value
        // (inherited from general case, but we need it explicit for TPair access)

        // ========================================================================
        // TTupleElementGetterByIndex - Retrieves elements by index
        // ========================================================================

        template<u32 Index, u32 TupleSize>
        struct TTupleElementGetterByIndex
        {
            template<typename DeducedType, typename TupleType>
            static inline decltype(auto) GetImpl(const volatile TTupleBaseElement<DeducedType, Index, TupleSize>& Element, TupleType&& Tuple)
            {
                // Brackets are important here - we want a reference type to be returned, not object type
                decltype(auto) Result = (ForwardAsBase<TupleType, TTupleBaseElement<DeducedType, Index, TupleSize>>(Tuple).Value);

                // Keep tuple rvalue references to rvalue reference elements as rvalues
                return static_cast<std::conditional_t<!std::is_reference_v<TupleType> && std::is_rvalue_reference_v<DeducedType>, DeducedType, decltype(Result)>>(Result);
            }

            template<typename TupleType>
            static OLO_FINLINE decltype(auto) Get(TupleType&& Tuple)
            {
                return GetImpl(Tuple, Forward<TupleType>(Tuple));
            }
        };

        // Specialization for first element of 2-tuple (accesses Key)
        template<>
        struct TTupleElementGetterByIndex<0, 2>
        {
            template<typename TupleType>
            static inline decltype(auto) Get(TupleType&& Tuple)
            {
                using KeyType = decltype(std::decay_t<TupleType>::Key);

                // Brackets are important here - we want a reference type to be returned, not object type
                decltype(auto) Result = (ForwardAsBase<TupleType, TTupleBaseElement<decltype(Tuple.Key), 0, 2>>(Tuple).Key);

                // Keep tuple rvalue references to rvalue reference elements as rvalues
                return static_cast<std::conditional_t<!std::is_reference_v<TupleType> && std::is_rvalue_reference_v<decltype(Tuple.Key)>, decltype(Tuple.Key), decltype(Result)>>(Result);
            }
        };

        // Specialization for second element of 2-tuple (accesses Value)
        template<>
        struct TTupleElementGetterByIndex<1, 2>
        {
            template<typename TupleType>
            static inline decltype(auto) Get(TupleType&& Tuple)
            {
                using ValueType = decltype(std::decay_t<TupleType>::Value);

                // Brackets are important here - we want a reference type to be returned, not object type
                decltype(auto) Result = (ForwardAsBase<TupleType, TTupleBaseElement<decltype(Tuple.Value), 1, 2>>(Tuple).Value);

                // Keep tuple rvalue references to rvalue reference elements as rvalues
                return static_cast<std::conditional_t<!std::is_reference_v<TupleType> && std::is_rvalue_reference_v<decltype(Tuple.Value)>, decltype(Tuple.Value), decltype(Result)>>(Result);
            }
        };

        // ========================================================================
        // TTupleElementGetterByType - Retrieves elements by type
        // ========================================================================

        template<typename Type, u32 TupleSize>
        struct TTupleElementGetterByType
        {
            template<u32 DeducedIndex, typename TupleType>
            static OLO_FINLINE decltype(auto) GetImpl(const volatile TTupleBaseElement<Type, DeducedIndex, TupleSize>&, TupleType&& Tuple)
            {
                return TTupleElementGetterByIndex<DeducedIndex, TupleSize>::Get(Forward<TupleType>(Tuple));
            }

            template<typename TupleType>
            static OLO_FINLINE decltype(auto) Get(TupleType&& Tuple)
            {
                return GetImpl(Tuple, Forward<TupleType>(Tuple));
            }
        };

        // ========================================================================
        // FEqualityHelper / TLessThanHelper - Comparison helpers
        // ========================================================================

        template<u32 ArgCount, u32 ArgToCompare>
        struct FEqualityHelper
        {
            template<typename TupleType>
            OLO_FINLINE static bool Compare(const TupleType& Lhs, const TupleType& Rhs)
            {
                return Lhs.template Get<ArgToCompare>() == Rhs.template Get<ArgToCompare>() && FEqualityHelper<ArgCount, ArgToCompare + 1>::Compare(Lhs, Rhs);
            }
        };

        template<u32 ArgCount>
        struct FEqualityHelper<ArgCount, ArgCount>
        {
            template<typename TupleType>
            OLO_FINLINE static bool Compare(const TupleType& Lhs, const TupleType& Rhs)
            {
                return true;
            }
        };

        template<u32 NumArgs, u32 ArgToCompare = 0, bool Last = ArgToCompare + 1 == NumArgs>
        struct TLessThanHelper
        {
            template<typename TupleType>
            OLO_FINLINE static bool Do(const TupleType& Lhs, const TupleType& Rhs)
            {
                return Lhs.template Get<ArgToCompare>() < Rhs.template Get<ArgToCompare>() || (!(Rhs.template Get<ArgToCompare>() < Lhs.template Get<ArgToCompare>()) && TLessThanHelper<NumArgs, ArgToCompare + 1>::Do(Lhs, Rhs));
            }
        };

        template<u32 NumArgs, u32 ArgToCompare>
        struct TLessThanHelper<NumArgs, ArgToCompare, true>
        {
            template<typename TupleType>
            OLO_FINLINE static bool Do(const TupleType& Lhs, const TupleType& Rhs)
            {
                return Lhs.template Get<ArgToCompare>() < Rhs.template Get<ArgToCompare>();
            }
        };

        template<u32 NumArgs>
        struct TLessThanHelper<NumArgs, NumArgs, false>
        {
            template<typename TupleType>
            OLO_FINLINE static bool Do(const TupleType& Lhs, const TupleType& Rhs)
            {
                return false;
            }
        };

        // ========================================================================
        // TTupleBase - Common base for all tuples
        // ========================================================================

        template<typename Indices, typename... Types>
        struct TTupleBase;

        template<u32... Indices, typename... Types>
        struct TTupleBase<TIntegerSequence<u32, Indices...>, Types...> : TTupleBaseElement<Types, Indices, sizeof...(Types)>...
        {
            template<typename... ArgTypes>
            constexpr explicit TTupleBase(EForwardingConstructor, ArgTypes&&... Args)
                : TTupleBaseElement<Types, Indices, sizeof...(Types)>(ForwardingConstructor, Forward<ArgTypes>(Args))...
            {
            }

            template<typename TupleType>
            explicit TTupleBase(EOtherTupleConstructor, TupleType&& Other)
                : TTupleBaseElement<Types, Indices, sizeof...(Types)>(ForwardingConstructor, Forward<TupleType>(Other).template Get<Indices>())...
            {
            }

            constexpr TTupleBase() = default;
            TTupleBase(TTupleBase&& Other) = default;
            TTupleBase(const TTupleBase& Other) = default;
            TTupleBase& operator=(TTupleBase&& Other) = default;
            TTupleBase& operator=(const TTupleBase& Other) = default;

            // Get by index - all cv/ref qualifications
            template<u32 Index>
            OLO_FINLINE decltype(auto) Get() &
            {
                static_assert(Index < sizeof...(Types), "Invalid index passed to TTuple::Get");
                return TTupleElementGetterByIndex<Index, sizeof...(Types)>::Get(static_cast<TTupleBase&>(*this));
            }
            template<u32 Index>
            OLO_FINLINE decltype(auto) Get() const&
            {
                static_assert(Index < sizeof...(Types), "Invalid index passed to TTuple::Get");
                return TTupleElementGetterByIndex<Index, sizeof...(Types)>::Get(static_cast<const TTupleBase&>(*this));
            }
            template<u32 Index>
            OLO_FINLINE decltype(auto) Get() volatile&
            {
                static_assert(Index < sizeof...(Types), "Invalid index passed to TTuple::Get");
                return TTupleElementGetterByIndex<Index, sizeof...(Types)>::Get(static_cast<volatile TTupleBase&>(*this));
            }
            template<u32 Index>
            OLO_FINLINE decltype(auto) Get() const volatile&
            {
                static_assert(Index < sizeof...(Types), "Invalid index passed to TTuple::Get");
                return TTupleElementGetterByIndex<Index, sizeof...(Types)>::Get(static_cast<const volatile TTupleBase&>(*this));
            }
            template<u32 Index>
            OLO_FINLINE decltype(auto) Get() &&
            {
                static_assert(Index < sizeof...(Types), "Invalid index passed to TTuple::Get");
                return TTupleElementGetterByIndex<Index, sizeof...(Types)>::Get(static_cast<TTupleBase&&>(*this));
            }
            template<u32 Index>
            OLO_FINLINE decltype(auto) Get() const&&
            {
                static_assert(Index < sizeof...(Types), "Invalid index passed to TTuple::Get");
                return TTupleElementGetterByIndex<Index, sizeof...(Types)>::Get(static_cast<const TTupleBase&&>(*this));
            }
            template<u32 Index>
            OLO_FINLINE decltype(auto) Get() volatile&&
            {
                static_assert(Index < sizeof...(Types), "Invalid index passed to TTuple::Get");
                return TTupleElementGetterByIndex<Index, sizeof...(Types)>::Get(static_cast<volatile TTupleBase&&>(*this));
            }
            template<u32 Index>
            OLO_FINLINE decltype(auto) Get() const volatile&&
            {
                static_assert(Index < sizeof...(Types), "Invalid index passed to TTuple::Get");
                return TTupleElementGetterByIndex<Index, sizeof...(Types)>::Get(static_cast<const volatile TTupleBase&&>(*this));
            }

            // Get by type - all cv/ref qualifications
            template<typename T>
            OLO_FINLINE decltype(auto) Get() &
            {
                static_assert(TTypeCountInParameterPack_V<T, Types...> == 1, "Invalid type passed to TTuple::Get");
                return TTupleElementGetterByType<T, sizeof...(Types)>::Get(static_cast<TTupleBase&>(*this));
            }
            template<typename T>
            OLO_FINLINE decltype(auto) Get() const&
            {
                static_assert(TTypeCountInParameterPack_V<T, Types...> == 1, "Invalid type passed to TTuple::Get");
                return TTupleElementGetterByType<T, sizeof...(Types)>::Get(static_cast<const TTupleBase&>(*this));
            }
            template<typename T>
            OLO_FINLINE decltype(auto) Get() volatile&
            {
                static_assert(TTypeCountInParameterPack_V<T, Types...> == 1, "Invalid type passed to TTuple::Get");
                return TTupleElementGetterByType<T, sizeof...(Types)>::Get(static_cast<volatile TTupleBase&>(*this));
            }
            template<typename T>
            OLO_FINLINE decltype(auto) Get() const volatile&
            {
                static_assert(TTypeCountInParameterPack_V<T, Types...> == 1, "Invalid type passed to TTuple::Get");
                return TTupleElementGetterByType<T, sizeof...(Types)>::Get(static_cast<const volatile TTupleBase&>(*this));
            }
            template<typename T>
            OLO_FINLINE decltype(auto) Get() &&
            {
                static_assert(TTypeCountInParameterPack_V<T, Types...> == 1, "Invalid type passed to TTuple::Get");
                return TTupleElementGetterByType<T, sizeof...(Types)>::Get(static_cast<TTupleBase&&>(*this));
            }
            template<typename T>
            OLO_FINLINE decltype(auto) Get() const&&
            {
                static_assert(TTypeCountInParameterPack_V<T, Types...> == 1, "Invalid type passed to TTuple::Get");
                return TTupleElementGetterByType<T, sizeof...(Types)>::Get(static_cast<const TTupleBase&&>(*this));
            }
            template<typename T>
            OLO_FINLINE decltype(auto) Get() volatile&&
            {
                static_assert(TTypeCountInParameterPack_V<T, Types...> == 1, "Invalid type passed to TTuple::Get");
                return TTupleElementGetterByType<T, sizeof...(Types)>::Get(static_cast<volatile TTupleBase&&>(*this));
            }
            template<typename T>
            OLO_FINLINE decltype(auto) Get() const volatile&&
            {
                static_assert(TTypeCountInParameterPack_V<T, Types...> == 1, "Invalid type passed to TTuple::Get");
                return TTupleElementGetterByType<T, sizeof...(Types)>::Get(static_cast<const volatile TTupleBase&&>(*this));
            }

            // ApplyAfter - invoke function with additional args followed by tuple elements
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyAfter(FuncType&& Func, ArgTypes&&... Args) &
            {
                return Invoke(Forward<FuncType>(Func), Forward<ArgTypes>(Args)..., static_cast<TTupleBase&>(*this).template Get<Indices>()...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyAfter(FuncType&& Func, ArgTypes&&... Args) const&
            {
                return Invoke(Forward<FuncType>(Func), Forward<ArgTypes>(Args)..., static_cast<const TTupleBase&>(*this).template Get<Indices>()...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyAfter(FuncType&& Func, ArgTypes&&... Args) volatile&
            {
                return Invoke(Forward<FuncType>(Func), Forward<ArgTypes>(Args)..., static_cast<volatile TTupleBase&>(*this).template Get<Indices>()...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyAfter(FuncType&& Func, ArgTypes&&... Args) const volatile&
            {
                return Invoke(Forward<FuncType>(Func), Forward<ArgTypes>(Args)..., static_cast<const volatile TTupleBase&>(*this).template Get<Indices>()...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyAfter(FuncType&& Func, ArgTypes&&... Args) &&
            {
                return Invoke(Forward<FuncType>(Func), Forward<ArgTypes>(Args)..., static_cast<TTupleBase&&>(*this).template Get<Indices>()...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyAfter(FuncType&& Func, ArgTypes&&... Args) const&&
            {
                return Invoke(Forward<FuncType>(Func), Forward<ArgTypes>(Args)..., static_cast<const TTupleBase&&>(*this).template Get<Indices>()...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyAfter(FuncType&& Func, ArgTypes&&... Args) volatile&&
            {
                return Invoke(Forward<FuncType>(Func), Forward<ArgTypes>(Args)..., static_cast<volatile TTupleBase&&>(*this).template Get<Indices>()...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyAfter(FuncType&& Func, ArgTypes&&... Args) const volatile&&
            {
                return Invoke(Forward<FuncType>(Func), Forward<ArgTypes>(Args)..., static_cast<const volatile TTupleBase&&>(*this).template Get<Indices>()...);
            }

            // ApplyBefore - invoke function with tuple elements followed by additional args
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyBefore(FuncType&& Func, ArgTypes&&... Args) &
            {
                return Invoke(Forward<FuncType>(Func), static_cast<TTupleBase&>(*this).template Get<Indices>()..., Forward<ArgTypes>(Args)...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyBefore(FuncType&& Func, ArgTypes&&... Args) const&
            {
                return Invoke(Forward<FuncType>(Func), static_cast<const TTupleBase&>(*this).template Get<Indices>()..., Forward<ArgTypes>(Args)...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyBefore(FuncType&& Func, ArgTypes&&... Args) volatile&
            {
                return Invoke(Forward<FuncType>(Func), static_cast<volatile TTupleBase&>(*this).template Get<Indices>()..., Forward<ArgTypes>(Args)...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyBefore(FuncType&& Func, ArgTypes&&... Args) const volatile&
            {
                return Invoke(Forward<FuncType>(Func), static_cast<const volatile TTupleBase&>(*this).template Get<Indices>()..., Forward<ArgTypes>(Args)...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyBefore(FuncType&& Func, ArgTypes&&... Args) &&
            {
                return Invoke(Forward<FuncType>(Func), static_cast<TTupleBase&&>(*this).template Get<Indices>()..., Forward<ArgTypes>(Args)...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyBefore(FuncType&& Func, ArgTypes&&... Args) const&&
            {
                return Invoke(Forward<FuncType>(Func), static_cast<const TTupleBase&&>(*this).template Get<Indices>()..., Forward<ArgTypes>(Args)...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyBefore(FuncType&& Func, ArgTypes&&... Args) volatile&&
            {
                return Invoke(Forward<FuncType>(Func), static_cast<volatile TTupleBase&&>(*this).template Get<Indices>()..., Forward<ArgTypes>(Args)...);
            }
            template<typename FuncType, typename... ArgTypes>
            decltype(auto) ApplyBefore(FuncType&& Func, ArgTypes&&... Args) const volatile&&
            {
                return Invoke(Forward<FuncType>(Func), static_cast<const volatile TTupleBase&&>(*this).template Get<Indices>()..., Forward<ArgTypes>(Args)...);
            }

            // Comparison operators
            OLO_FINLINE bool operator==(const TTupleBase& Rhs) const
            {
                return FEqualityHelper<sizeof...(Types), 0>::Compare(*this, Rhs);
            }

            OLO_FINLINE bool operator!=(const TTupleBase& Rhs) const
            {
                return !(*this == Rhs);
            }

            OLO_FINLINE bool operator<(const TTupleBase& Rhs) const
            {
                return TLessThanHelper<sizeof...(Types)>::Do(*this, Rhs);
            }

            OLO_FINLINE bool operator<=(const TTupleBase& Rhs) const
            {
                return !(Rhs < *this);
            }

            OLO_FINLINE bool operator>(const TTupleBase& Rhs) const
            {
                return Rhs < *this;
            }

            OLO_FINLINE bool operator>=(const TTupleBase& Rhs) const
            {
                return !(*this < Rhs);
            }
        };

        // ========================================================================
        // Helper functions
        // ========================================================================

        template<typename LhsType, typename RhsType, u32... Indices>
        static void Assign(LhsType& Lhs, RhsType&& Rhs, TIntegerSequence<u32, Indices...>)
        {
            int Temp[] = { 0, (Lhs.template Get<Indices>() = Forward<RhsType>(Rhs).template Get<Indices>(), 0)... };
            (void)Temp;
        }

        template<typename... ElementTypes, typename... Types>
        OLO_FINLINE constexpr TTuple<ElementTypes...> MakeTupleImpl(Types&&... Args)
        {
            return TTuple<ElementTypes...>(Forward<Types>(Args)...);
        }

        template<typename IntegerSequence>
        struct TTransformTuple_Impl;

        template<u32... Indices>
        struct TTransformTuple_Impl<TIntegerSequence<u32, Indices...>>
        {
            template<typename TupleType, typename FuncType>
            static decltype(auto) Do(TupleType&& Tuple, FuncType Func)
            {
                return MakeTuple(Func(Forward<TupleType>(Tuple).template Get<Indices>())...);
            }
        };

        template<typename IntegerSequence>
        struct TVisitTupleElements_Impl;

        template<u32... Indices>
        struct TVisitTupleElements_Impl<TIntegerSequence<u32, Indices...>>
        {
            template<u32 Index, typename FuncType, typename... TupleTypes>
            OLO_FINLINE static void InvokeFunc(FuncType&& Func, TupleTypes&&... Tuples)
            {
                Invoke(Forward<FuncType>(Func), Forward<TupleTypes>(Tuples).template Get<Index>()...);
            }

            template<typename FuncType, typename... TupleTypes>
            static void Do(FuncType&& Func, TupleTypes&&... Tuples)
            {
                int Temp[] = { 0, (InvokeFunc<Indices>(Forward<FuncType>(Func), Forward<TupleTypes>(Tuples)...), 0)... };
                (void)Temp;
            }
        };

        // ========================================================================
        // Traits helpers (internal)
        // ========================================================================

        template<typename TupleType>
        struct TCVTupleArity;

        template<typename... Types>
        struct TCVTupleArity<const volatile TTuple<Types...>>
        {
            enum
            {
                Value = sizeof...(Types)
            };
        };

        template<typename Type, typename TupleType>
        struct TCVTupleIndex
        {
            static_assert(sizeof(TupleType) == 0, "TTupleIndex instantiated with a non-tuple type");
            static constexpr u32 Value = 0;
        };

        template<typename Type, typename... TupleTypes>
        struct TCVTupleIndex<Type, const volatile TTuple<TupleTypes...>>
        {
            static_assert(TTypeCountInParameterPack_V<Type, TupleTypes...> >= 1, "TTupleIndex instantiated with a tuple which does not contain the type");
            static_assert(TTypeCountInParameterPack_V<Type, TupleTypes...> <= 1, "TTupleIndex instantiated with a tuple which contains multiple instances of the type");

          private:
            template<u32 DeducedIndex>
            static auto Resolve(TTupleBaseElement<Type, DeducedIndex, sizeof...(TupleTypes)>*) -> char (&)[DeducedIndex + 1];
            static auto Resolve(...) -> char;

          public:
            static constexpr u32 Value = sizeof(Resolve(DeclVal<TTuple<TupleTypes...>*>())) - 1;
        };

        template<u32 Index, typename TupleType>
        struct TCVTupleElement
        {
            static_assert(sizeof(TupleType) == 0, "TTupleElement instantiated with a non-tuple type");
            using Type = void;
        };

        template<u32 Index, typename... TupleTypes>
        struct TCVTupleElement<Index, const volatile TTuple<TupleTypes...>>
        {
            static_assert(Index < sizeof...(TupleTypes), "TTupleElement instantiated with an invalid index");

#ifdef __clang__
            using Type = __type_pack_element<Index, TupleTypes...>;
#else
          private:
            template<typename DeducedType>
            static DeducedType Resolve(TTupleBaseElement<DeducedType, Index, sizeof...(TupleTypes)>*);
            static void Resolve(...);

          public:
            using Type = decltype(Resolve(DeclVal<TTuple<TupleTypes...>*>()));
#endif
        };

        // Hash helper
        template<u32 ArgToCombine, u32 ArgCount>
        struct TGetTupleHashHelper
        {
            template<typename TupleType>
            OLO_FINLINE static u32 Do(u32 Hash, const TupleType& Tuple)
            {
                return TGetTupleHashHelper<ArgToCombine + 1, ArgCount>::Do(HashCombine(Hash, GetTypeHash(Tuple.template Get<ArgToCombine>())), Tuple);
            }
        };

        template<u32 ArgIndex>
        struct TGetTupleHashHelper<ArgIndex, ArgIndex>
        {
            template<typename TupleType>
            OLO_FINLINE static u32 Do(u32 Hash, const TupleType& Tuple)
            {
                return Hash;
            }
        };

    } // namespace Private::Tuple

    // ========================================================================
    // TTuple - Main tuple class
    // ========================================================================

    template<typename... Types>
    struct TTuple : Private::Tuple::TTupleBase<TMakeIntegerSequence<u32, sizeof...(Types)>, Types...>
    {
      private:
        using Super = Private::Tuple::TTupleBase<TMakeIntegerSequence<u32, sizeof...(Types)>, Types...>;

      public:
        // Forwarding constructor from individual arguments
        template<typename... ArgTypes>
            requires((sizeof...(ArgTypes) > 0) && (std::is_constructible_v<Types, ArgTypes &&> && ...))
        constexpr explicit(!std::conjunction_v<std::is_convertible<ArgTypes&&, Types>...>) TTuple(ArgTypes&&... Args)
            : Super(Private::Tuple::ForwardingConstructor, Forward<ArgTypes>(Args)...)
        {
        }

        // Move constructor from other tuple types
        template<typename... OtherTypes>
            requires(std::is_constructible_v<Types, OtherTypes &&> && ...)
        TTuple(TTuple<OtherTypes...>&& Other)
            : Super(Private::Tuple::OtherTupleConstructor, MoveTemp(Other))
        {
        }

        // Copy constructor from other tuple types
        template<typename... OtherTypes>
            requires(std::is_constructible_v<Types, const OtherTypes&> && ...)
        TTuple(const TTuple<OtherTypes...>& Other)
            : Super(Private::Tuple::OtherTupleConstructor, Other)
        {
        }

        constexpr TTuple()
            requires(std::is_default_constructible_v<Types> && ...)
        = default;
        TTuple(TTuple&&) = default;
        TTuple(const TTuple&) = default;
        TTuple& operator=(TTuple&&) = default;
        TTuple& operator=(const TTuple&) = default;

        // Copy assignment from other tuple types
        template<typename... OtherTypes>
            requires(std::is_assignable_v<Types, const OtherTypes&> && ...)
        TTuple& operator=(const TTuple<OtherTypes...>& Other)
        {
            Private::Tuple::Assign(*this, Other, TMakeIntegerSequence<u32, sizeof...(Types)>{});
            return *this;
        }

        // Move assignment from other tuple types
        template<typename... OtherTypes>
            requires(std::is_assignable_v<Types, OtherTypes &&> && ...)
        TTuple& operator=(TTuple<OtherTypes...>&& Other)
        {
            Private::Tuple::Assign(*this, MoveTemp(Other), TMakeIntegerSequence<u32, sizeof...(OtherTypes)>{});
            return *this;
        }
    };

    // ========================================================================
    // Tuple traits
    // ========================================================================

    /**
     * @brief Traits class which calculates the number of elements in a tuple.
     */
    template<typename TupleType>
    struct TTupleArity : Private::Tuple::TCVTupleArity<const volatile TupleType>
    {
    };

    /**
     * @brief Traits class which gets the tuple index of a given type from a given TTuple.
     *
     * If the type doesn't appear, or appears more than once, a compile error is generated.
     *
     * Given Type = char, and Tuple = TTuple<int, float, char>,
     * TTupleIndex<Type, Tuple>::Value will be 2.
     */
    template<typename Type, typename TupleType>
    using TTupleIndex = Private::Tuple::TCVTupleIndex<Type, const volatile TupleType>;

    /**
     * @brief Traits class which gets the element type of a TTuple with the given index.
     *
     * If the index is out of range, a compile error is generated.
     *
     * Given Index = 1, and Tuple = TTuple<int, float, char>,
     * TTupleElement<Index, Tuple>::Type will be float.
     */
    template<u32 Index, typename TupleType>
    using TTupleElement = Private::Tuple::TCVTupleElement<Index, const volatile TupleType>;

    /**
     * @brief Type trait to check if a type is a TTuple
     */
    template<typename T>
    constexpr bool TIsTuple_V = false;

    template<typename... Types>
    constexpr bool TIsTuple_V<TTuple<Types...>> = true;
    template<typename... Types>
    constexpr bool TIsTuple_V<const TTuple<Types...>> = true;
    template<typename... Types>
    constexpr bool TIsTuple_V<volatile TTuple<Types...>> = true;
    template<typename... Types>
    constexpr bool TIsTuple_V<const volatile TTuple<Types...>> = true;

    template<typename T>
    struct TIsTuple
    {
        enum
        {
            Value = TIsTuple_V<T>
        };
    };

    // ========================================================================
    // GetTypeHash for TTuple
    // ========================================================================

    template<typename... Types>
    OLO_FINLINE u32 GetTypeHash(const TTuple<Types...>& Tuple)
    {
        return Private::Tuple::TGetTupleHashHelper<1u, sizeof...(Types)>::Do(GetTypeHash(Tuple.template Get<0>()), Tuple);
    }

    OLO_FINLINE u32 GetTypeHash(const TTuple<>& Tuple)
    {
        return 0;
    }

    // ========================================================================
    // MakeTuple - Create a tuple with decayed types
    // ========================================================================

    /**
     * @brief Makes a TTuple from some arguments. The type of the TTuple elements are the decayed versions of the arguments.
     *
     * @param Args The arguments used to construct the tuple.
     * @return A tuple containing a copy of the arguments.
     *
     * Example:
     *     void Func(const int32 A, FString&& B)
     *     {
     *         // Equivalent to:
     *         // TTuple<int32, const TCHAR*, FString> MyTuple(A, TEXT("Hello"), MoveTemp(B));
     *         auto MyTuple = MakeTuple(A, TEXT("Hello"), MoveTemp(B));
     *     }
     */
    template<typename... Types>
    OLO_FINLINE constexpr TTuple<std::decay_t<Types>...> MakeTuple(Types&&... Args)
    {
        return Private::Tuple::MakeTupleImpl<std::decay_t<Types>...>(Forward<Types>(Args)...);
    }

    // ========================================================================
    // ForwardAsTuple - Create a tuple of references
    // ========================================================================

    /**
     * @brief Makes a TTuple from some arguments. Unlike MakeTuple, the TTuple element types are references
     * and retain the same value category of the arguments, like the Forward function.
     *
     * @param Args The arguments used to construct the tuple.
     * @return A tuple containing forwarded references to the arguments.
     *
     * Example:
     *     template <typename... Ts>
     *     void Foo(const TTuple<Ts...>&);
     *
     *     void Func(const int32 A, FString&& B)
     *     {
     *         // Calls Foo<const int32&, const TCHAR(&)[6], FString&&>(...);
     *         Foo(ForwardAsTuple(A, TEXT("Hello"), MoveTemp(B)));
     *     }
     */
    template<typename... Types>
    OLO_FINLINE TTuple<Types&&...> ForwardAsTuple(Types&&... Args)
    {
        return Private::Tuple::MakeTupleImpl<Types&&...>(Forward<Types>(Args)...);
    }

    // ========================================================================
    // TransformTuple - Apply a functor to each element
    // ========================================================================

    /**
     * @brief Creates a new TTuple by applying a functor to each of the elements.
     *
     * @param Tuple The tuple to apply the functor to.
     * @param Func The functor to apply.
     * @return A new tuple of the transformed elements.
     *
     * Example:
     *     float        Overloaded(int32 Arg);
     *     char         Overloaded(const TCHAR* Arg);
     *     const TCHAR* Overloaded(const FString& Arg);
     *
     *     void Func(const TTuple<int32, const TCHAR*, FString>& MyTuple)
     *     {
     *         auto TransformedTuple = TransformTuple(MyTuple, [](const auto& Arg) { return Overloaded(Arg); });
     *     }
     */
    template<typename FuncType, typename... Types>
    OLO_FINLINE decltype(auto) TransformTuple(TTuple<Types...>&& Tuple, FuncType Func)
    {
        return Private::Tuple::TTransformTuple_Impl<TMakeIntegerSequence<u32, sizeof...(Types)>>::Do(MoveTemp(Tuple), MoveTemp(Func));
    }

    template<typename FuncType, typename... Types>
    OLO_FINLINE decltype(auto) TransformTuple(const TTuple<Types...>& Tuple, FuncType Func)
    {
        return Private::Tuple::TTransformTuple_Impl<TMakeIntegerSequence<u32, sizeof...(Types)>>::Do(Tuple, MoveTemp(Func));
    }

    // ========================================================================
    // VisitTupleElements - Visit each element in parallel tuples
    // ========================================================================

    /**
     * @brief Visits each element in the specified tuples in parallel and applies them as arguments to the functor.
     *
     * All specified tuples must have the same number of elements.
     *
     * @param Func The functor to apply.
     * @param Tuples The tuples whose elements are to be applied to the functor.
     *
     * Example:
     *     void Func(const TTuple<int32, const TCHAR*, FString>& Tuple1, const TTuple<bool, float, FName>& Tuple2)
     *     {
     *         // Equivalent to:
     *         // Functor(Tuple1.Get<0>(), Tuple2.Get<0>());
     *         // Functor(Tuple1.Get<1>(), Tuple2.Get<1>());
     *         // Functor(Tuple1.Get<2>(), Tuple2.Get<2>());
     *         VisitTupleElements(Functor, Tuple1, Tuple2);
     *     }
     */
    template<typename FuncType, typename FirstTupleType, typename... TupleTypes>
    OLO_FINLINE void VisitTupleElements(FuncType&& Func, FirstTupleType&& FirstTuple, TupleTypes&&... Tuples)
    {
        Private::Tuple::TVisitTupleElements_Impl<TMakeIntegerSequence<u32, TTupleArity<std::decay_t<FirstTupleType>>::Value>>::Do(Forward<FuncType>(Func), Forward<FirstTupleType>(FirstTuple), Forward<TupleTypes>(Tuples)...);
    }

    // ========================================================================
    // Tie - Structured unpacking of tuples into variables
    // ========================================================================

    /**
     * @brief Tie function for structured unpacking of tuples into individual variables.
     *
     * Example:
     *     TTuple<FString, float, TArray<int32>> SomeFunction();
     *
     *     FString Ret1;
     *     float Ret2;
     *     TArray<int32> Ret3;
     *
     *     Tie(Ret1, Ret2, Ret3) = SomeFunction();
     *
     *     // Now Ret1, Ret2 and Ret3 contain the unpacked return values.
     */
    template<typename... Types>
    OLO_FINLINE TTuple<Types&...> Tie(Types&... Args)
    {
        return TTuple<Types&...>(Args...);
    }

    // ========================================================================
    // ADL-based get<> for structured bindings
    // ========================================================================

    /**
     * @brief Get element by index for structured bindings (ADL lookup)
     */
    template<std::size_t N, typename TupleType>
        requires(TIsTuple_V<std::decay_t<TupleType>>)
    decltype(auto) get(TupleType&& val)
    {
        return ((TupleType&&)val).template Get<N>();
    }

} // namespace OloEngine

// ============================================================================
// Structured binding support (must be in std namespace)
// ============================================================================

template<typename... ArgTypes>
struct std::tuple_size<OloEngine::TTuple<ArgTypes...>>
    : std::integral_constant<std::size_t, sizeof...(ArgTypes)>
{
};

template<std::size_t N, typename... ArgTypes>
struct std::tuple_element<N, OloEngine::TTuple<ArgTypes...>>
{
  public:
    using type = typename OloEngine::TTupleElement<N, OloEngine::TTuple<ArgTypes...>>::Type;
};
