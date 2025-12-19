#pragma once

// @file FunctionWithContext.h
// @brief Callable adapter that separates function pointer and context
// 
// Ported from Unreal Engine's Templates/FunctionWithContext.h

#include "OloEngine/Templates/Invoke.h"

#include <type_traits>

namespace OloEngine
{

// @class TFunctionWithContext
// @brief Type that adapts a callable into a function pointer with a context pointer.
//
// This does not take ownership of the callable. If constructed with a lambda,
// it is only valid until the lambda goes out of scope. Use TFunction to take
// ownership of the lambda.
//
// This behaves like a nullable TFunctionRef with the addition of accessors to
// pass on the function and context pointers to implementation functions. This
// tends to generate more efficient code than when passing a TFunctionRef or a
// TFunctionWithContext either by value or by reference.
//
// A function taking a string and float and returning int32 usage might be:
//
// @code
// void ParseLines(std::string_view View, void (*Visitor)(void* Context, std::string_view Line), void* Context);
// 
// inline void ParseLines(std::string_view View, TFunctionWithContext<void (std::string_view Line)> Visitor)
// {
//     ParseLines(View, Visitor.GetFunction(), Visitor.GetContext());
// }
//
// The example ParseLines can be called as:
//
// ParseLines(Input, [](std::string_view Line) { PrintLine(Line); });
// @endcode
template <typename FunctionType>
class TFunctionWithContext;

template <typename ReturnType, typename... ArgTypes>
class TFunctionWithContext<ReturnType (ArgTypes...)>
{
public:
	using FunctionType = ReturnType (void*, ArgTypes...);

	// Construct from a lambda or a function pointer.
	template <typename LambdaType>
	TFunctionWithContext(LambdaType&& Lambda)
		requires (!std::is_same_v<std::decay_t<LambdaType>, TFunctionWithContext>) &&
			std::is_invocable_r_v<ReturnType, std::decay_t<LambdaType>, ArgTypes...>
		: Function(&Call<std::decay_t<LambdaType>>)
		, Context(&Lambda)
	{
	}

	// Assign from a lambda or a function pointer.
	template <typename LambdaType>
	TFunctionWithContext& operator=(LambdaType&& Lambda)
		requires (!std::is_same_v<std::decay_t<LambdaType>, TFunctionWithContext>) &&
			std::is_invocable_r_v<ReturnType, std::decay_t<LambdaType>, ArgTypes...>
	{
		Function = &Call<std::decay_t<LambdaType>>;
		Context = &Lambda;
		return *this;
	}

	// Construct from a function pointer and context. Function and context may both be null.
	explicit TFunctionWithContext(FunctionType* InFunction, void* InContext)
		: Function(InFunction)
		, Context(InContext)
	{
	}

	// Construct a null function with null context.
	constexpr TFunctionWithContext(decltype(nullptr))
	{
	}

	// Default constructor - null function.
	constexpr TFunctionWithContext() = default;

	// Returns true if the function is non-null.
	explicit operator bool() const
	{
		return !!Function;
	}

	// Calls the function with the stored context and provided arguments. Function must be non-null.
	ReturnType operator()(ArgTypes... Args) const
	{
		return Function(Context, static_cast<ArgTypes&&>(Args)...);
	}

	[[nodiscard]] FunctionType* GetFunction() const
	{
		return Function;
	}

	[[nodiscard]] void* GetContext() const
	{
		return Context;
	}

private:
	template <typename LambdaType>
	static ReturnType Call(void* Lambda, ArgTypes... Args)
	{
		return Invoke(*static_cast<LambdaType*>(Lambda), static_cast<ArgTypes&&>(Args)...);
	}

	FunctionType* Function = nullptr;
	void* Context = nullptr;
};

} // namespace OloEngine
