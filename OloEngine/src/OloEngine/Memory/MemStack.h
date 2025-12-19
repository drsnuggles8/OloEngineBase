#pragma once

// @file MemStack.h
// @brief Simple linear-allocation memory stack
//
// Provides a fast linear allocator for temporary allocations.
// Items are allocated via PushBytes() or specialized operator new()s.
// Items are freed en masse by using FMemMark to Pop() them.
//
// Ported from Unreal Engine's MemStack.h

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/AlignmentTemplates.h"
#include "OloEngine/Memory/PageAllocator.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace OloEngine
{
    // ========================================================================
    // Enums for specifying memory allocation type
    // ========================================================================

    enum EMemZeroed
    {
        MEM_Zeroed = 1
    };

    enum EMemOned
    {
        MEM_Oned = 1
    };

    // ========================================================================
    // Forward Declarations
    // ========================================================================

    class FMemStackBase;
    class FMemStack;
    class FMemMark;

    // ========================================================================
    // FMemStackBase - Simple linear-allocation memory stack
    // ========================================================================

    // @class FMemStackBase
    // @brief Simple linear-allocation memory stack
    //
    // Items are allocated via PushBytes() or the specialized operator new()s.
    // Items are freed en masse by using FMemMark to Pop() them.
    class FMemStackBase
    {
      public:
        enum class EPageSize : u8
        {
            // Small pages are allocated unless the allocation requires a larger page.
            Small,

            // Large pages are always allocated.
            Large
        };

        explicit FMemStackBase(EPageSize InPageSize = EPageSize::Small);

        FMemStackBase(const FMemStackBase&) = delete;

        FMemStackBase(FMemStackBase&& Other) noexcept
        {
            *this = std::move(Other);
        }

        FMemStackBase& operator=(FMemStackBase&& Other) noexcept
        {
            FreeChunks(nullptr);
            m_Top = Other.m_Top;
            m_End = Other.m_End;
            m_TopChunk = Other.m_TopChunk;
            m_TopMark = Other.m_TopMark;
            m_NumMarks = Other.m_NumMarks;
            m_bShouldEnforceAllocMarks = Other.m_bShouldEnforceAllocMarks;
            m_PageSize = Other.m_PageSize;
            Other.m_Top = nullptr;
            Other.m_End = nullptr;
            Other.m_TopChunk = nullptr;
            Other.m_NumMarks = 0;
            Other.m_bShouldEnforceAllocMarks = false;
            return *this;
        }

        ~FMemStackBase()
        {
            OLO_CORE_ASSERT(m_NumMarks == 0, "FMemStackBase destroyed with outstanding marks!");
            FreeChunks(nullptr);
        }

        // @brief Push bytes onto the stack with automatic alignment
        // @param AllocSize Number of bytes to allocate
        // @param Alignment Alignment requirement
        // @return Pointer to allocated memory
        OLO_FINLINE u8* PushBytes(sizet AllocSize, sizet Alignment)
        {
            return static_cast<u8*>(Alloc(AllocSize, std::max(AllocSize >= 16 ? sizet{ 16 } : sizet{ 8 }, Alignment)));
        }

        // @brief Check if an allocation can fit in the current page
        // @param AllocSize Number of bytes to allocate
        // @param Alignment Alignment requirement
        // @return True if allocation fits in current page
        bool CanFitInPage(sizet AllocSize, sizet Alignment) const
        {
            const u8* Result = Align(m_Top, Alignment);
            const u8* NewTop = Result + AllocSize;
            return NewTop <= m_End;
        }

        // @brief Allocate memory from the stack
        // @param AllocSize Number of bytes to allocate
        // @param Alignment Alignment requirement
        // @return Pointer to allocated memory
        void* Alloc(sizet AllocSize, sizet Alignment)
        {
            // Debug checks
            OLO_CORE_ASSERT((Alignment & (Alignment - 1)) == 0, "Alignment must be power of 2");
            OLO_CORE_ASSERT(m_Top <= m_End, "Stack corruption detected");
            OLO_CORE_ASSERT(!m_bShouldEnforceAllocMarks || m_NumMarks > 0, "Allocation without mark!");

            // Try to get memory from the current chunk
            u8* Result = Align(m_Top, Alignment);
            u8* NewTop = Result + AllocSize;

            // Make sure we didn't overflow
            if (NewTop <= m_End)
            {
                m_Top = NewTop;
            }
            else
            {
                // We'd pass the end of the current chunk, so allocate a new one
                AllocateNewChunk(static_cast<i32>(AllocSize + Alignment));
                Result = Align(m_Top, Alignment);
                NewTop = Result + AllocSize;
                m_Top = NewTop;
            }
            return Result;
        }

        // @brief Get the current top of the stack
        // @return Pointer to top of stack
        OLO_FINLINE u8* GetTop() const
        {
            return m_Top;
        }

        // @brief Check if this stack is empty
        // @return True if stack has no allocations
        OLO_FINLINE bool IsEmpty() const
        {
            return m_TopChunk == nullptr;
        }

        // @brief Flush all allocations (requires no outstanding marks)
        void Flush()
        {
            OLO_CORE_ASSERT(m_NumMarks == 0, "Cannot flush with outstanding marks!");
            FreeChunks(nullptr);
        }

        // @brief Get the number of outstanding marks
        // @return Number of marks
        OLO_FINLINE i32 GetNumMarks() const
        {
            return m_NumMarks;
        }

        // @brief Get the number of bytes allocated that are currently in use
        // @return Number of bytes in use
        i32 GetByteCount() const;

        // @brief Check if a pointer was allocated using this allocator
        // @param Pointer Pointer to check
        // @return True if pointer is from this allocator
        bool ContainsPointer(const void* Pointer) const;

        // Types
        struct FTaggedMemory
        {
            FTaggedMemory* Next;
            i32 DataSize;

            u8* Data() const
            {
                return reinterpret_cast<u8*>(const_cast<FTaggedMemory*>(this)) + sizeof(FTaggedMemory);
            }
        };

      private:
        friend class FMemMark;

        // Note: operator new overloads (defined in global namespace after the class)
        // use the public PushBytes() method, so no friend declarations needed.

        // Allocate a new chunk of memory of at least MinSize size,
        // updates the memory stack's Chunks table and ActiveChunks counter.
        void AllocateNewChunk(i32 MinSize);

        // Frees the chunks above the specified chunk on the stack.
        void FreeChunks(FTaggedMemory* NewTopChunk);

        // Variables
        u8* m_Top = nullptr;                 // Top of current chunk (Top<=End)
        u8* m_End = nullptr;                 // End of current chunk
        FTaggedMemory* m_TopChunk = nullptr; // Only chunks 0..ActiveChunks-1 are valid

        // The top mark on the stack.
        FMemMark* m_TopMark = nullptr;

        // The number of marks on this stack.
        i32 m_NumMarks = 0;

        // The page size to use when allocating.
        EPageSize m_PageSize = EPageSize::Small;

      protected:
        bool m_bShouldEnforceAllocMarks = false;
    };

    // ========================================================================
    // FMemStack - Thread-local memory stack singleton
    // ========================================================================

    // @class FMemStack
    // @brief Thread-local memory stack singleton
    //
    // Provides a thread-local memory stack for temporary allocations.
    // Use FMemStack::Get() to access the thread's stack.
    class FMemStack : public FMemStackBase
    {
      public:
        FMemStack()
        {
            m_bShouldEnforceAllocMarks = true;
        }

        // @brief Get the thread-local memory stack instance
        // @return Reference to this thread's memory stack
        static FMemStack& Get()
        {
            thread_local FMemStack Instance;
            return Instance;
        }
    };

    // ========================================================================
    // FMemMark - RAII marker for scoped allocations
    // ========================================================================

    // @class FMemMark
    // @brief Marks a top-of-stack position in the memory stack
    //
    // When the marker is constructed or initialized with a particular memory
    // stack, it saves the stack's current position. When marker is popped, it
    // pops all items that were added to the stack subsequent to initialization.
    class FMemMark
    {
      public:
        // @brief Construct a mark at the current stack position
        // @param InMem The memory stack to mark
        explicit FMemMark(FMemStackBase& InMem)
            : m_Mem(InMem), m_Top(InMem.m_Top), m_SavedChunk(InMem.m_TopChunk), m_bPopped(false), m_NextTopmostMark(InMem.m_TopMark)
        {
            m_Mem.m_TopMark = this;

            // Track the number of outstanding marks on the stack
            m_Mem.m_NumMarks++;
        }

        // Destructor - automatically pops if not already popped
        ~FMemMark()
        {
            Pop();
        }

        // Non-copyable, non-movable
        FMemMark(const FMemMark&) = delete;
        FMemMark& operator=(const FMemMark&) = delete;
        FMemMark(FMemMark&&) = delete;
        FMemMark& operator=(FMemMark&&) = delete;

        // Free the memory allocated after the mark was created.
        void Pop()
        {
            if (!m_bPopped)
            {
                OLO_CORE_ASSERT(m_Mem.m_TopMark == this, "Marks must be popped in LIFO order!");
                m_bPopped = true;

                // Track the number of outstanding marks on the stack
                --m_Mem.m_NumMarks;

                // Unlock any new chunks that were allocated
                if (m_SavedChunk != m_Mem.m_TopChunk)
                {
                    m_Mem.FreeChunks(m_SavedChunk);
                }

                // Restore the memory stack's state
                m_Mem.m_Top = m_Top;
                m_Mem.m_TopMark = m_NextTopmostMark;

                // Ensure that the mark is only popped once by clearing the top pointer
                m_Top = nullptr;
            }
        }

      private:
        FMemStackBase& m_Mem;
        u8* m_Top;
        FMemStackBase::FTaggedMemory* m_SavedChunk;
        bool m_bPopped;
        FMemMark* m_NextTopmostMark;
    };

    // ========================================================================
    // FMemStack templates
    // ========================================================================

    // @brief Allocate typed memory from a memory stack
    // @tparam T Type to allocate
    // @param Mem Memory stack to allocate from
    // @param Count Number of elements to allocate
    // @param Alignment Alignment requirement
    // @return Pointer to uninitialized memory
    template<class T>
    T* New(FMemStackBase& Mem, i32 Count = 1, i32 Alignment = OLO_DEFAULT_ALIGNMENT)
    {
        return reinterpret_cast<T*>(Mem.PushBytes(Count * sizeof(T), Alignment));
    }

    // @brief Allocate zero-initialized typed memory from a memory stack
    // @tparam T Type to allocate
    // @param Mem Memory stack to allocate from
    // @param Count Number of elements to allocate
    // @param Alignment Alignment requirement
    // @return Pointer to zero-initialized memory
    template<class T>
    T* NewZeroed(FMemStackBase& Mem, i32 Count = 1, i32 Alignment = OLO_DEFAULT_ALIGNMENT)
    {
        u8* Result = Mem.PushBytes(Count * sizeof(T), Alignment);
        FMemory::Memzero(Result, Count * sizeof(T));
        return reinterpret_cast<T*>(Result);
    }

    // @brief Allocate 0xFF-initialized typed memory from a memory stack
    // @tparam T Type to allocate
    // @param Mem Memory stack to allocate from
    // @param Count Number of elements to allocate
    // @param Alignment Alignment requirement
    // @return Pointer to 0xFF-initialized memory
    template<class T>
    T* NewOned(FMemStackBase& Mem, i32 Count = 1, i32 Alignment = OLO_DEFAULT_ALIGNMENT)
    {
        u8* Result = Mem.PushBytes(Count * sizeof(T), Alignment);
        FMemory::Memset(Result, 0xff, Count * sizeof(T));
        return reinterpret_cast<T*>(Result);
    }

    // ========================================================================
    // TMemStackAllocator - Standard C++ allocator backed by FMemStack
    // ========================================================================

    // @class TMemStackAllocator
    // @brief A C++ standard-compatible allocator that allocates from a memory stack
    //
    // This allocator can be used with standard containers like std::vector when you
    // want temporary allocations that will be freed in bulk via FMemMark.
    //
    // Note: This allocator does NOT support individual deallocation (deallocate is a no-op).
    // Memory is only freed when the FMemMark is popped or the FMemStack is flushed.
    //
    // Usage:
    // @code
    //     FMemMark Mark(FMemStack::Get());
    //     std::vector<int, TMemStackAllocator<int>> tempVec(TMemStackAllocator<int>{});
    //     tempVec.reserve(100);
    //     // ... use tempVec ...
    //     // Memory automatically freed when Mark goes out of scope
    // @endcode
    //
    // @tparam T The element type to allocate
    // @tparam Alignment The alignment requirement (defaults to alignof(T))
    template<typename T, sizet Alignment = alignof(T)>
    class TMemStackAllocator
    {
      public:
        using value_type = T;
        using size_type = sizet;
        using difference_type = std::ptrdiff_t;
        using propagate_on_container_move_assignment = std::true_type;
        using is_always_equal = std::false_type;

        // @brief Construct an allocator using the thread-local FMemStack
        TMemStackAllocator() noexcept
            : m_Mem(&FMemStack::Get())
        {
        }

        // @brief Construct an allocator using a specific memory stack
        // @param Mem The memory stack to allocate from
        explicit TMemStackAllocator(FMemStackBase& Mem) noexcept
            : m_Mem(&Mem)
        {
        }

        // @brief Copy constructor (for rebinding)
        template<typename U, sizet OtherAlignment>
        TMemStackAllocator(const TMemStackAllocator<U, OtherAlignment>& Other) noexcept
            : m_Mem(Other.m_Mem)
        {
        }

        // @brief Allocate memory for n elements
        // @param n Number of elements to allocate
        // @return Pointer to allocated memory
        [[nodiscard]] T* allocate(size_type n)
        {
            const sizet SizeInBytes = n * sizeof(T);
            OLO_CORE_ASSERT(SizeInBytes <= static_cast<sizet>(std::numeric_limits<i32>::max()),
                            "TMemStackAllocator: Allocation too large!");

            return reinterpret_cast<T*>(m_Mem->PushBytes(
                SizeInBytes,
                std::max(Alignment, alignof(T))));
        }

        // @brief Deallocate memory (no-op for mem stack allocator)
        //
        // Memory is only freed when the FMemMark is popped or stack is flushed.
        // Individual deallocation is not supported.
        void deallocate([[maybe_unused]] T* p, [[maybe_unused]] size_type n) noexcept
        {
            // No-op: Memory stack doesn't support individual deallocation
            // Memory is freed in bulk when FMemMark is popped
        }

        // @brief Get the underlying memory stack
        FMemStackBase* GetMemStack() const noexcept
        {
            return m_Mem;
        }

        // @brief Equality comparison
        template<typename U, sizet OtherAlignment>
        bool operator==(const TMemStackAllocator<U, OtherAlignment>& Other) const noexcept
        {
            return m_Mem == Other.m_Mem;
        }

        // @brief Inequality comparison
        template<typename U, sizet OtherAlignment>
        bool operator!=(const TMemStackAllocator<U, OtherAlignment>& Other) const noexcept
        {
            return m_Mem != Other.m_Mem;
        }

      private:
        template<typename U, sizet OtherAlignment>
        friend class TMemStackAllocator;

        FMemStackBase* m_Mem;
    };

    // @brief Type alias for a vector using the memory stack allocator
    // @tparam T Element type
    template<typename T>
    using TMemStackVector = std::vector<T, TMemStackAllocator<T>>;

    // ========================================================================
    // FMemStackAllocator - UE-style container allocator policy
    // ========================================================================

    // @class FMemStackAllocator
    // @brief UE-style allocator policy for use with TArray and other OloEngine containers
    //
    // This allocator uses a memory stack for allocation but follows the UE
    // ForElementType pattern for compatibility with TArray.
    //
    // Unlike TMemStackAllocator (which is std-compatible), this allocator:
    // - Uses the ForElementType pattern required by TArray
    // - Does not support individual element deallocation
    // - Allocations are freed in bulk when the FMemMark is popped
    class FMemStackAllocator
    {
      public:
        using SizeType = i32;

        enum
        {
            NeedsElementType = true
        };
        enum
        {
            RequireRangeCheck = true
        };
        enum
        {
            ShrinkByDefault = false
        }; // Stack allocator cannot shrink

        template<typename ElementType>
        class ForElementType
        {
          public:
            // Default constructor - uses thread-local memory stack
            ForElementType()
                : m_Data(nullptr), m_Mem(&FMemStack::Get())
            {
            }

            // Construct with a specific memory stack
            explicit ForElementType(FMemStackBase& InMem)
                : m_Data(nullptr), m_Mem(&InMem)
            {
            }

            // Moves the state of another allocator into this one.
            // @param Other - The allocator to move from
            OLO_FINLINE void MoveToEmpty(ForElementType& Other)
            {
                OLO_CORE_ASSERT(this != &Other, "Cannot move to self");

                // For stack allocator, we just take the pointer - we don't free the old one
                // since it's on a memory stack that will be freed in bulk
                m_Data = Other.m_Data;
                m_Mem = Other.m_Mem;
                Other.m_Data = nullptr;
            }

            // Destructor - does nothing (memory is freed by FMemMark)
            ~ForElementType() = default;

            // FContainerAllocatorInterface
            OLO_FINLINE ElementType* GetAllocation() const
            {
                return m_Data;
            }

            // Resize the allocation
            //
            // Note: For stack allocator, we cannot shrink - we can only grow.
            // Growing creates a new allocation and the old one is "leaked"
            // (will be freed when FMemMark is popped).
            void ResizeAllocation(SizeType CurrentNum, SizeType NewMax, sizet NumBytesPerElement)
            {
                OLO_CORE_ASSERT(NewMax >= 0, "NewMax must be non-negative");

                if (NewMax == 0)
                {
                    // Don't actually free - just mark as empty
                    // The old allocation will be freed when FMemMark is popped
                    m_Data = nullptr;
                    return;
                }

                // Allocate new memory from the stack
                ElementType* NewData = reinterpret_cast<ElementType*>(
                    m_Mem->PushBytes(static_cast<sizet>(NewMax) * NumBytesPerElement, alignof(ElementType)));

                // If we had existing data, copy it over
                if (m_Data && CurrentNum > 0)
                {
                    const SizeType NumToCopy = (CurrentNum < NewMax) ? CurrentNum : NewMax;
                    FMemory::Memcpy(NewData, m_Data, static_cast<sizet>(NumToCopy) * NumBytesPerElement);
                }

                // Old data is "leaked" - will be freed when FMemMark is popped
                m_Data = NewData;
            }

            OLO_FINLINE SizeType CalculateSlackReserve(SizeType NewMax, [[maybe_unused]] sizet NumBytesPerElement) const
            {
                // No slack for stack allocator - allocate exactly what's needed
                return NewMax;
            }

            OLO_FINLINE SizeType CalculateSlackShrink(SizeType NewMax, SizeType CurrentMax, [[maybe_unused]] sizet NumBytesPerElement) const
            {
                // Stack allocator cannot shrink - return current max
                return (NewMax == 0) ? 0 : CurrentMax;
            }

            OLO_FINLINE SizeType CalculateSlackGrow(SizeType NewMax, [[maybe_unused]] SizeType CurrentMax, [[maybe_unused]] sizet NumBytesPerElement) const
            {
                // No slack for stack allocator - allocate exactly what's needed
                return NewMax;
            }

            sizet GetAllocatedSize(SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                return m_Data ? (static_cast<sizet>(CurrentMax) * NumBytesPerElement) : 0;
            }

            bool HasAllocation() const
            {
                return m_Data != nullptr;
            }

            constexpr SizeType GetInitialCapacity() const
            {
                return 0;
            }

            // Get the underlying memory stack
            FMemStackBase* GetMemStack() const
            {
                return m_Mem;
            }

            // Set the memory stack to use
            void SetMemStack(FMemStackBase& InMem)
            {
                m_Mem = &InMem;
            }

          private:
            ForElementType(const ForElementType&) = delete;
            ForElementType& operator=(const ForElementType&) = delete;

            // Pointer to the allocated data
            ElementType* m_Data;

            // The memory stack to allocate from
            FMemStackBase* m_Mem;
        };

        using ForAnyElementType = void;
    };

} // namespace OloEngine

