// Deque.h - Double-ended queue container
// Ported from UE5.7 Containers/Deque.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Memory/MemoryOps.h"

#include <initializer_list>

namespace OloEngine
{
    // Forward declarations
    template<typename InElementType, typename InAllocatorType = FDefaultAllocator>
    class TDeque;

    namespace Deque
    {
        namespace Private
        {
            /**
             * Efficient wrap-around function that avoids modulo operator.
             * Assumes Index never exceeds twice the Range value.
             */
            template<typename SizeType>
            OLO_FINLINE SizeType WrapAround(SizeType Index, SizeType Range)
            {
                return (Index < Range) ? Index : Index - Range;
            }

            /**
             * TDeque iterator base class
             */
            template<typename InElementType, typename InSizeType>
            class TIteratorBase
            {
              public:
                using ElementType = InElementType;
                using SizeType = InSizeType;

                TIteratorBase() = default;

                TIteratorBase(ElementType* InData, SizeType InRange, SizeType InOffset)
                    : Data(InData), Range(InRange), Offset(InOffset)
                {
                }

                OLO_FINLINE ElementType& operator*() const
                {
                    return *(Data + WrapAround(Offset, Range));
                }

                OLO_FINLINE ElementType* operator->() const
                {
                    return Data + WrapAround(Offset, Range);
                }

                TIteratorBase& operator++()
                {
                    OLO_CORE_ASSERT(Offset + 1 < Range * 2, "Iterator overflow");
                    ++Offset;
                    return *this;
                }

                TIteratorBase operator++(int)
                {
                    TIteratorBase Temp = *this;
                    ++(*this);
                    return Temp;
                }

                OLO_FINLINE bool operator==(const TIteratorBase& Other) const
                {
                    return Data + Offset == Other.Data + Other.Offset;
                }

                OLO_FINLINE bool operator!=(const TIteratorBase& Other) const
                {
                    return !(*this == Other);
                }

              private:
                ElementType* Data = nullptr;
                SizeType Range = 0;
                SizeType Offset = 0;
            };
        } // namespace Private
    } // namespace Deque

    /**
     * @class TDeque
     * @brief Sequential double-ended queue (deque) container class
     *
     * A dynamically sized sequential queue that supports efficient insertion
     * and removal at both ends. Uses a circular buffer internally.
     *
     * @tparam InElementType The type of elements stored
     * @tparam InAllocatorType The allocator type (default: FDefaultAllocator)
     *
     * Example:
     * @code
     * TDeque<int> Queue;
     * Queue.PushLast(1);
     * Queue.PushLast(2);
     * Queue.PushFirst(0);  // Queue is now: 0, 1, 2
     *
     * int First = Queue.First();  // 0
     * int Last = Queue.Last();    // 2
     *
     * Queue.PopFirst();  // Queue is now: 1, 2
     * Queue.PopLast();   // Queue is now: 1
     * @endcode
     */
    template<typename InElementType, typename InAllocatorType>
    class TDeque
    {
        template<typename AnyElementType, typename AnyAllocatorType>
        friend class TDeque;

      public:
        using AllocatorType = InAllocatorType;
        using SizeType = typename InAllocatorType::SizeType;
        using ElementType = InElementType;

        using ElementAllocatorType = std::conditional_t<
            AllocatorType::NeedsElementType,
            typename AllocatorType::template ForElementType<ElementType>,
            typename AllocatorType::ForAnyElementType>;

        using ConstIteratorType = Deque::Private::TIteratorBase<const ElementType, SizeType>;
        using IteratorType = Deque::Private::TIteratorBase<ElementType, SizeType>;

        // ============================================================================
        // Constructors / Destructor
        // ============================================================================

        TDeque() : m_Capacity(m_Storage.GetInitialCapacity())
        {
        }

        TDeque(TDeque&& Other) noexcept
        {
            MoveUnchecked(MoveTemp(Other));
        }

        TDeque(const TDeque& Other)
            : m_Capacity(m_Storage.GetInitialCapacity())
        {
            CopyUnchecked(Other);
        }

        TDeque(std::initializer_list<ElementType> InList)
            : m_Capacity(m_Storage.GetInitialCapacity())
        {
            CopyUnchecked(InList);
        }

        ~TDeque()
        {
            Empty();
        }

        // ============================================================================
        // Assignment Operators
        // ============================================================================

        TDeque& operator=(TDeque&& Other) noexcept
        {
            if (this != &Other)
            {
                Reset();
                MoveUnchecked(MoveTemp(Other));
            }
            return *this;
        }

        TDeque& operator=(const TDeque& Other)
        {
            if (this != &Other)
            {
                Reset();
                CopyUnchecked(Other);
            }
            return *this;
        }

