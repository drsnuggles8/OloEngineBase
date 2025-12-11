#pragma once

/**
 * @file ContainerAllocationPolicies.h
 * @brief Container allocation policies for OloEngine containers
 * 
 * Provides allocator policies used by TArray and other containers:
 * - TAllocatorTraits: Trait system for allocator capabilities
 * - TAlignedHeapAllocator: Heap allocator with custom alignment
 * - TSizedHeapAllocator: Heap allocator with configurable index size
 * - DefaultCalculateSlack*: Functions for computing slack/growth
 * 
 * Ported from Unreal Engine's Containers/ContainerAllocationPolicies.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Memory/MemoryOps.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/AlignmentTemplates.h"
#include "OloEngine/Memory/ArraySlackTracking.h"
#include <type_traits>
#include <limits>

namespace OloEngine
{
    // ============================================================================
    // Configuration Macros
    // ============================================================================

    // This option disables array slack for initial allocations, e.g where TArray::SetNum
    // is called. This tends to save a lot of memory with almost no measured performance cost.
    #ifndef CONTAINER_INITIAL_ALLOC_ZERO_SLACK
    #define CONTAINER_INITIAL_ALLOC_ZERO_SLACK 1
    #endif

    // Memory saving mode - if enabled, reduces slack growth
    #ifndef AGGRESSIVE_MEMORY_SAVING
    #define AGGRESSIVE_MEMORY_SAVING 0
    #endif

    // Slack growth factor (numerator / denominator)
    #ifndef OLO_CONTAINER_SLACK_GROWTH_FACTOR_NUMERATOR
        #if AGGRESSIVE_MEMORY_SAVING
            #define OLO_CONTAINER_SLACK_GROWTH_FACTOR_NUMERATOR 1
        #else
            #define OLO_CONTAINER_SLACK_GROWTH_FACTOR_NUMERATOR 3
        #endif
    #endif

    #ifndef OLO_CONTAINER_SLACK_GROWTH_FACTOR_DENOMINATOR
        #if AGGRESSIVE_MEMORY_SAVING
            #define OLO_CONTAINER_SLACK_GROWTH_FACTOR_DENOMINATOR 4
        #else
            #define OLO_CONTAINER_SLACK_GROWTH_FACTOR_DENOMINATOR 8
        #endif
    #endif

    static_assert(OLO_CONTAINER_SLACK_GROWTH_FACTOR_DENOMINATOR > 0, 
                  "OLO_CONTAINER_SLACK_GROWTH_FACTOR_DENOMINATOR must be greater than 0");
    static_assert(OLO_CONTAINER_SLACK_GROWTH_FACTOR_DENOMINATOR > OLO_CONTAINER_SLACK_GROWTH_FACTOR_NUMERATOR, 
                  "OLO_CONTAINER_SLACK_GROWTH_FACTOR_DENOMINATOR must be greater than OLO_CONTAINER_SLACK_GROWTH_FACTOR_NUMERATOR");

    // ============================================================================
    // Numeric Limits Helper
    // ============================================================================

    template <typename T>
    struct TNumericLimits
    {
        static constexpr T Max() { return std::numeric_limits<T>::max(); }
        static constexpr T Min() { return std::numeric_limits<T>::min(); }
        static constexpr T Lowest() { return std::numeric_limits<T>::lowest(); }
    };

    // ============================================================================
    // Slack Calculation Functions
    // ============================================================================

    /**
     * @brief Calculate slack when shrinking an array
     * 
     * @param NewMax            The number of elements to allocate space for
     * @param CurrentMax        The number of elements for which space is currently allocated
     * @param BytesPerElement   The number of bytes/element
     * @param bAllowQuantize    Whether to quantize the allocation size
     * @param Alignment         Alignment requirement
     * @return The recommended allocation size
     */
    template <typename SizeType>
    OLO_FINLINE SizeType DefaultCalculateSlackShrink(SizeType NewMax, SizeType CurrentMax, sizet BytesPerElement, 
                                                      bool bAllowQuantize, u32 Alignment = DEFAULT_ALIGNMENT)
    {
        SizeType Retval;
        OLO_CORE_ASSERT(NewMax < CurrentMax, "NewMax must be less than CurrentMax");

        // If the container has too much slack, shrink it to exactly fit the number of elements.
        const SizeType CurrentSlackElements = CurrentMax - NewMax;
        const sizet CurrentSlackBytes = static_cast<sizet>(CurrentMax - NewMax) * BytesPerElement;
        const bool bTooManySlackBytes = CurrentSlackBytes >= 16384;
        const bool bTooManySlackElements = 3 * NewMax < 2 * CurrentMax;
        
        if ((bTooManySlackBytes || bTooManySlackElements) && (CurrentSlackElements > 64 || !NewMax))
        {
            Retval = NewMax;
            if (Retval > 0)
            {
                if (bAllowQuantize)
                {
                    Retval = static_cast<SizeType>(FMemory::QuantizeSize(static_cast<sizet>(Retval) * BytesPerElement, Alignment) / BytesPerElement);
                }
            }
        }
        else
        {
            Retval = CurrentMax;
        }

        return Retval;
    }

    /**
     * @brief Calculate slack when growing an array
     * 
     * @param NewMax            The number of elements to allocate space for
     * @param CurrentMax        The number of elements for which space is currently allocated
     * @param BytesPerElement   The number of bytes/element
     * @param bAllowQuantize    Whether to quantize the allocation size
     * @param Alignment         Alignment requirement
     * @return The recommended allocation size
     */
    template <typename SizeType>
    OLO_FINLINE SizeType DefaultCalculateSlackGrow(SizeType NewMax, SizeType CurrentMax, sizet BytesPerElement, 
                                                    bool bAllowQuantize, u32 Alignment = DEFAULT_ALIGNMENT)
    {
    #if AGGRESSIVE_MEMORY_SAVING
        const sizet FirstGrow = 1;
        const sizet ConstantGrow = 0;
    #else
        const sizet FirstGrow = 4;
        const sizet ConstantGrow = 16;
    #endif

        SizeType Retval;
        OLO_CORE_ASSERT(NewMax > CurrentMax && NewMax > 0, "NewMax must be greater than CurrentMax and positive");

        sizet Grow = FirstGrow; // this is the amount for the first alloc

    #if CONTAINER_INITIAL_ALLOC_ZERO_SLACK
        if (CurrentMax)
        {
            // Allocate slack for the array proportional to its size.
            Grow = static_cast<sizet>(NewMax) + OLO_CONTAINER_SLACK_GROWTH_FACTOR_NUMERATOR * static_cast<sizet>(NewMax) / OLO_CONTAINER_SLACK_GROWTH_FACTOR_DENOMINATOR + ConstantGrow;
        }
        else if (static_cast<sizet>(NewMax) > Grow)
        {
            Grow = static_cast<sizet>(NewMax);
        }
    #else
        if (CurrentMax || static_cast<sizet>(NewMax) > Grow)
        {
            // Allocate slack for the array proportional to its size.
            Grow = static_cast<sizet>(NewMax) + OLO_CONTAINER_SLACK_GROWTH_FACTOR_NUMERATOR * static_cast<sizet>(NewMax) / OLO_CONTAINER_SLACK_GROWTH_FACTOR_DENOMINATOR + ConstantGrow;
        }
    #endif

        if (bAllowQuantize)
        {
            Retval = static_cast<SizeType>(FMemory::QuantizeSize(Grow * BytesPerElement, Alignment) / BytesPerElement);
        }
        else
        {
            Retval = static_cast<SizeType>(Grow);
        }
        
        // NumElements and MaxElements are stored in signed integers so we must be careful not to overflow here.
        if (NewMax > Retval)
        {
            Retval = TNumericLimits<SizeType>::Max();
        }

        return Retval;
    }

    /**
     * @brief Calculate slack when reserving space
     * 
     * @param NewMax            The number of elements to allocate space for
     * @param BytesPerElement   The number of bytes/element
     * @param bAllowQuantize    Whether to quantize the allocation size
     * @param Alignment         Alignment requirement
     * @return The recommended allocation size
     */
    template <typename SizeType>
    OLO_FINLINE SizeType DefaultCalculateSlackReserve(SizeType NewMax, sizet BytesPerElement, 
                                                       bool bAllowQuantize, u32 Alignment = DEFAULT_ALIGNMENT)
    {
        SizeType Retval = NewMax;
        OLO_CORE_ASSERT(NewMax > 0, "NewMax must be positive");
        
        if (bAllowQuantize)
        {
            Retval = static_cast<SizeType>(FMemory::QuantizeSize(static_cast<sizet>(Retval) * BytesPerElement, Alignment) / BytesPerElement);
            // NumElements and MaxElements are stored in signed integers so we must be careful not to overflow here.
            if (NewMax > Retval)
            {
                Retval = TNumericLimits<SizeType>::Max();
            }
        }

        return Retval;
    }

    // ============================================================================
    // Script Container Element (type-erased element)
    // ============================================================================

    /** A type which is used to represent a script type that is unknown at compile time. */
    struct FScriptContainerElement
    {
    };

    // ============================================================================
    // Allocator Traits
    // ============================================================================

    /**
     * @struct TAllocatorTraitsBase
     * @brief Base traits for allocators
     */
    template <typename AllocatorType>
    struct TAllocatorTraitsBase
    {
        enum { IsZeroConstruct           = false };  // Does the allocator zero-initialize new memory?
        enum { SupportsFreezeMemoryImage = false };  // Can the allocator be frozen for memory images?
        enum { SupportsElementAlignment  = false };  // Does the allocator support custom element alignment?
        enum { SupportsSlackTracking     = false };  // Does the allocator support slack tracking?
    };

    /**
     * @struct TAllocatorTraits
     * @brief Traits for container allocators - specialize for custom allocators
     */
    template <typename AllocatorType>
    struct TAllocatorTraits : TAllocatorTraitsBase<AllocatorType>
    {
    };

    /**
     * @struct TCanMoveBetweenAllocators
     * @brief Check if data can be moved between two allocator types
     */
    template <typename FromAllocatorType, typename ToAllocatorType>
    struct TCanMoveBetweenAllocators
    {
        enum { Value = false };
    };

    // ============================================================================
    // Bits to Size Type Mapping
    // ============================================================================

    template <i32 IndexSize>
    struct TBitsToSizeType
    {
        // Fabricate a compile-time false result that's still dependent on the template parameter
        static_assert(IndexSize == IndexSize + 1, "Unsupported allocator index size.");
    };

    template <> struct TBitsToSizeType<8>  { using Type = i8;  };
    template <> struct TBitsToSizeType<16> { using Type = i16; };
    template <> struct TBitsToSizeType<32> { using Type = i32; };
    template <> struct TBitsToSizeType<64> { using Type = i64; };

    // ============================================================================
    // TAlignedHeapAllocator
    // ============================================================================

    namespace Detail
    {
        [[noreturn]] inline void OnInvalidAlignedHeapAllocatorNum(i32 NewNum, sizet NumBytesPerElement)
        {
            OLO_CORE_ASSERT(false, "Invalid heap allocator num: NewNum={}, NumBytesPerElement={}", NewNum, NumBytesPerElement);
            std::abort(); // In case asserts are disabled
        }

        [[noreturn]] inline void OnInvalidSizedHeapAllocatorNum(i32 IndexSize, i64 NewNum, sizet NumBytesPerElement)
        {
            OLO_CORE_ASSERT(false, "Invalid sized heap allocator num: IndexSize={}, NewNum={}, NumBytesPerElement={}", 
                           IndexSize, NewNum, NumBytesPerElement);
            std::abort(); // In case asserts are disabled
        }
    }

    /**
     * @class TAlignedHeapAllocator
     * @brief Heap allocator with custom alignment
     * 
     * The indirect allocation policy always allocates the elements indirectly.
     * 
     * @tparam Alignment The alignment for allocations (default: DEFAULT_ALIGNMENT)
     */
    template<u32 Alignment = DEFAULT_ALIGNMENT>
    class TAlignedHeapAllocator
    {
    public:
        using SizeType = i32;

        enum { NeedsElementType = false };
        enum { RequireRangeCheck = true };

        class ForAnyElementType
        {
        public:
            /** Default constructor. */
            ForAnyElementType()
                : Data(nullptr)
            {}

            /**
             * Moves the state of another allocator into this one.
             * Assumes that the allocator is currently empty.
             * @param Other - The allocator to move the state from. This allocator should be left in a valid empty state.
             */
            OLO_FINLINE void MoveToEmpty(ForAnyElementType& Other)
            {
                OLO_CORE_ASSERT(this != &Other, "Cannot move to self");

                if (Data)
                {
#if OLO_ENABLE_ARRAY_SLACK_TRACKING
                    FArraySlackTrackingHeader::Free(Data);
#else
                    FMemory::Free(Data);
#endif
                }

                Data = Other.Data;
                Other.Data = nullptr;
            }

            /** Destructor. */
            OLO_FINLINE ~ForAnyElementType()
            {
                if (Data)
                {
#if OLO_ENABLE_ARRAY_SLACK_TRACKING
                    FArraySlackTrackingHeader::Free(Data);
#else
                    FMemory::Free(Data);
#endif
                }
            }

            // FContainerAllocatorInterface
            OLO_FINLINE FScriptContainerElement* GetAllocation() const
            {
                return Data;
            }

            void ResizeAllocation(SizeType /*CurrentNum*/, SizeType NewMax, sizet NumBytesPerElement)
            {
                // Avoid calling FMemory::Realloc(nullptr, 0) as ANSI C mandates returning a valid pointer which is not what we want.
                if (Data || NewMax)
                {
                    static_assert(sizeof(i32) <= sizeof(sizet), "sizet is expected to be larger than i32");

                    // Check for under/overflow
                    if (NewMax < 0 || NumBytesPerElement < 1 || NumBytesPerElement > static_cast<sizet>(MAX_i32))
                    {
                        Detail::OnInvalidAlignedHeapAllocatorNum(NewMax, NumBytesPerElement);
                    }

#if OLO_ENABLE_ARRAY_SLACK_TRACKING
                    constexpr u32 SlackAlignment = (Alignment > alignof(FArraySlackTrackingHeader)) ? Alignment : alignof(FArraySlackTrackingHeader);
                    Data = static_cast<FScriptContainerElement*>(FArraySlackTrackingHeader::Realloc(Data, NewMax, NumBytesPerElement, SlackAlignment));
#else
                    Data = static_cast<FScriptContainerElement*>(FMemory::Realloc(Data, static_cast<sizet>(NewMax) * NumBytesPerElement, Alignment));
#endif
                }
            }

            OLO_FINLINE SizeType CalculateSlackReserve(SizeType NewMax, sizet NumBytesPerElement) const
            {
                return DefaultCalculateSlackReserve(NewMax, NumBytesPerElement, true, Alignment);
            }

            OLO_FINLINE SizeType CalculateSlackShrink(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                return DefaultCalculateSlackShrink(NewMax, CurrentMax, NumBytesPerElement, true, Alignment);
            }

            OLO_FINLINE SizeType CalculateSlackGrow(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                return DefaultCalculateSlackGrow(NewMax, CurrentMax, NumBytesPerElement, true, Alignment);
            }

            sizet GetAllocatedSize(SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                return static_cast<sizet>(CurrentMax) * NumBytesPerElement;
            }

            bool HasAllocation() const
            {
                return !!Data;
            }

            SizeType GetInitialCapacity() const
            {
                return 0;
            }

#if OLO_ENABLE_ARRAY_SLACK_TRACKING
            /** Called when ArrayNum changes for slack tracking */
            OLO_FINLINE void SlackTrackerLogNum(SizeType NewNumUsed)
            {
                FArraySlackTrackingHeader::UpdateNumUsed(Data, static_cast<i64>(NewNumUsed));
            }

            /** Suppress slack tracking on an allocation */
            OLO_FINLINE void DisableSlackTracking()
            {
                FArraySlackTrackingHeader::DisableTracking(Data);
            }
#endif

        private:
            ForAnyElementType(const ForAnyElementType&) = delete;
            ForAnyElementType& operator=(const ForAnyElementType&) = delete;

            /** A pointer to the container's elements. */
            FScriptContainerElement* Data;
        };

        template<typename ElementType>
        class ForElementType : public ForAnyElementType
        {
            static constexpr sizet MinimumAlignment = (Alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) ? __STDCPP_DEFAULT_NEW_ALIGNMENT__ : Alignment;

        public:
            /** Default constructor. */
            ForElementType()
            {
                static_assert(alignof(ElementType) <= MinimumAlignment, 
                    "Using TAlignedHeapAllocator with an alignment lower than the element type's alignment - please update the alignment parameter");
            }

            OLO_FINLINE ElementType* GetAllocation() const
            {
                return reinterpret_cast<ElementType*>(ForAnyElementType::GetAllocation());
            }
        };
    };

    template <u32 Alignment>
    struct TAllocatorTraits<TAlignedHeapAllocator<Alignment>> : TAllocatorTraitsBase<TAlignedHeapAllocator<Alignment>>
    {
        enum { IsZeroConstruct = true };
        enum { SupportsSlackTracking = true };
    };

    // ============================================================================
    // TSizedHeapAllocator
    // ============================================================================

    /**
     * @class TSizedHeapAllocator
     * @brief Heap allocator with configurable index size
     * 
     * The indirect allocation policy always allocates the elements indirectly.
     * 
     * @tparam IndexSize Size of the index type in bits (8, 16, 32, or 64)
     * @tparam BaseMallocType The malloc type to use for allocations (default: FMemory)
     */
    template <i32 IndexSize, typename BaseMallocType = FMemory>
    class TSizedHeapAllocator
    {
    public:
        using SizeType = typename TBitsToSizeType<IndexSize>::Type;
        using BaseMalloc = BaseMallocType;

    private:
        using USizeType = std::make_unsigned_t<SizeType>;

    public:
        enum { NeedsElementType = false };
        enum { RequireRangeCheck = true };

        class ForAnyElementType
        {
            template <i32, typename>
            friend class TSizedHeapAllocator;

        public:
            /** Default constructor. */
            constexpr ForAnyElementType()
                : Data(nullptr)
            {}

            /** Explicitly consteval constructor for compile-time constant arrays. */
            explicit consteval ForAnyElementType(EConstEval)
                : Data(nullptr)
            {}

            /**
             * Moves the state of another allocator into this one.
             * Assumes that the allocator is currently empty.
             * @param Other - The allocator to move the state from.
             */
            template <typename OtherAllocator>
            OLO_FINLINE void MoveToEmptyFromOtherAllocator(typename OtherAllocator::ForAnyElementType& Other)
            {
                OLO_CORE_ASSERT(reinterpret_cast<void*>(this) != reinterpret_cast<void*>(&Other), "Cannot move to self");

                if (Data)
                {
#if OLO_ENABLE_ARRAY_SLACK_TRACKING
                    FArraySlackTrackingHeader::Free(Data);
#else
                    BaseMallocType::Free(Data);
#endif
                }

                Data = Other.Data;
                Other.Data = nullptr;
            }

            /**
             * Moves the state of another allocator into this one (same allocator type).
             */
            OLO_FINLINE void MoveToEmpty(ForAnyElementType& Other)
            {
                this->MoveToEmptyFromOtherAllocator<TSizedHeapAllocator>(Other);
            }

            /** Destructor. */
            OLO_FINLINE ~ForAnyElementType()
            {
                if (Data)
                {
#if OLO_ENABLE_ARRAY_SLACK_TRACKING
                    FArraySlackTrackingHeader::Free(Data);
#else
                    BaseMallocType::Free(Data);
#endif
                }
            }

            // FContainerAllocatorInterface
            OLO_FINLINE FScriptContainerElement* GetAllocation() const
            {
                return Data;
            }

            void ResizeAllocation(SizeType /*CurrentNum*/, SizeType NewMax, sizet NumBytesPerElement)
            {
                // Avoid calling FMemory::Realloc(nullptr, 0)
                if (Data || NewMax)
                {
                    static_assert(sizeof(SizeType) <= sizeof(sizet), "sizet is expected to handle all possible sizes");

                    // Check for under/overflow
                    bool bInvalidResize = NewMax < 0 || NumBytesPerElement < 1 || NumBytesPerElement > static_cast<sizet>(MAX_i32);
                    if constexpr (sizeof(SizeType) == sizeof(sizet))
                    {
                        bInvalidResize = bInvalidResize || (static_cast<sizet>(static_cast<USizeType>(NewMax)) > static_cast<sizet>(TNumericLimits<SizeType>::Max()) / NumBytesPerElement);
                    }
                    if (bInvalidResize)
                    {
                        Detail::OnInvalidSizedHeapAllocatorNum(IndexSize, NewMax, NumBytesPerElement);
                    }

#if OLO_ENABLE_ARRAY_SLACK_TRACKING
                    Data = static_cast<FScriptContainerElement*>(FArraySlackTrackingHeader::Realloc(Data, NewMax, NumBytesPerElement, alignof(FArraySlackTrackingHeader)));
#else
                    Data = static_cast<FScriptContainerElement*>(BaseMallocType::Realloc(Data, static_cast<sizet>(NewMax) * NumBytesPerElement));
#endif
                }
            }

            void ResizeAllocation(SizeType /*CurrentNum*/, SizeType NewMax, sizet NumBytesPerElement, u32 AlignmentOfElement)
            {
                // Avoid calling FMemory::Realloc(nullptr, 0)
                if (Data || NewMax)
                {
                    static_assert(sizeof(SizeType) <= sizeof(sizet), "sizet is expected to handle all possible sizes");

                    // Check for under/overflow
                    bool bInvalidResize = NewMax < 0 || NumBytesPerElement < 1 || NumBytesPerElement > static_cast<sizet>(MAX_i32);
                    if constexpr (sizeof(SizeType) == sizeof(sizet))
                    {
                        bInvalidResize = bInvalidResize || (static_cast<sizet>(static_cast<USizeType>(NewMax)) > static_cast<sizet>(TNumericLimits<SizeType>::Max()) / NumBytesPerElement);
                    }
                    if (bInvalidResize)
                    {
                        Detail::OnInvalidSizedHeapAllocatorNum(IndexSize, NewMax, NumBytesPerElement);
                    }

#if OLO_ENABLE_ARRAY_SLACK_TRACKING
                    u32 const SlackAlignment = (AlignmentOfElement > alignof(FArraySlackTrackingHeader)) ? AlignmentOfElement : alignof(FArraySlackTrackingHeader);
                    Data = static_cast<FScriptContainerElement*>(FArraySlackTrackingHeader::Realloc(Data, NewMax, NumBytesPerElement, SlackAlignment));
#else
                    Data = static_cast<FScriptContainerElement*>(BaseMallocType::Realloc(Data, static_cast<sizet>(NewMax) * NumBytesPerElement, AlignmentOfElement));
#endif
                }
            }

            OLO_FINLINE SizeType CalculateSlackReserve(SizeType NewMax, sizet NumBytesPerElement) const
            {
                return DefaultCalculateSlackReserve(NewMax, NumBytesPerElement, true);
            }

            OLO_FINLINE SizeType CalculateSlackReserve(SizeType NewMax, sizet NumBytesPerElement, u32 AlignmentOfElement) const
            {
                return DefaultCalculateSlackReserve(NewMax, NumBytesPerElement, true, AlignmentOfElement);
            }

            OLO_FINLINE SizeType CalculateSlackShrink(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                return DefaultCalculateSlackShrink(NewMax, CurrentMax, NumBytesPerElement, true);
            }

            OLO_FINLINE SizeType CalculateSlackShrink(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement, u32 AlignmentOfElement) const
            {
                return DefaultCalculateSlackShrink(NewMax, CurrentMax, NumBytesPerElement, true, AlignmentOfElement);
            }

            OLO_FINLINE SizeType CalculateSlackGrow(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                return DefaultCalculateSlackGrow(NewMax, CurrentMax, NumBytesPerElement, true);
            }

            OLO_FINLINE SizeType CalculateSlackGrow(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement, u32 AlignmentOfElement) const
            {
                return DefaultCalculateSlackGrow(NewMax, CurrentMax, NumBytesPerElement, true, AlignmentOfElement);
            }

            sizet GetAllocatedSize(SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                return static_cast<sizet>(CurrentMax) * NumBytesPerElement;
            }

            bool HasAllocation() const
            {
                return !!Data;
            }

            constexpr SizeType GetInitialCapacity() const
            {
                return 0;
            }

#if OLO_ENABLE_ARRAY_SLACK_TRACKING
            /** Called when ArrayNum changes for slack tracking */
            OLO_FINLINE void SlackTrackerLogNum(SizeType NewNumUsed)
            {
                FArraySlackTrackingHeader::UpdateNumUsed(Data, static_cast<i64>(NewNumUsed));
            }

            /** Suppress slack tracking on an allocation */
            OLO_FINLINE void DisableSlackTracking()
            {
                FArraySlackTrackingHeader::DisableTracking(Data);
            }
#endif

        private:
            ForAnyElementType(const ForAnyElementType&) = delete;
            ForAnyElementType& operator=(const ForAnyElementType&) = delete;

            /** A pointer to the container's elements. */
            FScriptContainerElement* Data;
        };

        template<typename ElementType>
        class ForElementType : public ForAnyElementType
        {
        public:
            /** Default constructor. */
            ForElementType() = default;

            /** Explicitly consteval constructor for compile-time constant arrays. */
            explicit consteval ForElementType(EConstEval)
                : ForAnyElementType(ConstEval)
            {}

            OLO_FINLINE ElementType* GetAllocation() const
            {
                return reinterpret_cast<ElementType*>(ForAnyElementType::GetAllocation());
            }
        };
    };

    template <u8 IndexSize>
    struct TAllocatorTraits<TSizedHeapAllocator<IndexSize>> : TAllocatorTraitsBase<TSizedHeapAllocator<IndexSize>>
    {
        enum { IsZeroConstruct          = true };
        enum { SupportsElementAlignment = true };
        enum { SupportsSlackTracking    = true };
    };

    // Allow conversions between different int width versions of the allocator
    template <u8 FromIndexSize, u8 ToIndexSize>
    struct TCanMoveBetweenAllocators<TSizedHeapAllocator<FromIndexSize>, TSizedHeapAllocator<ToIndexSize>>
    {
        enum { Value = true };
    };

    // ============================================================================
    // Default Allocator Aliases
    // ============================================================================

    /** Default sized allocator (32-bit indices) - inherits from TSizedHeapAllocator<32> */
    template <i32 IndexSize>
    class TSizedDefaultAllocator : public TSizedHeapAllocator<IndexSize>
    {
    public:
        using Typedef = TSizedHeapAllocator<IndexSize>;
    };

    /** The default allocator used by TArray - uses 32-bit signed indices */
    using FDefaultAllocator = TSizedDefaultAllocator<32>;

    /** 64-bit index allocator */
    using FDefaultAllocator64 = TSizedDefaultAllocator<64>;

    /** Standard heap allocator alias */
    using FHeapAllocator = TSizedHeapAllocator<32>;

    // Traits for default allocator
    template <i32 IndexSize>
    struct TAllocatorTraits<TSizedDefaultAllocator<IndexSize>> : TAllocatorTraits<typename TSizedDefaultAllocator<IndexSize>::Typedef>
    {
    };

    template <>
    struct TAllocatorTraits<FDefaultAllocator> : TAllocatorTraits<typename FDefaultAllocator::Typedef>
    {
    };

    // Allow moving between different sized default allocators
    template <i32 FromIndexSize, i32 ToIndexSize>
    struct TCanMoveBetweenAllocators<TSizedDefaultAllocator<FromIndexSize>, TSizedDefaultAllocator<ToIndexSize>> 
        : TCanMoveBetweenAllocators<typename TSizedDefaultAllocator<FromIndexSize>::Typedef, typename TSizedDefaultAllocator<ToIndexSize>::Typedef>
    {
    };

    // ============================================================================
    // TSizedInlineAllocator
    // ============================================================================

    namespace Detail
    {
        // This concept allows us to support allocators which don't specify `enum { ShrinkByDefault = ...; }`.
        template <typename AllocatorType>
        concept CHasShrinkByDefault = requires { AllocatorType::ShrinkByDefault; };

        // Returns the value of AllocatorType::ShrinkByDefault if the enum exists, or bFallback if the enum is absent.
        template <bool bFallback, typename AllocatorType>
        consteval bool ShrinkByDefaultOr()
        {
            if constexpr (CHasShrinkByDefault<AllocatorType>)
            {
                return AllocatorType::ShrinkByDefault;
            }
            else
            {
                return bFallback;
            }
        }
    }

    /**
     * @class TSizedInlineAllocator
     * @brief Inline allocator with secondary heap fallback
     * 
     * Allocates up to NumInlineElements in embedded storage, then falls back to
     * a secondary allocator for larger allocations.
     * 
     * @tparam NumInlineElements    Maximum number of elements to store inline
     * @tparam IndexSize           Size of the index type in bits (8, 16, 32, or 64)
     * @tparam SecondaryAllocator  Allocator to use when inline storage is exceeded
     */
    template <u32 NumInlineElements, i32 IndexSize, typename SecondaryAllocator = FDefaultAllocator>
    class TSizedInlineAllocator
    {
    public:
        using SizeType = typename TBitsToSizeType<IndexSize>::Type;

        static_assert(std::is_same_v<SizeType, typename SecondaryAllocator::SizeType>, "Secondary allocator SizeType mismatch");

        enum { NeedsElementType = true };
        enum { RequireRangeCheck = true };
        enum { ShrinkByDefault = Detail::ShrinkByDefaultOr<true, SecondaryAllocator>() };

        template<typename ElementType>
        class ForElementType
        {
        public:
            /** Default constructor. */
            constexpr ForElementType()
            {
                // In consteval context, zero-initialize the inline data
                if (std::is_constant_evaluated())
                {
                    for (u8& Byte : InlineData)
                    {
                        Byte = 0;
                    }
                }
            }

            /** Explicitly consteval constructor for compile-time constant arrays. */
            explicit consteval ForElementType(EConstEval)
                : InlineData() // Force value initialization
                , SecondaryData(ConstEval)
            {}

            /**
             * Moves the state of another allocator into this one.
             * Assumes that the allocator is currently empty.
             * @param Other - The allocator to move the state from.
             */
            OLO_FINLINE void MoveToEmpty(ForElementType& Other)
            {
                OLO_CORE_ASSERT(this != &Other, "Cannot move to self");

                if (!Other.SecondaryData.GetAllocation())
                {
                    // Relocate objects from other inline storage only if it was stored inline in Other
                    RelocateConstructItems<ElementType>(static_cast<void*>(InlineData), Other.GetInlineElements(), NumInlineElements);
                }

                // Move secondary storage in any case.
                // This will move secondary storage if it exists but will also handle the case where 
                // secondary storage is used in Other but not in *this.
                SecondaryData.MoveToEmpty(Other.SecondaryData);
            }

            // FContainerAllocatorInterface
            OLO_FINLINE ElementType* GetAllocation() const
            {
                if (ElementType* Result = SecondaryData.GetAllocation())
                {
                    return Result;
                }
                return GetInlineElements();
            }

            void ResizeAllocation(SizeType CurrentNum, SizeType NewMax, sizet NumBytesPerElement)
            {
                // Make sure the number of live elements is still within the allocation since we only memmove, not destruct
                OLO_CORE_ASSERT(CurrentNum <= NewMax, "CurrentNum must be <= NewMax");

                // Check if the new allocation will fit in the inline data area.
                if (NewMax <= static_cast<SizeType>(NumInlineElements))
                {
                    // If the old allocation wasn't in the inline data area, relocate it into the inline data area.
                    if (SecondaryData.GetAllocation())
                    {
                        RelocateConstructItems<ElementType>(static_cast<void*>(InlineData), 
                            reinterpret_cast<ElementType*>(SecondaryData.GetAllocation()), CurrentNum);

                        // Free the old indirect allocation.
                        SecondaryData.ResizeAllocation(0, 0, NumBytesPerElement);
                    }
                }
                else
                {
                    if (!SecondaryData.GetAllocation())
                    {
                        // Allocate new indirect memory for the data.
                        SecondaryData.ResizeAllocation(0, NewMax, NumBytesPerElement);

                        // Move the data out of the inline data area into the new allocation.
                        RelocateConstructItems<ElementType>(SecondaryData.GetAllocation(), GetInlineElements(), CurrentNum);
                    }
                    else
                    {
                        // Reallocate the indirect data for the new size.
                        SecondaryData.ResizeAllocation(CurrentNum, NewMax, NumBytesPerElement);
                    }
                }
            }

            OLO_FINLINE SizeType CalculateSlackReserve(SizeType NewMax, sizet NumBytesPerElement) const
            {
                // If the elements use less space than the inline allocation, only use the inline allocation as slack.
                return NewMax <= static_cast<SizeType>(NumInlineElements) ?
                    static_cast<SizeType>(NumInlineElements) :
                    SecondaryData.CalculateSlackReserve(NewMax, NumBytesPerElement);
            }

            OLO_FINLINE SizeType CalculateSlackShrink(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                // If the elements use less space than the inline allocation, only use the inline allocation as slack.
                return NewMax <= static_cast<SizeType>(NumInlineElements) ?
                    static_cast<SizeType>(NumInlineElements) :
                    SecondaryData.CalculateSlackShrink(NewMax, CurrentMax, NumBytesPerElement);
            }

            OLO_FINLINE SizeType CalculateSlackGrow(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                // If the elements use less space than the inline allocation, only use the inline allocation as slack.
                // Also, when computing slack growth, don't count inline elements -- the slack algorithm has a special
                // case to save memory on the initial heap allocation, versus subsequent reallocations, and we don't
                // want the inline elements to be treated as if they were the first heap allocation.
                return NewMax <= static_cast<SizeType>(NumInlineElements) ?
                    static_cast<SizeType>(NumInlineElements) :
                    SecondaryData.CalculateSlackGrow(NewMax, CurrentMax <= static_cast<SizeType>(NumInlineElements) ? 0 : CurrentMax, NumBytesPerElement);
            }

            sizet GetAllocatedSize(SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                if (CurrentMax > static_cast<SizeType>(NumInlineElements))
                {
                    return SecondaryData.GetAllocatedSize(CurrentMax, NumBytesPerElement);
                }
                return 0;
            }

            bool HasAllocation() const
            {
                return SecondaryData.HasAllocation();
            }

            constexpr SizeType GetInitialCapacity() const
            {
                return static_cast<SizeType>(NumInlineElements);
            }

#if OLO_ENABLE_ARRAY_SLACK_TRACKING
            /** Called when ArrayNum changes for slack tracking (delegates to secondary) */
            OLO_FINLINE void SlackTrackerLogNum(SizeType NewNumUsed)
            {
                // Only delegate if we're using secondary storage
                if (SecondaryData.HasAllocation())
                {
                    SecondaryData.SlackTrackerLogNum(NewNumUsed);
                }
            }

            /** Suppress slack tracking on an allocation (delegates to secondary) */
            OLO_FINLINE void DisableSlackTracking()
            {
                if (SecondaryData.HasAllocation())
                {
                    SecondaryData.DisableSlackTracking();
                }
            }
#endif

        private:
            ForElementType(const ForElementType&) = delete;
            ForElementType& operator=(const ForElementType&) = delete;

            /** The data is stored in this array if less than NumInlineElements is needed. Uninitialized by default. */
            alignas(ElementType) u8 InlineData[sizeof(ElementType) * NumInlineElements];

            /** The data is allocated through the indirect allocation policy if more than NumInlineElements is needed. */
            typename SecondaryAllocator::template ForElementType<ElementType> SecondaryData;

            /** @return the base of the aligned inline element data */
            ElementType* GetInlineElements() const
            {
                return reinterpret_cast<ElementType*>(const_cast<u8*>(InlineData));
            }
        };

        using ForAnyElementType = void;
    };

    template <u32 NumInlineElements, i32 IndexSize, typename SecondaryAllocator>
    struct TAllocatorTraits<TSizedInlineAllocator<NumInlineElements, IndexSize, SecondaryAllocator>> 
        : TAllocatorTraitsBase<TSizedInlineAllocator<NumInlineElements, IndexSize, SecondaryAllocator>>
    {
        enum { SupportsSlackTracking = true };
    };

    // Convenience aliases
    template <u32 NumInlineElements, typename SecondaryAllocator = FDefaultAllocator>
    using TInlineAllocator = TSizedInlineAllocator<NumInlineElements, 32, SecondaryAllocator>;

    template <u32 NumInlineElements, typename SecondaryAllocator = FDefaultAllocator64>
    using TInlineAllocator64 = TSizedInlineAllocator<NumInlineElements, 64, SecondaryAllocator>;

    // ============================================================================
    // TSizedNonshrinkingAllocator
    // ============================================================================

    /**
     * @class TSizedNonshrinkingAllocator
     * @brief Heap allocator that prevents automatic shrinking unless explicitly requested
     */
    template <i32 IndexSize>
    class TSizedNonshrinkingAllocator : public TSizedHeapAllocator<IndexSize>
    {
    public:
        using Typedef = TSizedHeapAllocator<IndexSize>;
        static constexpr bool ShrinkByDefault = false;
    };

    template <i32 IndexSize>
    struct TAllocatorTraits<TSizedNonshrinkingAllocator<IndexSize>> 
        : TAllocatorTraits<typename TSizedNonshrinkingAllocator<IndexSize>::Typedef>
    {
    };

    // ============================================================================
    // TFixedAllocator
    // ============================================================================

    /**
     * @class TFixedAllocator
     * @brief Fixed-size inline allocator with no secondary storage
     * 
     * Allocates up to a specified number of elements inline with the container.
     * Does not provide secondary storage when inline storage is exhausted.
     */
    template <u32 NumInlineElements>
    class TFixedAllocator
    {
    public:
        using SizeType = i32;

        enum { NeedsElementType = true };
        enum { RequireRangeCheck = true };
        static constexpr bool ShrinkByDefault = false;

        template <typename ElementType>
        class ForElementType
        {
        public:
            ForElementType() = default;

            OLO_FINLINE void MoveToEmpty(ForElementType& Other)
            {
                OLO_CORE_ASSERT(this != &Other, "Cannot move to self");
                RelocateConstructItems<ElementType>(GetInlineElements(), Other.GetInlineElements(), NumInlineElements);
            }

            OLO_FINLINE ElementType* GetAllocation() const
            {
                return GetInlineElements();
            }

            void ResizeAllocation(SizeType /*CurrentNum*/, SizeType NewMax, sizet /*NumBytesPerElement*/)
            {
                OLO_CORE_ASSERT(NewMax >= 0 && static_cast<u32>(NewMax) <= NumInlineElements, 
                               "TFixedAllocator cannot allocate more than NumInlineElements");
            }

            OLO_FINLINE SizeType CalculateSlackReserve(SizeType NewMax, sizet /*NumBytesPerElement*/) const
            {
                OLO_CORE_ASSERT(static_cast<u32>(NewMax) <= NumInlineElements, "Request exceeds inline capacity");
                return static_cast<SizeType>(NumInlineElements);
            }

            OLO_FINLINE SizeType CalculateSlackShrink(SizeType /*NewMax*/, SizeType /*CurrentMax*/, sizet /*NumBytesPerElement*/) const
            {
                return static_cast<SizeType>(NumInlineElements);
            }

            OLO_FINLINE SizeType CalculateSlackGrow(SizeType NewMax, SizeType /*CurrentMax*/, sizet /*NumBytesPerElement*/) const
            {
                OLO_CORE_ASSERT(static_cast<u32>(NewMax) <= NumInlineElements, "Request exceeds inline capacity");
                return static_cast<SizeType>(NumInlineElements);
            }

            sizet GetAllocatedSize(SizeType /*CurrentMax*/, sizet /*NumBytesPerElement*/) const
            {
                return 0; // No heap allocation
            }

            bool HasAllocation() const
            {
                return false; // Always inline
            }

            SizeType GetInitialCapacity() const
            {
                return static_cast<SizeType>(NumInlineElements);
            }

        private:
            ForElementType(const ForElementType&) = delete;
            ForElementType& operator=(const ForElementType&) = delete;

            alignas(ElementType) u8 InlineData[NumInlineElements * sizeof(ElementType)];

            ElementType* GetInlineElements() const
            {
                return reinterpret_cast<ElementType*>(const_cast<u8*>(InlineData));
            }
        };

        using ForAnyElementType = void;
    };

    template <u32 NumInlineElements>
    struct TAllocatorTraits<TFixedAllocator<NumInlineElements>> 
        : TAllocatorTraitsBase<TFixedAllocator<NumInlineElements>>
    {
    };

    // ============================================================================
    // TNonRelocatableInlineAllocator
    // ============================================================================

    /**
     * @class TNonRelocatableInlineAllocator
     * @brief Inline allocator variant that caches the data pointer
     * 
     * A variant of TInlineAllocator with a secondary heap allocator that is allowed to store 
     * a pointer to its inline elements. This allows caching a pointer to the elements which 
     * avoids any conditional logic in GetAllocation(), but prevents the allocator being 
     * trivially relocatable.
     * 
     * All OloEngine allocators typically rely on elements being trivially relocatable, so 
     * instances of this allocator cannot be used in other containers.
     *
     * NOTE: instances of this allocator - or containers which use them - are non-trivially-relocatable, 
     * but the allocator still expects elements themselves to be trivially-relocatable.
     */
    template <u32 NumInlineElements>
    class TNonRelocatableInlineAllocator
    {
    public:
        using SizeType = i32;

        enum { NeedsElementType = true };
        enum { RequireRangeCheck = true };

        template <typename ElementType>
        class ForElementType
        {
        public:
            /** Default constructor - points to inline storage */
            ForElementType()
                : Data(GetInlineElements())
            {
                static_assert(alignof(ElementType) <= DEFAULT_ALIGNMENT, 
                    "TNonRelocatableInlineAllocator uses default alignment, which is lower than the element type's alignment");
            }

            ~ForElementType()
            {
                if (HasAllocation())
                {
                    FMemory::Free(Data);
                }
            }

            /**
             * @brief Moves the state of another allocator into this one
             * 
             * Assumes that the allocator is currently empty, i.e. memory may be allocated 
             * but any existing elements have already been destructed (if necessary).
             * 
             * @param Other The allocator to move the state from
             */
            OLO_FINLINE void MoveToEmpty(ForElementType& Other)
            {
                OLO_CORE_ASSERT(this != &Other, "Cannot move to self");

                if (HasAllocation())
                {
                    FMemory::Free(Data);
                }

                if (Other.HasAllocation())
                {
                    Data = Other.Data;
                    Other.Data = Other.GetInlineElements();
                }
                else
                {
                    Data = GetInlineElements();
                    RelocateConstructItems<ElementType>(GetInlineElements(), Other.GetInlineElements(), NumInlineElements);
                }
            }

            OLO_FINLINE ElementType* GetAllocation() const
            {
                return Data;
            }

            void ResizeAllocation(SizeType CurrentNum, SizeType NewMax, sizet NumBytesPerElement)
            {
                OLO_CORE_ASSERT(CurrentNum <= NewMax, "CurrentNum must be <= NewMax");

                // Check if the new allocation will fit in the inline data area.
                if (static_cast<u32>(NewMax) <= NumInlineElements)
                {
                    // If the old allocation wasn't in the inline data area, relocate it into the inline data area.
                    if (HasAllocation())
                    {
                        RelocateConstructItems<ElementType>(GetInlineElements(), Data, CurrentNum);
                        FMemory::Free(Data);
                        Data = GetInlineElements();
                    }
                }
                else
                {
                    if (HasAllocation())
                    {
                        // Reallocate the indirect data for the new size.
                        Data = reinterpret_cast<ElementType*>(FMemory::Realloc(Data, NewMax * NumBytesPerElement));
                    }
                    else
                    {
                        // Allocate new indirect memory for the data.
                        Data = reinterpret_cast<ElementType*>(FMemory::Realloc(nullptr, NewMax * NumBytesPerElement));

                        // Move the data out of the inline data area into the new allocation.
                        RelocateConstructItems<ElementType>(Data, GetInlineElements(), CurrentNum);
                    }
                }
            }

            OLO_FINLINE SizeType CalculateSlackReserve(SizeType NewMax, sizet NumBytesPerElement) const
            {
                // If the elements use less space than the inline allocation, only use the inline allocation as slack.
                return (static_cast<u32>(NewMax) <= NumInlineElements) ? 
                    static_cast<SizeType>(NumInlineElements) : 
                    DefaultCalculateSlackReserve(NewMax, NumBytesPerElement, true);
            }

            OLO_FINLINE SizeType CalculateSlackShrink(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                // If the elements use less space than the inline allocation, only use the inline allocation as slack.
                return (static_cast<u32>(NewMax) <= NumInlineElements) ? 
                    static_cast<SizeType>(NumInlineElements) : 
                    DefaultCalculateSlackShrink(NewMax, CurrentMax, NumBytesPerElement, true);
            }

            OLO_FINLINE SizeType CalculateSlackGrow(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                // If the elements use less space than the inline allocation, only use the inline allocation as slack.
                return (static_cast<u32>(NewMax) <= NumInlineElements) ? 
                    static_cast<SizeType>(NumInlineElements) : 
                    DefaultCalculateSlackGrow(NewMax, CurrentMax, NumBytesPerElement, true);
            }

            sizet GetAllocatedSize(SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                return HasAllocation() ? (static_cast<sizet>(CurrentMax) * NumBytesPerElement) : 0;
            }

            OLO_FINLINE bool HasAllocation() const
            {
                return Data != GetInlineElements();
            }

            SizeType GetInitialCapacity() const
            {
                return static_cast<SizeType>(NumInlineElements);
            }

        private:
            ForElementType(const ForElementType&) = delete;
            ForElementType& operator=(const ForElementType&) = delete;

            /** Cached pointer to data (either inline or heap-allocated) */
            ElementType* Data;

            /** Inline storage for elements */
            alignas(ElementType) u8 InlineData[NumInlineElements * sizeof(ElementType)];

            /** @return the base of the inline element data */
            OLO_FINLINE ElementType* GetInlineElements() const
            {
                return reinterpret_cast<ElementType*>(const_cast<u8*>(InlineData));
            }
        };

        using ForAnyElementType = void;
    };

    template <u32 NumInlineElements>
    struct TAllocatorTraits<TNonRelocatableInlineAllocator<NumInlineElements>> 
        : TAllocatorTraitsBase<TNonRelocatableInlineAllocator<NumInlineElements>>
    {
        enum { SupportsSlackTracking = true };
    };

    // ============================================================================
    // Bit Array Constants
    // ============================================================================

    // We want these to be correctly typed as i32, but we don't want them to have linkage, so we make them macros
    #define NumBitsPerDWORD (static_cast<i32>(32))
    #define NumBitsPerDWORDLogTwo (static_cast<i32>(5))

    // ============================================================================
    // FDefaultBitArrayAllocator
    // ============================================================================
    // NOTE: Must be defined before TSparseArrayAllocator to avoid TAllocatorTraits
    // instantiation order issues (specialization must precede usage)

    /** Default bit array allocator (inline with 4 DWORDs) */
    class FDefaultBitArrayAllocator : public TInlineAllocator<4>
    {
    public:
        using Typedef = TInlineAllocator<4>;
    };

    template <>
    struct TAllocatorTraits<FDefaultBitArrayAllocator> : TAllocatorTraits<typename FDefaultBitArrayAllocator::Typedef>
    {
    };

    // ============================================================================
    // TSparseArrayAllocator
    // ============================================================================

    /**
     * @class TSparseArrayAllocator
     * @brief Encapsulates allocators used by TSparseArray
     */
    template <typename InElementAllocator = FDefaultAllocator, typename InBitArrayAllocator = FDefaultBitArrayAllocator>
    class TSparseArrayAllocator
    {
    public:
        using ElementAllocator = InElementAllocator;
        using BitArrayAllocator = InBitArrayAllocator;
    };

    template <typename InElementAllocator, typename InBitArrayAllocator>
    struct TAllocatorTraits<TSparseArrayAllocator<InElementAllocator, InBitArrayAllocator>> 
        : TAllocatorTraitsBase<TSparseArrayAllocator<InElementAllocator, InBitArrayAllocator>>
    {
        enum
        {
            SupportsFreezeMemoryImage = TAllocatorTraits<InElementAllocator>::SupportsFreezeMemoryImage && 
                                        TAllocatorTraits<InBitArrayAllocator>::SupportsFreezeMemoryImage,
        };
    };

    /**
     * @class TAlignedSparseArrayAllocator
     * @brief Sparse array allocator with custom alignment
     */
    template <u32 Alignment = DEFAULT_ALIGNMENT, 
              typename InElementAllocator = TAlignedHeapAllocator<Alignment>, 
              typename InBitArrayAllocator = FDefaultBitArrayAllocator>
    class TAlignedSparseArrayAllocator
    {
    public:
        using ElementAllocator = InElementAllocator;
        using BitArrayAllocator = InBitArrayAllocator;
    };

    /**
     * @class TInlineSparseArrayAllocator
     * @brief Inline sparse array allocator with secondary storage fallback
     */
    template <u32 NumInlineElements, typename SecondaryAllocator = TSparseArrayAllocator<FDefaultAllocator, FDefaultAllocator>>
    class TInlineSparseArrayAllocator
    {
    private:
        static constexpr u32 InlineBitArrayDWORDs = (NumInlineElements + 32 - 1) / 32;

    public:
        using ElementAllocator = TInlineAllocator<NumInlineElements, typename SecondaryAllocator::ElementAllocator>;
        using BitArrayAllocator = TInlineAllocator<InlineBitArrayDWORDs, typename SecondaryAllocator::BitArrayAllocator>;
    };

    /**
     * @class TFixedSparseArrayAllocator
     * @brief Fixed-size sparse array allocator with no secondary storage
     */
    template <u32 NumInlineElements>
    class TFixedSparseArrayAllocator
    {
    private:
        static constexpr u32 InlineBitArrayDWORDs = (NumInlineElements + 32 - 1) / 32;

    public:
        using ElementAllocator = TFixedAllocator<NumInlineElements>;
        using BitArrayAllocator = TFixedAllocator<InlineBitArrayDWORDs>;
    };

    // ============================================================================
    // TCompactSetAllocator
    // ============================================================================

    namespace CompactSetAllocatorHelpers
    {
        template <u32 NumInlineElements, i32 ElementSize>
        constexpr i32 CalculateRequiredBytes()
        {
            constexpr u32 TypeSize = 1 + (NumInlineElements > 0xff) + (NumInlineElements > 0xffff) * 2;
            constexpr u32 HashSize = NumInlineElements < 8 ? 4 : 
                (1u << (32 - __builtin_clz((NumInlineElements / 2) + 1 - 1))); // RoundUpToPowerOfTwo
            return static_cast<i32>(Align(NumInlineElements * ElementSize, 4) + 4 + (NumInlineElements + HashSize) * TypeSize);
        }
    }

    /**
     * @class TCompactSetAllocator
     * @brief Allocator for TCompactSet
     */
    template <typename InElementAllocator = FDefaultAllocator>
    struct TCompactSetAllocator
    {
        template <typename ElementType>
        struct AllocatorAlignment
        {
            static constexpr sizet Value = alignof(typename InElementAllocator::template ForElementType<u8>);
        };

        template <i32 ElementSize>
        using ElementAllocator = InElementAllocator;
    };

    template <typename InElementAllocator>
    struct TAllocatorTraits<TCompactSetAllocator<InElementAllocator>> 
        : TAllocatorTraitsBase<TCompactSetAllocator<InElementAllocator>>
    {
        enum
        {
            SupportsFreezeMemoryImage = TAllocatorTraits<InElementAllocator>::SupportsFreezeMemoryImage,
        };
    };

    /**
     * @class TInlineCompactSetAllocator
     * @brief Inline compact set allocator with secondary storage fallback
     */
    template <u32 NumInlineElements, typename SecondaryAllocator = TCompactSetAllocator<>>
    struct TInlineCompactSetAllocator
    {
        template <i32 ElementSize>
        using ElementAllocator = TInlineAllocator<
            CompactSetAllocatorHelpers::CalculateRequiredBytes<NumInlineElements, ElementSize>(),
            typename SecondaryAllocator::template ElementAllocator<ElementSize>>;

        template <typename ElementType>
        struct AllocatorAlignment
        {
            static constexpr sizet ElementAlignof = alignof(ElementType);
            static constexpr sizet AllocatorAlignof = alignof(
                typename ElementAllocator<sizeof(ElementType)>::template ForElementType<u8>);
            static constexpr sizet Value = Max(ElementAlignof, AllocatorAlignof);
        };
    };

    /**
     * @class TFixedCompactSetAllocator
     * @brief Fixed-size compact set allocator with no secondary storage
     */
    template <u32 NumInlineElements>
    struct TFixedCompactSetAllocator
    {
        template <i32 ElementSize>
        using ElementAllocator = TFixedAllocator<
            CompactSetAllocatorHelpers::CalculateRequiredBytes<NumInlineElements, ElementSize>()>;

        template <typename ElementType>
        struct AllocatorAlignment
        {
            static constexpr sizet ElementAlignof = alignof(ElementType);
            static constexpr sizet AllocatorAlignof = alignof(
                typename ElementAllocator<sizeof(ElementType)>::template ForElementType<u8>);
            static constexpr sizet Value = Max(ElementAlignof, AllocatorAlignof);
        };
    };

    // ============================================================================
    // TSparseSetAllocator
    // ============================================================================

    // Default configuration values
    #ifndef DEFAULT_NUMBER_OF_ELEMENTS_PER_HASH_BUCKET
    #define DEFAULT_NUMBER_OF_ELEMENTS_PER_HASH_BUCKET 2
    #endif
    constexpr u32 DEFAULT_BASE_NUMBER_OF_HASH_BUCKETS = 8;
    constexpr u32 DEFAULT_MIN_NUMBER_OF_HASHED_ELEMENTS = 4;

    /**
     * @class TSparseSetAllocator
     * @brief Encapsulates allocators used by TSparseSet
     */
    template <
        typename InSparseArrayAllocator = TSparseArrayAllocator<>,
        typename InHashAllocator = TInlineAllocator<1, FDefaultAllocator>,
        u32 AverageNumberOfElementsPerHashBucket = DEFAULT_NUMBER_OF_ELEMENTS_PER_HASH_BUCKET,
        u32 BaseNumberOfHashBuckets = DEFAULT_BASE_NUMBER_OF_HASH_BUCKETS,
        u32 MinNumberOfHashedElements = DEFAULT_MIN_NUMBER_OF_HASHED_ELEMENTS>
    class TSparseSetAllocator
    {
    public:
        static OLO_FINLINE u32 GetNumberOfHashBuckets(u32 NumHashedElements)
        {
            if (NumHashedElements >= MinNumberOfHashedElements)
            {
                // RoundUpToPowerOfTwo
                u32 Value = NumHashedElements / AverageNumberOfElementsPerHashBucket + BaseNumberOfHashBuckets;
                --Value;
                Value |= Value >> 1;
                Value |= Value >> 2;
                Value |= Value >> 4;
                Value |= Value >> 8;
                Value |= Value >> 16;
                ++Value;
                return Value;
            }
            return 1;
        }

        using SparseArrayAllocator = InSparseArrayAllocator;
        using HashAllocator = InHashAllocator;
    };

    template <
        typename InSparseArrayAllocator,
        typename InHashAllocator,
        u32 AverageNumberOfElementsPerHashBucket,
        u32 BaseNumberOfHashBuckets,
        u32 MinNumberOfHashedElements>
    struct TAllocatorTraits<TSparseSetAllocator<InSparseArrayAllocator, InHashAllocator, 
        AverageNumberOfElementsPerHashBucket, BaseNumberOfHashBuckets, MinNumberOfHashedElements>>
        : TAllocatorTraitsBase<TSparseSetAllocator<InSparseArrayAllocator, InHashAllocator,
            AverageNumberOfElementsPerHashBucket, BaseNumberOfHashBuckets, MinNumberOfHashedElements>>
    {
        enum
        {
            SupportsFreezeMemoryImage = TAllocatorTraits<InSparseArrayAllocator>::SupportsFreezeMemoryImage && 
                                        TAllocatorTraits<InHashAllocator>::SupportsFreezeMemoryImage,
        };
    };

    /**
     * @class TInlineSparseSetAllocator
     * @brief Inline sparse set allocator with secondary storage fallback
     */
    template <
        u32 NumInlineElements,
        typename SecondaryAllocator = TSparseSetAllocator<TSparseArrayAllocator<FDefaultAllocator, FDefaultAllocator>, FDefaultAllocator>,
        u32 AverageNumberOfElementsPerHashBucket = DEFAULT_NUMBER_OF_ELEMENTS_PER_HASH_BUCKET,
        u32 MinNumberOfHashedElements = DEFAULT_MIN_NUMBER_OF_HASHED_ELEMENTS>
    class TInlineSparseSetAllocator
    {
    private:
        static constexpr u32 NumInlineHashBuckets = (NumInlineElements + AverageNumberOfElementsPerHashBucket - 1) / 
                                                     AverageNumberOfElementsPerHashBucket;
        static_assert(NumInlineHashBuckets > 0 && !(NumInlineHashBuckets & (NumInlineHashBuckets - 1)), 
                     "Number of inline buckets must be a power of two");

    public:
        static OLO_FINLINE u32 GetNumberOfHashBuckets(u32 NumHashedElements)
        {
            // RoundUpToPowerOfTwo
            u32 Value = NumHashedElements / AverageNumberOfElementsPerHashBucket;
            if (Value == 0) Value = 1;
            --Value;
            Value |= Value >> 1;
            Value |= Value >> 2;
            Value |= Value >> 4;
            Value |= Value >> 8;
            Value |= Value >> 16;
            ++Value;

            if (Value < NumInlineHashBuckets) return NumInlineHashBuckets;
            if (NumHashedElements < MinNumberOfHashedElements) return NumInlineHashBuckets;
            return Value;
        }

        using SparseArrayAllocator = TInlineSparseArrayAllocator<NumInlineElements, typename SecondaryAllocator::SparseArrayAllocator>;
        using HashAllocator = TInlineAllocator<NumInlineHashBuckets, typename SecondaryAllocator::HashAllocator>;
    };

    /**
     * @class TFixedSparseSetAllocator
     * @brief Fixed-size sparse set allocator with no secondary storage
     */
    template <
        u32 NumInlineElements,
        u32 AverageNumberOfElementsPerHashBucket = DEFAULT_NUMBER_OF_ELEMENTS_PER_HASH_BUCKET,
        u32 MinNumberOfHashedElements = DEFAULT_MIN_NUMBER_OF_HASHED_ELEMENTS>
    class TFixedSparseSetAllocator
    {
    private:
        static constexpr u32 NumInlineHashBuckets = (NumInlineElements + AverageNumberOfElementsPerHashBucket - 1) / 
                                                     AverageNumberOfElementsPerHashBucket;
        static_assert(NumInlineHashBuckets > 0 && !(NumInlineHashBuckets & (NumInlineHashBuckets - 1)), 
                     "Number of inline buckets must be a power of two");

    public:
        static OLO_FINLINE u32 GetNumberOfHashBuckets(u32 NumHashedElements)
        {
            u32 Value = NumHashedElements / AverageNumberOfElementsPerHashBucket;
            if (Value == 0) Value = 1;
            --Value;
            Value |= Value >> 1;
            Value |= Value >> 2;
            Value |= Value >> 4;
            Value |= Value >> 8;
            Value |= Value >> 16;
            ++Value;

            if (Value < NumInlineHashBuckets) return NumInlineHashBuckets;
            if (NumHashedElements < MinNumberOfHashedElements) return NumInlineHashBuckets;
            return Value;
        }

        using SparseArrayAllocator = TFixedSparseArrayAllocator<NumInlineElements>;
        using HashAllocator = TFixedAllocator<NumInlineHashBuckets>;
    };

    // ============================================================================
    // Default Allocator Typedefs
    // ============================================================================

    /** Default sparse set allocator */
    class FDefaultSparseSetAllocator : public TSparseSetAllocator<>
    {
    public:
        using Typedef = TSparseSetAllocator<>;
    };

    /** Default compact set allocator */
    class FDefaultCompactSetAllocator : public TCompactSetAllocator<>
    {
    public:
        using Typedef = TCompactSetAllocator<>;
    };

    /** Default sparse array allocator */
    class FDefaultSparseArrayAllocator : public TSparseArrayAllocator<>
    {
    public:
        using Typedef = TSparseArrayAllocator<>;
    };

    // ============================================================================
    // TSetAllocator / FDefaultSetAllocator (conditional on OLO_USE_COMPACT_SET_AS_DEFAULT)
    // ============================================================================

    #ifndef OLO_USE_COMPACT_SET_AS_DEFAULT
    #define OLO_USE_COMPACT_SET_AS_DEFAULT 0
    #endif

    #if OLO_USE_COMPACT_SET_AS_DEFAULT
    
    /** Default set allocator uses TCompactSetAllocator */
    class FDefaultSetAllocator : public TCompactSetAllocator<>
    {
    public:
        using Typedef = TCompactSetAllocator<>;
    };

    template <
        typename InSparseArrayAllocator = TSparseArrayAllocator<>,
        typename InHashAllocator = TInlineAllocator<1, FDefaultAllocator>,
        u32... N>
    class TSetAllocator : public TCompactSetAllocator<InHashAllocator>
    {
    public:
        using Typedef = TCompactSetAllocator<InHashAllocator>;
    };

    template <u32 N, typename S = TCompactSetAllocator<>, u32... NN>
    class TInlineSetAllocator : public TInlineCompactSetAllocator<N, S>
    {
    public:
        using Typedef = TInlineCompactSetAllocator<N, S>;
    };

    template <u32 N, u32... Y>
    class TFixedSetAllocator : public TFixedCompactSetAllocator<N>
    {
    public:
        using Typedef = TFixedCompactSetAllocator<N>;
    };

    #else // !OLO_USE_COMPACT_SET_AS_DEFAULT

    /** Default set allocator uses TSparseSetAllocator */
    class FDefaultSetAllocator : public TSparseSetAllocator<>
    {
    public:
        using Typedef = TSparseSetAllocator<>;
    };

    template <
        typename InSparseArrayAllocator = TSparseArrayAllocator<>,
        typename InHashAllocator = TInlineAllocator<1, FDefaultAllocator>,
        u32... N>
    class TSetAllocator : public TSparseSetAllocator<InSparseArrayAllocator, InHashAllocator, N...>
    {
    public:
        using Typedef = TSparseSetAllocator<InSparseArrayAllocator, InHashAllocator, N...>;
    };

    template <
        u32 N,
        typename S = TSparseSetAllocator<TSparseArrayAllocator<FDefaultAllocator, FDefaultAllocator>, FDefaultAllocator>,
        u32... NN>
    class TInlineSetAllocator : public TInlineSparseSetAllocator<N, S, NN...>
    {
    public:
        using Typedef = TInlineSparseSetAllocator<N, S, NN...>;
    };

    template <u32... N>
    class TFixedSetAllocator : public TFixedSparseSetAllocator<N...>
    {
    public:
        using Typedef = TFixedSparseSetAllocator<N...>;
    };

    #endif // OLO_USE_COMPACT_SET_AS_DEFAULT

    // ============================================================================
    // Allocator Trait Specializations for Default Aliases
    // ============================================================================

    template <>
    struct TAllocatorTraits<FDefaultSetAllocator> : TAllocatorTraits<typename FDefaultSetAllocator::Typedef>
    {
    };

    template <>
    struct TAllocatorTraits<FDefaultSparseSetAllocator> : TAllocatorTraits<typename FDefaultSparseSetAllocator::Typedef>
    {
    };

    // NOTE: TAllocatorTraits<FDefaultBitArrayAllocator> is defined earlier in the file
    // (immediately after the class definition) to avoid instantiation order issues

    template <>
    struct TAllocatorTraits<FDefaultSparseArrayAllocator> : TAllocatorTraits<typename FDefaultSparseArrayAllocator::Typedef>
    {
    };

} // namespace OloEngine
