// MemoryView.h - Non-owning view of a contiguous memory region
// Ported from UE5.7 Memory/MemoryView.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Assert.h"
#include "OloEngine/Memory/MemoryFwd.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Templates/UnrealTemplate.h"

#include <initializer_list>
#include <type_traits>
#include <limits>
#include <algorithm>

namespace OloEngine
{

    // @brief A non-owning view of a contiguous region of memory.
    //
    // Prefer to use the aliases FMemoryView or FMutableMemoryView over this type.
    //
    // Functions that modify a view clamp sizes and offsets to always return a sub-view of the input.
    template<typename DataType>
    class TMemoryView
    {
        static_assert(std::is_void_v<DataType>, "DataType must be cv-qualified void");

        using ByteType = std::conditional_t<std::is_const_v<DataType>, const u8, u8>;

      public:
        // Construct an empty view.
        constexpr TMemoryView() = default;

        // Construct a view of by copying a view with compatible const/volatile qualifiers.
        template<typename OtherDataType>
            requires(std::is_convertible_v<OtherDataType*, DataType*>)
        constexpr inline TMemoryView(const TMemoryView<OtherDataType>& InView)
            : m_Data(InView.m_Data), m_Size(InView.m_Size)
        {
        }

        // Construct a view of InSize bytes starting at InData.
        constexpr inline TMemoryView(DataType* InData, u64 InSize)
            : m_Data(InData), m_Size(InSize)
        {
        }

        // Construct a view starting at InData and ending at InDataEnd.
        template<typename DataEndType>
            requires(std::is_convertible_v<DataEndType*, DataType*>)
        inline TMemoryView(DataType* InData, DataEndType* InDataEnd)
            : m_Data(InData), m_Size(static_cast<u64>(static_cast<ByteType*>(static_cast<DataType*>(InDataEnd)) - static_cast<ByteType*>(InData)))
        {
        }

        // Returns a pointer to the start of the view.
        [[nodiscard]] constexpr inline DataType* GetData() const
        {
            return m_Data;
        }

        // Returns a pointer to the end of the view.
        [[nodiscard]] inline DataType* GetDataEnd() const
        {
            return GetDataAtOffsetNoCheck(m_Size);
        }

        // Returns the number of bytes in the view.
        [[nodiscard]] constexpr inline u64 GetSize() const
        {
            return m_Size;
        }

        // Returns whether the view has a size of 0 regardless of its data pointer.
        [[nodiscard]] constexpr inline bool IsEmpty() const
        {
            return m_Size == 0;
        }

        // Resets to an empty view.
        constexpr inline void Reset()
        {
            *this = TMemoryView();
        }

        // Returns the left-most part of the view by taking the given number of bytes from the left.
        [[nodiscard]] constexpr inline TMemoryView Left(u64 InSize) const
        {
            TMemoryView View(*this);
            View.LeftInline(InSize);
            return View;
        }

        // Returns the left-most part of the view by chopping the given number of bytes from the right.
        [[nodiscard]] constexpr inline TMemoryView LeftChop(u64 InSize) const
        {
            TMemoryView View(*this);
            View.LeftChopInline(InSize);
            return View;
        }

        // Returns the right-most part of the view by taking the given number of bytes from the right.
        [[nodiscard]] inline TMemoryView Right(u64 InSize) const
        {
            TMemoryView View(*this);
            View.RightInline(InSize);
            return View;
        }

        // Returns the right-most part of the view by chopping the given number of bytes from the left.
        [[nodiscard]] inline TMemoryView RightChop(u64 InSize) const
        {
            TMemoryView View(*this);
            View.RightChopInline(InSize);
            return View;
        }

        // Returns the middle part of the view by taking up to the given number of bytes from the given position.
        [[nodiscard]] inline TMemoryView Mid(u64 InOffset, u64 InSize = std::numeric_limits<u64>::max()) const
        {
            TMemoryView View(*this);
            View.MidInline(InOffset, InSize);
            return View;
        }

        // Modifies the view to be the given number of bytes from the left.
        constexpr inline void LeftInline(u64 InSize)
        {
            m_Size = std::min(m_Size, InSize);
        }

        // Modifies the view by chopping the given number of bytes from the right.
        constexpr inline void LeftChopInline(u64 InSize)
        {
            m_Size -= std::min(m_Size, InSize);
        }

        // Modifies the view to be the given number of bytes from the right.
        inline void RightInline(u64 InSize)
        {
            const u64 OldSize = m_Size;
            const u64 NewSize = std::min(OldSize, InSize);
            m_Data = GetDataAtOffsetNoCheck(OldSize - NewSize);
            m_Size = NewSize;
        }

        // Modifies the view by chopping the given number of bytes from the left.
        inline void RightChopInline(u64 InSize)
        {
            const u64 Offset = std::min(m_Size, InSize);
            m_Data = GetDataAtOffsetNoCheck(Offset);
            m_Size -= Offset;
        }

        // Modifies the view to be the middle part by taking up to the given number of bytes from the given offset.
        inline void MidInline(u64 InOffset, u64 InSize = std::numeric_limits<u64>::max())
        {
            RightChopInline(InOffset);
            LeftInline(InSize);
        }

        // Returns whether this view fully contains the other view.
        template<typename OtherDataType>
        [[nodiscard]] inline bool Contains(const TMemoryView<OtherDataType>& InView) const
        {
            return m_Data <= InView.m_Data && GetDataAtOffsetNoCheck(m_Size) >= InView.GetDataAtOffsetNoCheck(InView.m_Size);
        }