        TDeque& operator=(std::initializer_list<ElementType> InList)
        {
            Reset();
            CopyUnchecked(InList);
            return *this;
        }

        // ============================================================================
        // Element Access
        // ============================================================================

        const ElementType& operator[](SizeType Index) const
        {
            CheckValidIndex(Index);
            return GetData()[Deque::Private::WrapAround(m_Head + Index, m_Capacity)];
        }

        ElementType& operator[](SizeType Index)
        {
            CheckValidIndex(Index);
            return GetData()[Deque::Private::WrapAround(m_Head + Index, m_Capacity)];
        }

        const ElementType& Last() const
        {
            CheckValidIndex(0);
            return GetData()[Deque::Private::WrapAround(m_Tail + m_Capacity - 1, m_Capacity)];
        }

        ElementType& Last()
        {
            CheckValidIndex(0);
            return GetData()[Deque::Private::WrapAround(m_Tail + m_Capacity - 1, m_Capacity)];
        }

        const ElementType& First() const
        {
            CheckValidIndex(0);
            return GetData()[m_Head];
        }

        ElementType& First()
        {
            CheckValidIndex(0);
            return GetData()[m_Head];
        }

        // ============================================================================
        // Size / Capacity
        // ============================================================================

        OLO_FINLINE bool IsEmpty() const
        {
            return m_Count == 0;
        }
        OLO_FINLINE SizeType Max() const
        {
            return m_Capacity;
        }
        OLO_FINLINE SizeType Num() const
        {
            return m_Count;
        }

        SIZE_T GetAllocatedSize() const
        {
            return m_Storage.GetAllocatedSize(m_Capacity, sizeof(ElementType));
        }

        // ============================================================================
        // Modifiers - Push/Pop
        // ============================================================================

        /**
         * @brief Constructs an element in place at the back of the queue
         * @return Reference to the constructed element
         */
        template<typename... ArgsType>
        ElementType& EmplaceLast(ArgsType&&... Args)
        {
            GrowIfRequired();
            ElementType* Target = GetData() + m_Tail;
            new (Target) ElementType(Forward<ArgsType>(Args)...);
            m_Tail = Deque::Private::WrapAround(m_Tail + 1, m_Capacity);
            ++m_Count;
            return *Target;
        }

        /**
         * @brief Constructs an element in place at the front of the queue
         * @return Reference to the constructed element
         */
        template<typename... ArgsType>
        ElementType& EmplaceFirst(ArgsType&&... Args)
        {
            GrowIfRequired();
            m_Head = Deque::Private::WrapAround(m_Head + m_Capacity - 1, m_Capacity);
            ElementType* Target = GetData() + m_Head;
            new (Target) ElementType(Forward<ArgsType>(Args)...);
            ++m_Count;
            return *Target;
        }

        OLO_FINLINE void PushLast(const ElementType& Element)
        {
            EmplaceLast(Element);
        }
        OLO_FINLINE void PushLast(ElementType&& Element)
        {
            EmplaceLast(MoveTemp(Element));
        }
        OLO_FINLINE void PushFirst(const ElementType& Element)
        {
            EmplaceFirst(Element);
        }
        OLO_FINLINE void PushFirst(ElementType&& Element)
        {
            EmplaceFirst(MoveTemp(Element));
        }

        // Aliases for compatibility with std::deque interface
        OLO_FINLINE void push_back(const ElementType& Element)
        {
            PushLast(Element);
        }
        OLO_FINLINE void push_back(ElementType&& Element)
        {
            PushLast(MoveTemp(Element));
        }
        OLO_FINLINE void push_front(const ElementType& Element)
        {
            PushFirst(Element);
        }
        OLO_FINLINE void push_front(ElementType&& Element)
        {
            PushFirst(MoveTemp(Element));
        }

        /**
         * @brief Removes the element at the back of the queue
         * @note Requires a non-empty queue
         */
        void PopLast()
        {
            CheckValidIndex(0);
            const SizeType NextTail = Deque::Private::WrapAround(m_Tail + m_Capacity - 1, m_Capacity);
            DestructItem(GetData() + NextTail);
            m_Tail = NextTail;
            --m_Count;
        }

        /**
         * @brief Removes the element at the front of the queue
         * @note Requires a non-empty queue
         */
        void PopFirst()
        {
            CheckValidIndex(0);
            DestructItem(GetData() + m_Head);
            m_Head = Deque::Private::WrapAround(m_Head + 1, m_Capacity);
            --m_Count;
        }