// ============================================================================
// FMemStack operator new overloads (must be in global namespace)
// ============================================================================

// Operator new for typesafe memory stack allocation
inline void* operator new(size_t Size, OloEngine::FMemStackBase& Mem, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    return Mem.PushBytes(SizeInBytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

inline void* operator new(size_t Size, std::align_val_t Align, OloEngine::FMemStackBase& Mem, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    return Mem.PushBytes(SizeInBytes, static_cast<size_t>(Align));
}

inline void* operator new(size_t Size, OloEngine::FMemStackBase& Mem, OloEngine::EMemZeroed /*Tag*/, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    uint8_t* Result = Mem.PushBytes(SizeInBytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    OloEngine::FMemory::Memzero(Result, SizeInBytes);
    return Result;
}

inline void* operator new(size_t Size, std::align_val_t Align, OloEngine::FMemStackBase& Mem, OloEngine::EMemZeroed /*Tag*/, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    uint8_t* Result = Mem.PushBytes(SizeInBytes, static_cast<size_t>(Align));
    OloEngine::FMemory::Memzero(Result, SizeInBytes);
    return Result;
}

inline void* operator new(size_t Size, OloEngine::FMemStackBase& Mem, OloEngine::EMemOned /*Tag*/, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    uint8_t* Result = Mem.PushBytes(SizeInBytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    OloEngine::FMemory::Memset(Result, 0xff, SizeInBytes);
    return Result;
}

inline void* operator new(size_t Size, std::align_val_t Align, OloEngine::FMemStackBase& Mem, OloEngine::EMemOned /*Tag*/, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    uint8_t* Result = Mem.PushBytes(SizeInBytes, static_cast<size_t>(Align));
    OloEngine::FMemory::Memset(Result, 0xff, SizeInBytes);
    return Result;
}

inline void* operator new[](size_t Size, OloEngine::FMemStackBase& Mem, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    return Mem.PushBytes(SizeInBytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

inline void* operator new[](size_t Size, std::align_val_t Align, OloEngine::FMemStackBase& Mem, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    return Mem.PushBytes(SizeInBytes, static_cast<size_t>(Align));
}

inline void* operator new[](size_t Size, OloEngine::FMemStackBase& Mem, OloEngine::EMemZeroed /*Tag*/, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    uint8_t* Result = Mem.PushBytes(SizeInBytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    OloEngine::FMemory::Memzero(Result, SizeInBytes);
    return Result;
}

inline void* operator new[](size_t Size, std::align_val_t Align, OloEngine::FMemStackBase& Mem, OloEngine::EMemZeroed /*Tag*/, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    uint8_t* Result = Mem.PushBytes(SizeInBytes, static_cast<size_t>(Align));
    OloEngine::FMemory::Memzero(Result, SizeInBytes);
    return Result;
}

inline void* operator new[](size_t Size, OloEngine::FMemStackBase& Mem, OloEngine::EMemOned /*Tag*/, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    uint8_t* Result = Mem.PushBytes(SizeInBytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    OloEngine::FMemory::Memset(Result, 0xff, SizeInBytes);
    return Result;
}

inline void* operator new[](size_t Size, std::align_val_t Align, OloEngine::FMemStackBase& Mem, OloEngine::EMemOned /*Tag*/, int32_t Count = 1)
{
    const size_t SizeInBytes = Size * Count;
    OLO_CORE_ASSERT(SizeInBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()), "Allocation too large!");
    uint8_t* Result = Mem.PushBytes(SizeInBytes, static_cast<size_t>(Align));
    OloEngine::FMemory::Memset(Result, 0xff, SizeInBytes);
    return Result;
}
