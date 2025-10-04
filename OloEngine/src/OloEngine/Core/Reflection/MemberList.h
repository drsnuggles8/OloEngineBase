#pragma once

#include "TypeUtils.h"
#include <tuple>
#include <variant>
#include <optional>
#include <type_traits>
#include <functional>

namespace OloEngine::Core::Reflection {

	//==============================================================================
	/// Utility wrapper to operate on a list of member pointers
	template<auto... MemberPointers>
	struct MemberList
	{
		//==============================================================================
		/// Helper type definitions

	public:
		using TupleType = decltype(std::tuple(MemberPointers...));

	private:
		template<sizet MemberIndex>
		using MemberType = typename MemberPointer::ReturnType<std::remove_cvref_t<decltype(std::get<MemberIndex>(TupleType()))>>::Type;

		template<typename TMemberPtr>
		using MemberPtrType = typename MemberPointer::ReturnType<std::remove_cvref_t<TMemberPtr>>::Type;

	public:
		using VariantType = std::variant<FilterVoid_t<MemberPtrType<decltype(MemberPointers)>>...>;

	public:
		static constexpr sizet Count() { return sizeof...(MemberPointers); }

		//==============================================================================
		/// Apply functions to member pointers

	private:
		/** Internal helper for Apply that forwards const-ness properly */
		template<typename TObj, typename TFunc>
		static constexpr auto ApplyImpl(TFunc func, TObj&& obj)
		{
			using result_t = std::invoke_result_t<TFunc, decltype(std::forward_like<TObj>(obj.*MemberPointers))...>;
			if constexpr (std::is_void_v<result_t>)
			{
				std::invoke(func, std::forward_like<TObj>(obj.*MemberPointers)...);
			}
			else
			{
				return std::invoke(func, std::forward_like<TObj>(obj.*MemberPointers)...);
			}
		}

	public:
		/** Apply a function to variadic pack of the member list */
		template<typename TObj, typename TFunc>
		static constexpr auto Apply(TFunc func, TObj& obj)
		{
			return ApplyImpl(func, obj);
		}

		/** Apply a function to variadic pack of the member list (const version) */
		template<typename TObj, typename TFunc>
		static constexpr auto Apply(TFunc func, const TObj& obj)
		{
			return ApplyImpl(func, obj);
		}

		/** Apply a function to each member that's not a member function */
		template<typename TObj, typename TFunc>
		static constexpr void ApplyForEach(TFunc func, TObj& obj)
		{
			(ApplyIfMemberNotFunction(func, MemberPointers, obj), ...);
		}

		/** Apply function to default initialized variables for each member type.
			This version does not require instance of the object of the described type.
			@param f - function to apply for each member type
			@return function return type
		*/
		template<typename TFunc>
		static constexpr auto ApplyToStaticType(TFunc f)
		{
			using result_t = std::invoke_result_t<TFunc, decltype(MemberPointers)...>;
			if constexpr (std::is_void_v<result_t>)
			{
				std::invoke(f, MemberPointers...);
			}
			else
			{
				return std::invoke(f, MemberPointers...);
			}
		}

	private:
		template<typename TFunc, typename TMemberPtr, typename TObj>
		static constexpr auto ApplyIfMemberNotFunction(TFunc func, TMemberPtr member, TObj&& obj)
		{
			if constexpr (!std::is_member_function_pointer_v<decltype(member)>)
			{
				func(obj.*member);
			}
		}

	public:
		//==============================================================================
		/// Member access by index

		template<typename TObj, typename TFunc>
		static constexpr void ApplyToMember(sizet memberIndex, TFunc&& f, TObj&& obj)
		{
			sizet memberCounter = 0;

			// Iterate the parameter pack directly and advance the counter for every element.
			// Only invoke the callback for data members; skip member-function pointers but
			// still increment the counter so indices remain aligned with the original pack.
			(([
				&]()
				{
					using MemberPtrT = decltype(MemberPointers);
					if (memberCounter == memberIndex)
					{
						if constexpr (!std::is_member_function_pointer_v<MemberPtrT>)
						{
							f(obj.*MemberPointers);
						}
					}
					++memberCounter;
				}()), ...);
		}