        // Returns whether this view intersects the other view.
        template<typename OtherDataType>
        [[nodiscard]] inline bool Intersects(const TMemoryView<OtherDataType>& InView) const
        {
            return m_Data < InView.GetDataAtOffsetNoCheck(InView.m_Size) && InView.m_Data < GetDataAtOffsetNoCheck(m_Size);
        }

        // Returns whether the bytes of this view are equal or less/greater than the bytes of the other view.
        template<typename OtherDataType>
        [[nodiscard]] inline i32 CompareBytes(const TMemoryView<OtherDataType>& InView) const
        {
            const i32 Compare = m_Data == InView.m_Data ? 0 : FMemory::Memcmp(m_Data, InView.m_Data, std::min(m_Size, InView.m_Size));
            return Compare || m_Size == InView.m_Size ? Compare : m_Size < InView.m_Size ? -1
                                                                                         : 1;
        }

        // Returns whether the bytes of this views are equal to the bytes of the other view.
        template<typename OtherDataType>
        [[nodiscard]] inline bool EqualBytes(const TMemoryView<OtherDataType>& InView) const
        {
            return m_Size == InView.m_Size && (m_Data == InView.m_Data || FMemory::Memcmp(m_Data, InView.m_Data, m_Size) == 0);
        }

        // Returns whether the data pointers and sizes of this view and the other view are equal.
        template<typename OtherDataType>
        [[nodiscard]] constexpr inline bool Equals(const TMemoryView<OtherDataType>& InView) const
        {
            return m_Size == InView.m_Size && (m_Size == 0 || m_Data == InView.m_Data);
        }

        // Returns whether the data pointers and sizes of this view and the other view are equal.
        template<typename OtherDataType>
        [[nodiscard]] constexpr inline bool operator==(const TMemoryView<OtherDataType>& InView) const
        {
            return Equals(InView);
        }

        // Returns whether the data pointers and sizes of this view and the other view are not equal.
        template<typename OtherDataType>
        [[nodiscard]] constexpr inline bool operator!=(const TMemoryView<OtherDataType>& InView) const
        {
            return !Equals(InView);
        }

        // Advances the start of the view by an offset, which is clamped to stay within the view.
        constexpr inline TMemoryView& operator+=(u64 InOffset)
        {
            RightChopInline(InOffset);
            return *this;
        }

        // Copies bytes from the input view into this view, and returns the remainder of this view.
        inline TMemoryView CopyFrom(FMemoryView InView) const
        {
            OLO_CORE_ASSERT(InView.m_Size <= m_Size, "Failed to copy from a view of {} bytes to a view of {} bytes.", InView.m_Size, m_Size);
            if (InView.m_Size)
            {
                FMemory::Memcpy(m_Data, InView.m_Data, InView.m_Size);
            }
            return RightChop(InView.m_Size);
        }

      private:
        // Returns the data pointer advanced by an offset in bytes.
        [[nodiscard]] inline DataType* GetDataAtOffsetNoCheck(u64 InOffset) const
        {
            return reinterpret_cast<ByteType*>(m_Data) + InOffset;
        }

        template<typename OtherDataType>
        friend class TMemoryView;

      private:
        DataType* m_Data = nullptr;
        u64 m_Size = 0;
    };

    // Advances the start of the view by an offset, which is clamped to stay within the view.
    template<typename DataType>
    [[nodiscard]] constexpr inline TMemoryView<DataType> operator+(const TMemoryView<DataType>& View, u64 Offset)
    {
        return TMemoryView<DataType>(View) += Offset;
    }

    // Advances the start of the view by an offset, which is clamped to stay within the view.
    template<typename DataType>
    [[nodiscard]] constexpr inline TMemoryView<DataType> operator+(u64 Offset, const TMemoryView<DataType>& View)
    {
        return TMemoryView<DataType>(View) += Offset;
    }

    // Make a non-owning mutable view of Size bytes starting at Data.
    [[nodiscard]] constexpr inline TMemoryView<void> MakeMemoryView(void* Data, u64 Size)
    {
        return TMemoryView<void>(Data, Size);
    }

    // Make a non-owning const view of Size bytes starting at Data.
    [[nodiscard]] constexpr inline TMemoryView<const void> MakeMemoryView(const void* Data, u64 Size)
    {
        return TMemoryView<const void>(Data, Size);
    }

    // Make a non-owning view starting at Data and ending at DataEnd.
    template<typename DataType, typename DataEndType>
    [[nodiscard]] inline auto MakeMemoryView(DataType* Data, DataEndType* DataEnd)
    {
        using VoidType = std::conditional_t<std::is_const_v<DataType> || std::is_const_v<DataEndType>, const void, void>;
        return TMemoryView<VoidType>(Data, DataEnd);
    }

    // Make a non-owning view of the memory of the initializer list.
    template<typename T>
    [[nodiscard]] constexpr inline TMemoryView<const void> MakeMemoryView(std::initializer_list<T> List)
    {
        return TMemoryView<const void>(List.begin(), List.size() * sizeof(T));
    }

    // Make a non-owning view of the memory of a contiguous container.
    template<typename ContainerType>
        requires(TIsContiguousContainer<std::remove_cvref_t<ContainerType>>::Value)
    [[nodiscard]] constexpr inline auto MakeMemoryView(ContainerType&& Container)
    {
        using ElementType = std::remove_pointer_t<decltype(GetData(std::declval<ContainerType>()))>;
        constexpr bool bIsConst = std::is_const_v<ElementType>;
        using DataType = std::conditional_t<bIsConst, const void, void>;
        return TMemoryView<DataType>(GetData(Container), GetNum(Container) * sizeof(ElementType));
    }

} // namespace OloEngine
