#pragma once

// @file RetainedRef.h
// @brief A helper class to prevent dangling references when storing references
//
// TRetainedRef<T> replaces T& as a function parameter when the reference is
// intended to be retained by the function (e.g. as a class member). The benefit
// of this class is that it is a compile error to pass an rvalue which might otherwise
// bind to a const reference, which is dangerous when the reference is retained.
//
// Ported from Unreal Engine's Templates/RetainedRef.h
//
// Example:
// @code
// struct FRaiiType
// {
//     explicit FRaiiType(TRetainedRef<const FThing> InThing)
//         : Ref(InThing)
//     {
//     }
//
//     void DoSomething()
//     {
//         Ref.Something();
//     }
//
//     FThing& Ref;
// };
//
// FThing Thing(...);
// FRaiiType Raii1(Thing);       // Compiles
// Raii1.DoSomething();           // Fine
//
// FRaiiType Raii2(FThing(...)); // Compile error!
// @endcode

namespace OloEngine
{
    // @class TRetainedRef
    // @brief A helper class to prevent dangling references for non-const types
    //
    // Prevents construction from rvalue references which would dangle.
    //
    // @tparam T The type to hold a reference to
    template<typename T>
    struct TRetainedRef
    {
        TRetainedRef(T& InRef)
            : Ref(InRef)
        {
        }

        // Can't construct a non-const reference with a const reference
        // and can't retain an rvalue reference.
        TRetainedRef(const T& InRef) = delete;
        TRetainedRef(T&& InRef) = delete;
        TRetainedRef(const T&& InRef) = delete;

        operator T&() const
        {
            return Ref;
        }

        T& Get() const
        {
            return Ref;
        }

      private:
        T& Ref;
    };

    // @brief Specialization for const types
    //
    // Allows construction from both const and non-const lvalue references,
    // but still prevents construction from rvalue references.
    template<typename T>
    struct TRetainedRef<const T>
    {
        TRetainedRef(T& InRef)
            : Ref(InRef)
        {
        }

        TRetainedRef(const T& InRef)
            : Ref(InRef)
        {
        }

        // Can't retain an rvalue reference.
        TRetainedRef(T&& InRef) = delete;
        TRetainedRef(const T&& InRef) = delete;

        operator const T&() const
        {
            return Ref;
        }

        const T& Get() const
        {
            return Ref;
        }

      private:
        const T& Ref;
    };

} // namespace OloEngine