        // Aliases for compatibility with std::deque interface
        OLO_FINLINE void pop_back()
        {
            PopLast();
        }
        OLO_FINLINE void pop_front()
        {
            PopFirst();
        }

        // Aliases for front/back access
        OLO_FINLINE ElementType& front()
        {
            return First();
        }
        OLO_FINLINE const ElementType& front() const
        {
            return First();
        }
        OLO_FINLINE ElementType& back()
        {
            return Last();
        }
        OLO_FINLINE const ElementType& back() const
        {
            return Last();
        }
        OLO_FINLINE bool empty() const
        {
            return IsEmpty();
        }

        /**
         * @brief Try to pop and return the last element
         * @param OutValue Receives the popped value if successful
         * @return true if an element was popped, false if empty
         */
        bool TryPopLast(ElementType& OutValue)
        {
            if (IsEmpty())
            {
                return false;
            }
            const SizeType NextTail = Deque::Private::WrapAround(m_Tail + m_Capacity - 1, m_Capacity);
            OutValue = MoveTemp(GetData()[NextTail]);
            DestructItem(GetData() + NextTail);
            m_Tail = NextTail;
            --m_Count;
            return true;
        }

        /**
         * @brief Try to pop and return the first element
         * @param OutValue Receives the popped value if successful
         * @return true if an element was popped, false if empty
         */
        bool TryPopFirst(ElementType& OutValue)
        {
            if (IsEmpty())
            {
                return false;
            }
            OutValue = MoveTemp(GetData()[m_Head]);
            DestructItem(GetData() + m_Head);
            m_Head = Deque::Private::WrapAround(m_Head + 1, m_Capacity);
            --m_Count;
            return true;
        }

        // ============================================================================
        // Modifiers - Reset/Empty/Reserve
        // ============================================================================

        /**
         * @brief Destroys all elements but keeps the storage
         */
        void Reset()
        {
            if (m_Count)
            {
                if (m_Head < m_Tail)
                {
                    DestructItems(GetData() + m_Head, m_Count);
                }
                else
                {
                    DestructItems(GetData(), m_Tail);
                    DestructItems(GetData() + m_Head, m_Capacity - m_Head);
                }
            }
            m_Head = m_Tail = m_Count = 0;
        }

        /**
         * @brief Destroys all elements and releases storage
         */
        void Empty()
        {
            Reset();
            if (m_Capacity)
            {
                m_Storage.ResizeAllocation(0, 0, sizeof(ElementType));
                m_Capacity = m_Storage.GetInitialCapacity();
            }
        }

        /**
         * @brief Reserves storage for at least the specified number of elements
         */
        void Reserve(SizeType InCount)
        {
            if (m_Capacity < InCount)
            {
                Grow(m_Storage.CalculateSlackReserve(InCount, sizeof(ElementType)));
            }
        }

        // ============================================================================
        // Iterators
        // ============================================================================

        OLO_FINLINE ConstIteratorType begin() const
        {
            return ConstIteratorType(GetData(), Max(), m_Head);
        }
        OLO_FINLINE IteratorType begin()
        {
            return IteratorType(GetData(), Max(), m_Head);
        }
        OLO_FINLINE ConstIteratorType end() const
        {
            return ConstIteratorType(GetData(), Max(), m_Head + m_Count);
        }
        OLO_FINLINE IteratorType end()
        {
            return IteratorType(GetData(), Max(), m_Head + m_Count);
        }

        // ============================================================================
        // Comparison Operators
        // ============================================================================

        bool operator==(const TDeque& Other) const
        {
            if (Num() != Other.Num())
            {
                return false;
            }
            auto LeftIt = begin();
            auto RightIt = Other.begin();
            auto EndIt = end();
            while (LeftIt != EndIt)
            {
                if (*LeftIt++ != *RightIt++)
                {
                    return false;
                }
            }
            return true;
        }

        bool operator!=(const TDeque& Other) const
        {
            return !(*this == Other);
        }

      private:
        // ============================================================================
        // Private Helpers
        // ============================================================================

        const ElementType* GetData() const
        {
            return reinterpret_cast<const ElementType*>(m_Storage.GetAllocation());
        }

        ElementType* GetData()
        {
            return reinterpret_cast<ElementType*>(m_Storage.GetAllocation());
        }

        void Grow(SizeType InCapacity)
        {
            OLO_CORE_ASSERT(m_Capacity < InCapacity, "Grow called with smaller capacity");
            if (m_Count)
            {
                Linearize();
            }
            m_Storage.ResizeAllocation(m_Count, InCapacity, sizeof(ElementType));
            m_Capacity = InCapacity;
            m_Head = 0;
            m_Tail = m_Count;
        }