		template<sizet MemberIndex, typename TObj, typename TFunc>
		constexpr static void ApplyToMember(TFunc&& f, TObj&& obj)
		{
			f(obj.*NthElement<MemberIndex>(MemberPointers...));
		}

		//==============================================================================
		/// Member value getters/setters

	private:
		/** Common logic for setting member values with type checking */
		template<typename TValue, typename TMember>
		static constexpr bool TrySetMemberValue(TMember& member, const TValue& value)
		{
			using TMemberNoCVR = std::remove_cvref_t<TMember>;
			using TValueNoCVR = std::remove_cvref_t<TValue>;

			if constexpr (std::is_same_v<TValueNoCVR, TMemberNoCVR> && std::is_assignable_v<TMemberNoCVR&, TValueNoCVR>)
			{
				member = value;
				return true;
			}
			else
			{
				return false;
			}
		}

	public:
		template<typename TValue, typename TObj>
		static constexpr bool SetMemberValue(sizet memberIndex, const TValue& value, TObj&& obj)
		{
			bool valueSet = false;

			ApplyToMember(memberIndex,
				[&](auto& memb)
				{
					valueSet = TrySetMemberValue(memb, value);
				}, std::forward<decltype(obj)>(obj));

			return valueSet;
		}

		template<sizet MemberIndex, typename TValue, typename TObj>
		static constexpr bool SetMemberValue(const TValue& value, TObj&& obj)
		{
			bool valueSet = false;

			ApplyToMember<MemberIndex>(
				[&](auto& memb)
				{
					valueSet = TrySetMemberValue(memb, value);
				}, std::forward<decltype(obj)>(obj));

			return valueSet;
		}

		template<sizet MemberIndex, typename TObj>
		static constexpr auto GetMemberValue(const TObj& obj)
		{
			static_assert(Count() > MemberIndex);

			using MemberPtr = decltype(NthElement<MemberIndex>(MemberPointers...));
			
			if constexpr (std::is_member_function_pointer_v<MemberPtr>)
			{
				// For member function pointers, return void (don't define the lambda)
				return;
			}
			else
			{
				// For data members, define lambda and return its result
				auto filter = [&obj](auto member)
				{
					return obj.*member;
				};
				return filter(NthElement<MemberIndex>(MemberPointers...));
			}
		}

		template<typename TValue, typename TObj>
		static constexpr std::optional<TValue> GetMemberValueOfType(sizet memberIndex, const TObj& obj)
		{
			std::optional<TValue> value{};

			if (Count() > memberIndex)
			{
				ApplyToMember(memberIndex,
					[&](const auto& memb)
					{
						using TMember = std::remove_cvref_t<decltype(memb)>;

						if constexpr (std::is_same_v<TValue, TMember>)
						{
							value = memb;
						}
					}, obj);
			}

			return value;
		}

		//==============================================================================
		/// Type information queries

		template<sizet MemberIndex>
		static constexpr bool IsFunction()
		{
			static_assert(Count() > MemberIndex);
			return std::is_member_function_pointer_v<decltype(NthElement<MemberIndex>(MemberPointers...))>;
		}

		static constexpr std::optional<bool> IsFunction(sizet memberIndex)
		{
			std::optional<bool> isFunction;

			if (Count() > memberIndex)
			{
				sizet memberCounter = 0;
				auto unwrap = [&isFunction, memberIndex](auto memb, sizet counter)
				{
					if (counter == memberIndex)
					{
						isFunction = std::is_member_function_pointer_v<decltype(memb)>;
					}
				};

				(unwrap(MemberPointers, memberCounter++), ...);
			}

			return isFunction;
		}

		template<sizet MemberIndex>
		static constexpr auto GetMemberSize()
		{
			return sizeof(FilterVoid_t<MemberType<MemberIndex>>);
		}

		static constexpr std::optional<sizet> GetMemberSize(sizet memberIndex)
		{
			std::optional<sizet> size;

			if (Count() > memberIndex)
			{
				sizet memberCounter = 0;
				auto unwrap = [&size, memberIndex](auto memb, sizet counter)
				{
					if (counter == memberIndex)
					{
						size = sizeof(FilterVoid_t<MemberPtrType<decltype(memb)>>);
					}
				};

				(unwrap(MemberPointers, memberCounter++), ...);
			}

			return size;
		}
	};

} // namespace OloEngine::Core::Reflection