#pragma once

#include <type_traits>
#include <vector>
#include <array>
#include <variant>
#include <optional>
#include <iostream>

namespace OloEngine::Core::Reflection {

	//==============================================================================
	/// Member pointer type extraction utilities
	namespace MemberPointer {
		namespace Impl {
			template <typename T>
			struct ReturnTypeFunction;

			template <typename Object, typename Return, typename... Args>
			struct ReturnTypeFunction<Return(Object::*)(Args...)> { using Type = Return; };

			template <typename Object, typename Return, typename... Args>
			struct ReturnTypeFunction<Return(Object::*)(Args...) const> { using Type = Return; };

			template <typename T>
			struct ReturnTypeObject;

			template <typename Object, typename Return>
			struct ReturnTypeObject<Return Object::*> { using Type = Return; };
		}

		template<typename T>
		struct ReturnType;

		template<typename T>
		struct ReturnType : std::enable_if_t<std::is_member_pointer_v<T>,
			std::conditional_t<std::is_member_object_pointer_v<T>,
								Impl::ReturnTypeObject<T>,
								Impl::ReturnTypeFunction<T>>>
		{
		};
	}

	//==============================================================================
	/// Check if a template is specialized
	template <typename, typename = void>
	struct IsSpecialized : std::false_type {};

	template<typename T>
	struct IsSpecialized<T, std::void_t<decltype(T{})>> : std::true_type {};

	template<typename T>
	constexpr bool IsSpecialized_v = IsSpecialized<T>::value;

	//==============================================================================
	/// Type filtering utilities
	struct FilterVoidAlt {};

	template<class T, class Alternative = FilterVoidAlt>
	struct FilterVoid
	{
		using Type = std::conditional_t<std::is_void_v<T>, Alternative, T>;
	};

	template<class T>
	using FilterVoid_t = typename FilterVoid<T>::Type;

	//==============================================================================
	/// Array detection utilities
	namespace ArrayImpl {
		template <typename T>					struct IsArrayImpl : std::false_type {};
		template <typename T, std::size_t N>	struct IsArrayImpl<std::array<T, N>> : std::true_type {};
		template <typename... Args>				struct IsArrayImpl<std::vector<Args...>> : std::true_type {};
	}

	template<typename T>
	struct IsArray
	{
		static constexpr bool Value = ArrayImpl::IsArrayImpl<std::decay_t<T>>::value;
	};

	template<typename T>
	inline constexpr bool IsArray_v = IsArray<T>::Value;

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
		static constexpr bool Value = decltype(Test<T>(0))::value;
	};

	template<class T>
	inline constexpr bool IsStreamable_v = IsStreamable<T>::Value;

	//==============================================================================
	/// Nth element extraction from parameter pack
	namespace Impl {
		template<size_t... Ns, typename... Args>
		constexpr auto NthElementUnroll(std::index_sequence<Ns...>, Args... args)
		{
			return [](decltype((void*)Ns)..., auto* nth, auto*...)
			{
				return *nth;
			}(&args...);
		}
	}

	template<auto N, typename... Args>
	constexpr auto NthElement(Args... args)
	{
		return Impl::NthElementUnroll(std::make_index_sequence<N>(), args...);
	}

} // namespace OloEngine::Core::Reflection