        void GrowIfRequired()
        {
            if (m_Count == m_Capacity)
            {
                Grow(m_Storage.CalculateSlackGrow(m_Count + 1, m_Capacity, sizeof(ElementType)));
            }
        }

        void CopyUnchecked(const TDeque& Other)
        {
            OLO_CORE_ASSERT(m_Count == 0, "CopyUnchecked called on non-empty deque");
            if (Other.m_Count)
            {
                Reserve(Other.m_Count);
                CopyElements(Other);
            }
            else
            {
                m_Capacity = Other.m_Storage.GetInitialCapacity();
            }
        }

        void CopyUnchecked(std::initializer_list<ElementType> InList)
        {
            const SizeType InCount = static_cast<SizeType>(InList.size());
            OLO_CORE_ASSERT(m_Count == 0, "CopyUnchecked called on non-empty deque");
            if (InCount)
            {
                Reserve(InCount);
                ConstructItems<ElementType>(GetData(), &*InList.begin(), InCount);
                m_Tail = m_Count = InCount;
            }
            else
            {
                m_Capacity = m_Storage.GetInitialCapacity();
            }
        }

        void MoveUnchecked(TDeque&& Other)
        {
            OLO_CORE_ASSERT(m_Count == 0, "MoveUnchecked called on non-empty deque");
            if (Other.m_Count)
            {
                m_Storage.MoveToEmpty(Other.m_Storage);
                m_Capacity = Other.m_Capacity;
                m_Count = Other.m_Count;
                m_Head = Other.m_Head;
                m_Tail = Other.m_Tail;
                Other.m_Capacity = Other.m_Storage.GetInitialCapacity();
                Other.m_Count = Other.m_Head = Other.m_Tail = 0;
            }
            else
            {
                m_Capacity = Other.m_Storage.GetInitialCapacity();
            }
        }

        void CopyElements(const TDeque& Other)
        {
            if (Other.m_Head < Other.m_Tail)
            {
                ConstructItems<ElementType>(GetData(), Other.GetData() + Other.m_Head, Other.m_Count);
            }
            else
            {
                const SizeType HeadToEndOffset = Other.m_Capacity - Other.m_Head;
                ConstructItems<ElementType>(GetData(), Other.GetData() + Other.m_Head, HeadToEndOffset);
                ConstructItems<ElementType>(GetData() + HeadToEndOffset, Other.GetData(), Other.m_Tail);
            }
            m_Count = Other.m_Count;
            m_Head = 0;
            m_Tail = Deque::Private::WrapAround(m_Count, m_Capacity);
        }

        void Linearize()
        {
            if (m_Head < m_Tail)
            {
                ShiftElementsLeft(m_Count);
            }
            else
            {
                ElementAllocatorType TempStorage;
                TempStorage.ResizeAllocation(0, m_Tail, sizeof(ElementType));
                RelocateConstructItems<ElementType>(reinterpret_cast<ElementType*>(TempStorage.GetAllocation()), GetData(), m_Tail);
                const SizeType HeadToEndOffset = m_Capacity - m_Head;
                ShiftElementsLeft(HeadToEndOffset);
                RelocateConstructItems<ElementType>(GetData() + HeadToEndOffset,
                                                    reinterpret_cast<ElementType*>(TempStorage.GetAllocation()), m_Tail);
            }
        }

        void ShiftElementsLeft(SizeType InCount)
        {
            if (m_Head == 0)
            {
                return;
            }
            SizeType Offset = 0;
            while (Offset < InCount)
            {
                const SizeType Step = (m_Head < InCount - Offset) ? m_Head : (InCount - Offset);
                RelocateConstructItems<ElementType>(GetData() + Offset, GetData() + m_Head + Offset, Step);
                Offset += Step;
            }
        }

        void CheckValidIndex(SizeType Index) const
        {
            OLO_CORE_ASSERT(m_Count >= 0 && m_Capacity >= m_Count, "Invalid deque state");
            OLO_CORE_ASSERT(Index >= 0 && Index < m_Count, "Index out of bounds");
        }

        static void DestructItem(ElementType* Item)
        {
            Item->~ElementType();
        }

        static void DestructItems(ElementType* Items, SizeType Count)
        {
            if constexpr (!TIsTriviallyDestructible<ElementType>::Value)
            {
                for (SizeType i = 0; i < Count; ++i)
                {
                    Items[i].~ElementType();
                }
            }
        }

        // ============================================================================
        // Member Variables
        // ============================================================================

        ElementAllocatorType m_Storage;
        SizeType m_Capacity = 0;
        SizeType m_Count = 0;
        SizeType m_Head = 0;
        SizeType m_Tail = 0;
    };

} // namespace OloEngine
