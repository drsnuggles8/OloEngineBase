// StaticArray.h - Static-sized array container
// Ported from UE5.7 Containers/StaticArray.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Assert.h"
#include "OloEngine/Templates/IntegerSequence.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Templates/TypeHash.h"
#include "OloEngine/Containers/ReverseIterate.h"

#include <type_traits>

namespace OloEngine
{

namespace Private
{
	// Workaround for parsing issues with fold expressions in constraints
	template <typename InElementType, typename... ArgTypes>
	constexpr bool TCanBeConvertedToFromAll_V = (std::is_convertible_v<ArgTypes, InElementType> && ...);
} // namespace Private

/** Tag for in-place construction. */
struct EInPlace {};
inline constexpr EInPlace InPlace{};

/** Tag for per-element initialization. */
struct EPerElement {};
inline constexpr EPerElement PerElement{};

/** An array with a static number of elements. */
template <typename InElementType, u32 NumElements>
class TStaticArray
{
public:
	using ElementType = InElementType;

	InElementType Elements[NumElements];

	[[nodiscard]] constexpr TStaticArray() = default;

	// Constructs each element with Args
	template <typename... ArgTypes>
	[[nodiscard]] constexpr explicit TStaticArray(EInPlace, ArgTypes&&... Args)
		: TStaticArray{InPlace, TMakeIntegerSequence<u32, NumElements>(), [&Args...](u32) { return InElementType(Args...); }}
	{
	}

	// Directly initializes the array with the provided values.
	template <typename... ArgTypes>
		requires((sizeof...(ArgTypes) > 0 && sizeof...(ArgTypes) <= NumElements) && Private::TCanBeConvertedToFromAll_V<InElementType, ArgTypes...>)
	[[nodiscard]] constexpr TStaticArray(ArgTypes&&... Args)
		: TStaticArray{PerElement, [&Args] { return InElementType(Forward<ArgTypes>(Args)); }...}
	{
	}

	[[nodiscard]] constexpr TStaticArray(TStaticArray&& Other) = default;
	[[nodiscard]] constexpr TStaticArray(const TStaticArray& Other) = default;
	constexpr TStaticArray& operator=(TStaticArray&& Other) = default;
	constexpr TStaticArray& operator=(const TStaticArray& Other) = default;

	// Accessors.
	[[nodiscard]] constexpr InElementType& operator[](u32 Index)
	{
		OLO_CORE_CHECK_SLOW(Index < NumElements);
		return Elements[Index];
	}

	[[nodiscard]] constexpr const InElementType& operator[](u32 Index) const
	{
		OLO_CORE_CHECK_SLOW(Index < NumElements);
		return Elements[Index];
	}

	[[nodiscard]] bool operator==(const TStaticArray&) const = default;

	/**
	 * Returns true if the array is empty and contains no elements. 
	 *
	 * @returns True if the array is empty.
	 * @see Num
	 */
	[[nodiscard]] constexpr bool IsEmpty() const
	{
		return NumElements == 0;
	}

	/** The number of elements in the array. */
	[[nodiscard]] constexpr i32 Num() const
	{
		return NumElements;
	}

	/** A pointer to the first element of the array */
	[[nodiscard]] constexpr InElementType* GetData()
	{
		return Elements;
	}
	
	[[nodiscard]] constexpr const InElementType* GetData() const
	{
		return Elements;
	}

	[[nodiscard]] constexpr InElementType*                               begin ()       { return Elements; }
	[[nodiscard]] constexpr const InElementType*                         begin () const { return Elements; }
	[[nodiscard]] constexpr InElementType*                               end   ()       { return Elements + NumElements; }
	[[nodiscard]] constexpr const InElementType*                         end   () const { return Elements + NumElements; }
	[[nodiscard]] constexpr TReversePointerIterator<InElementType>       rbegin()       { return TReversePointerIterator<InElementType>(Elements + NumElements); }
	[[nodiscard]] constexpr TReversePointerIterator<const InElementType> rbegin() const { return TReversePointerIterator<const InElementType>(Elements + NumElements); }
	[[nodiscard]] constexpr TReversePointerIterator<InElementType>       rend  ()       { return TReversePointerIterator<InElementType>(Elements); }
	[[nodiscard]] constexpr TReversePointerIterator<const InElementType> rend  () const { return TReversePointerIterator<const InElementType>(Elements); }
	
private:
	template <u32... Indices, typename ArgGeneratorType>
	constexpr explicit TStaticArray(EInPlace, TIntegerSequence<u32, Indices...>, ArgGeneratorType Generator)
		: Elements{Generator(Indices)...}
	{
	}

	template <typename... ArgGeneratorTypes>
	constexpr explicit TStaticArray(EPerElement, ArgGeneratorTypes... Generator)
		: Elements{Generator()...}
	{
	}
};

/** Creates a static array filled with the specified value. */
template <typename InElementType, u32 NumElements>
[[nodiscard]] constexpr TStaticArray<InElementType, NumElements> MakeUniformStaticArray(typename TCallTraits<InElementType>::ParamType InValue)
{
	TStaticArray<InElementType, NumElements> Result;
	for (u32 ElementIndex = 0; ElementIndex < NumElements; ++ElementIndex)
	{
		Result[ElementIndex] = InValue;
	}
	return Result;
}

template <typename ElementType, u32 NumElements>
struct TIsContiguousContainer<TStaticArray<ElementType, NumElements>>
{
	enum { Value = true };
};

/** Hash function. */
template <typename ElementType, u32 NumElements>
[[nodiscard]] u32 GetTypeHash(const TStaticArray<ElementType, NumElements>& Array)
{
	u32 Hash = 0;
	for (const ElementType& Element : Array)
	{
		Hash = HashCombineFast(Hash, GetTypeHash(Element));
	}
	return Hash;
}

} // namespace OloEngine
