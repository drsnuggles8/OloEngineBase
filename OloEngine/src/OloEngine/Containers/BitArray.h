#pragma once

/**
 * @file BitArray.h
 * @brief Dynamic bit array container for memory-efficient boolean storage
 * 
 * Provides a compact bit array implementation:
 * - FRelativeBitReference: Computes word index and mask from bit index
 * - FBitReference: Mutable reference to a single bit
 * - FConstBitReference: Const reference to a single bit
 * - TBitArray: Dynamic array of bits with pluggable allocator
 * - TConstSetBitIterator: Iterator over set bits only
 * 
 * Used as a foundation for TSparseArray allocation tracking.
 * 
 * Ported from Unreal Engine's Containers/BitArray.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Memory/MemoryOps.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include <cstring>
#include <type_traits>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace OloEngine
{
    // ============================================================================
    // Constants
    // ============================================================================

    // NumBitsPerDWORD and NumBitsPerDWORDLogTwo are defined as macros in ContainerAllocationPolicies.h
    static constexpr u32 FullWordMask = 0xffffffffu;

    // ============================================================================
    // FBitSet Helper
    // ============================================================================

    /**
     * @struct FBitSet
     * @brief Helper struct with static utilities for bit operations
     */
    struct FBitSet
    {
        static constexpr u32 BitsPerWord = NumBitsPerDWORD;

        /** Calculate the number of words needed for a given number of bits */
        [[nodiscard]] static constexpr u32 CalculateNumWords(i32 NumBits)
        {
            OLO_CORE_ASSERT(NumBits >= 0, "NumBits must be non-negative");
            return static_cast<u32>((NumBits + BitsPerWord - 1) / BitsPerWord);
        }

        /** Get and clear the next set bit from a word, returning its index */
        [[nodiscard]] static i32 GetAndClearNextBit(u32& Value)
        {
            const u32 LowestBitMask = static_cast<u32>(static_cast<i32>(Value) & -static_cast<i32>(Value));
            const i32 LowestBitIndex = CountTrailingZeros(LowestBitMask);
            Value ^= LowestBitMask;
            return LowestBitIndex;
        }

    private:
        /** Count trailing zeros in a 32-bit word */
        [[nodiscard]] static i32 CountTrailingZeros(u32 Value)
        {
            if (Value == 0)
            {
                return 32;
            }
#if defined(_MSC_VER)
            unsigned long Index;
            _BitScanForward(&Index, Value);
            return static_cast<i32>(Index);
#elif defined(__GNUC__) || defined(__clang__)
            return __builtin_ctz(Value);
#else
            // Fallback implementation
            i32 Count = 0;
            while ((Value & 1) == 0)
            {
                Value >>= 1;
                ++Count;
            }
            return Count;
#endif
        }
    };

    // ============================================================================
    // Math Utilities (subset needed for bit array)
    // ============================================================================

    namespace BitArrayMath
    {
        /** Count leading zeros in a 32-bit word */
        [[nodiscard]] inline i32 CountLeadingZeros(u32 Value)
        {
            if (Value == 0)
            {
                return 32;
            }
#if defined(_MSC_VER)
            unsigned long Index;
            _BitScanReverse(&Index, Value);
            return 31 - static_cast<i32>(Index);
#elif defined(__GNUC__) || defined(__clang__)
            return __builtin_clz(Value);
#else
            // Fallback implementation
            i32 Count = 0;
            while ((Value & 0x80000000u) == 0)
            {
                Value <<= 1;
                ++Count;
            }
            return Count;
#endif
        }

        /** Count trailing zeros in a 32-bit word */
        [[nodiscard]] inline i32 CountTrailingZeros(u32 Value)
        {
            if (Value == 0)
            {
                return 32;
            }
#if defined(_MSC_VER)
            unsigned long Index;
            _BitScanForward(&Index, Value);
            return static_cast<i32>(Index);
#elif defined(__GNUC__) || defined(__clang__)
            return __builtin_ctz(Value);
#else
            // Fallback implementation
            i32 Count = 0;
            while ((Value & 1) == 0)
            {
                Value >>= 1;
                ++Count;
            }
            return Count;
#endif
        }

        /** Count the number of set bits (population count) in a 32-bit word */
        [[nodiscard]] inline i32 PopCount(u32 Value)
        {
#if defined(_MSC_VER)
            return static_cast<i32>(__popcnt(Value));
#elif defined(__GNUC__) || defined(__clang__)
            return __builtin_popcount(Value);
#else
            // Fallback implementation (Brian Kernighan's algorithm)
            i32 Count = 0;
            while (Value)
            {
                Value &= Value - 1;
                ++Count;
            }
            return Count;
#endif
        }

        /** Divide and round down to integer */
        template <typename T>
        [[nodiscard]] constexpr T DivideAndRoundDown(T Dividend, T Divisor)
        {
            return Dividend / Divisor;
        }

        /** Divide and round up to integer */
        template <typename T>
        [[nodiscard]] constexpr T DivideAndRoundUp(T Dividend, T Divisor)
        {
            return (Dividend + Divisor - 1) / Divisor;
        }

        /** Max of two values */
        template <typename T>
        [[nodiscard]] constexpr T Max(T A, T B)
        {
            return (A >= B) ? A : B;
        }
    }

    // ============================================================================
    // FRelativeBitReference
    // ============================================================================

    /**
     * @class FRelativeBitReference
     * @brief Used to reference a bit in an unspecified bit array.
     *        Encapsulates word index and bit mask computation for a bit index
     */
    class FRelativeBitReference
    {
    public:
        [[nodiscard]] OLO_FINLINE explicit FRelativeBitReference(i32 BitIndex)
            : WordIndex(BitIndex >> NumBitsPerDWORDLogTwo)
            , Mask(1u << (BitIndex & (NumBitsPerDWORD - 1)))
        {
        }

        i32 WordIndex;
        u32 Mask;

        [[nodiscard]] OLO_FINLINE bool operator==(FRelativeBitReference Other) const
        {
            return (WordIndex == Other.WordIndex) & (Mask == Other.Mask);
        }

        [[nodiscard]] OLO_FINLINE bool operator!=(FRelativeBitReference Other) const
        {
            return !(Other == *this);
        }
    };

    // ============================================================================
    // FBitReference
    // ============================================================================

    /**
     * @class FBitReference
     * @brief A mutable reference to a single bit in a bit array word
     */
    class FBitReference
    {
    public:
        [[nodiscard]] OLO_FINLINE FBitReference(u32& InData, u32 InMask)
            : Data(InData)
            , Mask(InMask)
        {
        }

        [[nodiscard]] OLO_FINLINE operator bool() const
        {
            return (Data & Mask) != 0;
        }

        OLO_FINLINE void operator=(bool NewValue)
        {
            if (NewValue)
            {
                Data |= Mask;
            }
            else
            {
                Data &= ~Mask;
            }
        }

        OLO_FINLINE void operator|=(bool NewValue)
        {
            if (NewValue)
            {
                Data |= Mask;
            }
        }

        OLO_FINLINE void operator&=(bool NewValue)
        {
            if (!NewValue)
            {
                Data &= ~Mask;
            }
        }

        OLO_FINLINE FBitReference& operator=(const FBitReference& Copy)
        {
            *this = static_cast<bool>(Copy);
            return *this;
        }

        /**
         * @brief Atomically set the bit value
         * @param NewValue The value to set atomically
         * 
         * Thread-safe bit modification using compare-exchange.
         */
        OLO_FINLINE void AtomicSet(bool NewValue)
        {
            if (NewValue)
            {
                // Atomically set the bit
                u32 Expected = Data;
                while (!(Expected & Mask))
                {
                    u32 Desired = Expected | Mask;
                    // Use platform atomic compare-exchange
#if defined(_MSC_VER)
                    u32 Actual = _InterlockedCompareExchange(
                        reinterpret_cast<volatile long*>(&Data),
                        static_cast<long>(Desired),
                        static_cast<long>(Expected));
                    if (Actual == Expected)
                    {
                        break;
                    }
                    Expected = Actual;
#else
                    if (__sync_bool_compare_and_swap(&Data, Expected, Desired))
                    {
                        break;
                    }
                    Expected = Data;
#endif
                }
            }
            else
            {
                // Atomically clear the bit
                u32 Expected = Data;
                while (Expected & Mask)
                {
                    u32 Desired = Expected & ~Mask;
#if defined(_MSC_VER)
                    u32 Actual = _InterlockedCompareExchange(
                        reinterpret_cast<volatile long*>(&Data),
                        static_cast<long>(Desired),
                        static_cast<long>(Expected));
                    if (Actual == Expected)
                    {
                        break;
                    }
                    Expected = Actual;
#else
                    if (__sync_bool_compare_and_swap(&Data, Expected, Desired))
                    {
                        break;
                    }
                    Expected = Data;
#endif
                }
            }
        }

    private:
        u32& Data;
        u32 Mask;
    };

    // ============================================================================
    // FConstBitReference
    // ============================================================================

    /**
     * @class FConstBitReference
     * @brief A const reference to a single bit in a bit array word
     */
    class FConstBitReference
    {
    public:
        [[nodiscard]] OLO_FINLINE FConstBitReference(const u32& InData, u32 InMask)
            : Data(InData)
            , Mask(InMask)
        {
        }

        [[nodiscard]] OLO_FINLINE operator bool() const
        {
            return (Data & Mask) != 0;
        }

    private:
        const u32& Data;
        u32 Mask;
    };

    // ============================================================================
    // EBitwiseOperatorFlags
    // ============================================================================

    /**
     * @enum EBitwiseOperatorFlags
     * @brief Flag enumeration for controlling bitwise operator functionality
     */
    enum class EBitwiseOperatorFlags : u8
    {
        /** No flags */
        None = 0,
        /** Specifies that the result should be sized Max(A.Num(), B.Num()) */
        MaxSize = 1 << 0,
        /** Specifies that the result should be sized Min(A.Num(), B.Num()) */
        MinSize = 1 << 1,
        /** Only valid for self-mutating bitwise operators - indicates that the size of the LHS operand should not be changed */
        MaintainSize = 1 << 2,
        /** When MaxSize or MaintainSize is specified and the operands are sized differently, any missing bits will be considered as 1, rather than 0 */
        OneFillMissingBits = 1 << 4,
    };

    /** Enable bitwise operators for EBitwiseOperatorFlags */
    [[nodiscard]] OLO_FINLINE constexpr EBitwiseOperatorFlags operator|(EBitwiseOperatorFlags A, EBitwiseOperatorFlags B)
    {
        return static_cast<EBitwiseOperatorFlags>(static_cast<u8>(A) | static_cast<u8>(B));
    }

    [[nodiscard]] OLO_FINLINE constexpr EBitwiseOperatorFlags operator&(EBitwiseOperatorFlags A, EBitwiseOperatorFlags B)
    {
        return static_cast<EBitwiseOperatorFlags>(static_cast<u8>(A) & static_cast<u8>(B));
    }

    [[nodiscard]] OLO_FINLINE constexpr bool EnumHasAnyFlags(EBitwiseOperatorFlags Test, EBitwiseOperatorFlags Flags)
    {
        return (static_cast<u8>(Test) & static_cast<u8>(Flags)) != 0;
    }

    // ============================================================================
    // FBitArrayMemory
    // ============================================================================

    /**
     * @class FBitArrayMemory
     * @brief Memory operations for bit arrays
     * 
     * Provides optimized bit-level memory operations for copying and moving
     * bits within and between bit arrays.
     */
    class FBitArrayMemory
    {
    public:
        /**
         * @brief Copy NumBits bits from the source pointer and offset into the dest pointer and offset.
         * 
         * This function is not suitable for general use because it uses a bit order that is specific 
         * to the u32 internal storage of BitArray.
         * 
         * Bits within each word are read or written in the current platform's mathematical bitorder
         * (Data[0] & 0x1, Data[0] & 0x2, ... Data[0] & 0x80000000, Data[1] & 0x1 ...)
         * 
         * Correctly handles overlap between destination range and source range.
         * 
         * @param DestBits The base location to which the bits are written.
         * @param DestOffset The (word-order) bit within DestBits at which to start writing.
         * @param SourceBits The base location from which the bits are read.
         * @param SourceOffset The (word-order) bit within SourceBits at which to start reading.
         * @param NumBits Number of bits to copy. Must be >= 0.
         */
        static void MemmoveBitsWordOrder(u32* DestBits, i32 DestOffset, const u32* SourceBits, i32 SourceOffset, u32 NumBits)
        {
            if (NumBits == 0)
            {
                return;
            }

            // Normalize offsets
            ModularizeWordOffset(DestBits, DestOffset);
            ModularizeWordOffset(SourceBits, SourceOffset);

            // Check for overlap and direction
            const u32* DestEnd = DestBits + (DestOffset + NumBits + NumBitsPerDWORD - 1) / NumBitsPerDWORD;
            const u32* SourceEnd = SourceBits + (SourceOffset + NumBits + NumBitsPerDWORD - 1) / NumBitsPerDWORD;
            
            bool bOverlap = (DestBits < SourceEnd && SourceBits < DestEnd);
            bool bCopyForward = !bOverlap || (DestBits < SourceBits) || (DestBits == SourceBits && DestOffset <= SourceOffset);

            if (bCopyForward)
            {
                // Copy forward
                MemmoveBitsWordOrderInternal<true>(DestBits, DestOffset, SourceBits, SourceOffset, NumBits);
            }
            else
            {
                // Copy backward
                MemmoveBitsWordOrderInternal<false>(DestBits, DestOffset, SourceBits, SourceOffset, NumBits);
            }
        }

        /** Overload for signed integers */
        static void MemmoveBitsWordOrder(i32* DestBits, i32 DestOffset, const i32* SourceBits, i32 SourceOffset, u32 NumBits)
        {
            MemmoveBitsWordOrder(reinterpret_cast<u32*>(DestBits), DestOffset, 
                                 reinterpret_cast<const u32*>(SourceBits), SourceOffset, NumBits);
        }

        /** 
         * @brief Given Data and Offset that specify a specific bit in a specific word, 
         * modify Data and Offset so that they specify the same bit but that 0 <= Offset < NumBitsPerDWORD.
         */
        static void ModularizeWordOffset(u32*& Data, i32& Offset)
        {
            ModularizeWordOffset(const_cast<const u32*&>(Data), Offset);
        }

        static void ModularizeWordOffset(const u32*& Data, i32& Offset)
        {
            if (Offset >= 0)
            {
                Data += Offset / NumBitsPerDWORD;
                Offset = Offset % NumBitsPerDWORD;
            }
            else
            {
                // Handle negative offsets
                i32 WordOffset = (Offset - (NumBitsPerDWORD - 1)) / NumBitsPerDWORD;
                Data += WordOffset;
                Offset -= WordOffset * NumBitsPerDWORD;
            }
        }

    private:
        template <bool bForward>
        static void MemmoveBitsWordOrderInternal(u32* DestBits, i32 DestOffset, const u32* SourceBits, i32 SourceOffset, u32 NumBits)
        {
            // Simple bit-by-bit copy (could be optimized for word-aligned cases)
            if constexpr (bForward)
            {
                for (u32 i = 0; i < NumBits; ++i)
                {
                    i32 SrcIdx = SourceOffset + static_cast<i32>(i);
                    i32 DstIdx = DestOffset + static_cast<i32>(i);
                    i32 SrcWord = SrcIdx / NumBitsPerDWORD;
                    i32 DstWord = DstIdx / NumBitsPerDWORD;
                    u32 SrcMask = 1u << (SrcIdx % NumBitsPerDWORD);
                    u32 DstMask = 1u << (DstIdx % NumBitsPerDWORD);
                    
                    bool BitValue = (SourceBits[SrcWord] & SrcMask) != 0;
                    if (BitValue)
                    {
                        DestBits[DstWord] |= DstMask;
                    }
                    else
                    {
                        DestBits[DstWord] &= ~DstMask;
                    }
                }
            }
            else
            {
                for (i32 i = static_cast<i32>(NumBits) - 1; i >= 0; --i)
                {
                    i32 SrcIdx = SourceOffset + i;
                    i32 DstIdx = DestOffset + i;
                    i32 SrcWord = SrcIdx / NumBitsPerDWORD;
                    i32 DstWord = DstIdx / NumBitsPerDWORD;
                    u32 SrcMask = 1u << (SrcIdx % NumBitsPerDWORD);
                    u32 DstMask = 1u << (DstIdx % NumBitsPerDWORD);
                    
                    bool BitValue = (SourceBits[SrcWord] & SrcMask) != 0;
                    if (BitValue)
                    {
                        DestBits[DstWord] |= DstMask;
                    }
                    else
                    {
                        DestBits[DstWord] &= ~DstMask;
                    }
                }
            }
        }
    };

    // ============================================================================
    // TBitArray
    // ============================================================================

    /**
     * @class TBitArray
     * @brief Dynamic array of bits with pluggable allocator support
     * 
     * Stores bits compactly in 32-bit words with efficient bit manipulation.
     * Maintains an invariant that unused bits in the last word are always zero.
     * 
     * @tparam Allocator The allocator type for the underlying word storage
     */
    template <typename Allocator = FDefaultAllocator>
    class TBitArray
    {
        using AllocatorType = typename Allocator::template ForElementType<u32>;

    public:
        // ========================================================================
        // Constructors / Destructor
        // ========================================================================

        /** Default constructor - creates an empty bit array */
        [[nodiscard]] TBitArray()
            : NumBits(0)
            , MaxBits(0)
        {
        }

        /** Explicitly consteval constructor for compile-time constant arrays */
        [[nodiscard]] explicit consteval TBitArray(EConstEval)
            : AllocatorInstance(ConstEval)
            , NumBits(0)
            , MaxBits(0)
        {
        }

        /** Constructor with initial value and size */
        [[nodiscard]] explicit TBitArray(bool bValue, i32 InNumBits)
            : NumBits(0)
            , MaxBits(0)
        {
            Init(bValue, InNumBits);
        }

        /** Move constructor */
        [[nodiscard]] TBitArray(TBitArray&& Other)
            : NumBits(0)
            , MaxBits(0)
        {
            Move(*this, Other);
        }

        /** Copy constructor */
        [[nodiscard]] TBitArray(const TBitArray& Other)
            : NumBits(0)
            , MaxBits(0)
        {
            Assign(Other);
        }

        /** Copy constructor from different allocator */
        template <typename OtherAllocator>
        [[nodiscard]] explicit TBitArray(const TBitArray<OtherAllocator>& Other)
            : NumBits(0)
            , MaxBits(0)
        {
            Assign(Other);
        }

        /** Destructor */
        ~TBitArray() = default;

        // ========================================================================
        // Assignment Operators
        // ========================================================================

        /** Move assignment */
        TBitArray& operator=(TBitArray&& Other)
        {
            if (this != &Other)
            {
                Move(*this, Other);
            }
            return *this;
        }

        /** Copy assignment */
        TBitArray& operator=(const TBitArray& Other)
        {
            if (this != &Other)
            {
                Assign(Other);
            }
            return *this;
        }

        /** Copy assignment from different allocator */
        template <typename OtherAllocator>
        TBitArray& operator=(const TBitArray<OtherAllocator>& Other)
        {
            Assign(Other);
            return *this;
        }

        // ========================================================================
        // Comparison Operators
        // ========================================================================

        [[nodiscard]] bool operator==(const TBitArray& Other) const
        {
            if (Num() != Other.Num())
            {
                return false;
            }
            return std::memcmp(GetData(), Other.GetData(), GetNumWords() * sizeof(u32)) == 0;
        }

        [[nodiscard]] bool operator!=(const TBitArray& Other) const
        {
            return !(*this == Other);
        }

        /** Three-way comparison operator (C++20) */
        [[nodiscard]] auto operator<=>(const TBitArray& Other) const
        {
            const i32 MinBits = (NumBits < Other.NumBits) ? NumBits : Other.NumBits;
            const i32 MinWords = FBitSet::CalculateNumWords(MinBits);
            
            for (i32 i = 0; i < MinWords; ++i)
            {
                if (GetData()[i] < Other.GetData()[i])
                {
                    return std::strong_ordering::less;
                }
                if (GetData()[i] > Other.GetData()[i])
                {
                    return std::strong_ordering::greater;
                }
            }
            
            if (NumBits < Other.NumBits)
            {
                return std::strong_ordering::less;
            }
            if (NumBits > Other.NumBits)
            {
                return std::strong_ordering::greater;
            }
            return std::strong_ordering::equal;
        }

        // ========================================================================
        // Size / Capacity Methods
        // ========================================================================

        /** Returns the number of bits in the array */
        [[nodiscard]] OLO_FINLINE i32 Num() const
        {
            return NumBits;
        }

        /** Returns true if the array is empty */
        [[nodiscard]] OLO_FINLINE bool IsEmpty() const
        {
            return NumBits == 0;
        }

        /** Returns the maximum number of bits the array can hold without reallocation */
        [[nodiscard]] OLO_FINLINE i32 Max() const
        {
            return MaxBits;
        }

        /** Checks if an index is valid */
        [[nodiscard]] OLO_FINLINE bool IsValidIndex(i32 Index) const
        {
            return Index >= 0 && Index < NumBits;
        }

        /**
         * @brief Verify internal invariants are valid
         * 
         * Checks that the bit array's internal state is consistent:
         * - NumBits <= MaxBits
         * - NumBits >= 0 and MaxBits >= 0
         * - Slack bits in the last word are cleared
         */
        void CheckInvariants() const
        {
#if !OLO_BUILD_SHIPPING
            OLO_CORE_ASSERT(NumBits <= MaxBits, 
                "TBitArray::NumBits ({}) should never be greater than MaxBits ({})", NumBits, MaxBits);
            OLO_CORE_ASSERT(NumBits >= 0 && MaxBits >= 0,
                "NumBits ({}) and MaxBits ({}) should always be >= 0", NumBits, MaxBits);

            // Verify the ClearPartialSlackBits invariant
            const i32 UsedBits = (NumBits % NumBitsPerDWORD);
            if (UsedBits != 0)
            {
                const i32 LastWordIndex = NumBits / NumBitsPerDWORD;
                const u32 SlackMask = FullWordMask << UsedBits;
                const u32 LastWord = *(GetData() + LastWordIndex);
                OLO_CORE_ASSERT((LastWord & SlackMask) == 0,
                    "TBitArray slack bits are non-zero, this will result in undefined behavior.");
            }
#endif
        }

        // ========================================================================
        // Element Access
        // ========================================================================

        /** Access a bit by index (mutable) */
        [[nodiscard]] OLO_FINLINE FBitReference operator[](i32 Index)
        {
            OLO_CORE_ASSERT(Index >= 0 && Index < NumBits, "Bit index out of bounds");
            return FBitReference(
                GetData()[Index / NumBitsPerDWORD],
                1u << (Index & (NumBitsPerDWORD - 1))
            );
        }

        /** Access a bit by index (const) */
        [[nodiscard]] OLO_FINLINE FConstBitReference operator[](i32 Index) const
        {
            OLO_CORE_ASSERT(Index >= 0 && Index < NumBits, "Bit index out of bounds");
            return FConstBitReference(
                GetData()[Index / NumBitsPerDWORD],
                1u << (Index & (NumBitsPerDWORD - 1))
            );
        }

        /** Access the bit at an index directly (no bounds check) */
        [[nodiscard]] OLO_FINLINE bool AccessCorrespondingBit(const FRelativeBitReference& Reference) const
        {
            return (GetData()[Reference.WordIndex] & Reference.Mask) != 0;
        }

        // ========================================================================
        // Data Access
        // ========================================================================

        /** Get raw pointer to the word data */
        [[nodiscard]] OLO_FINLINE u32* GetData()
        {
            return reinterpret_cast<u32*>(AllocatorInstance.GetAllocation());
        }

        /** Get const raw pointer to the word data */
        [[nodiscard]] OLO_FINLINE const u32* GetData() const
        {
            return reinterpret_cast<const u32*>(AllocatorInstance.GetAllocation());
        }

        // ========================================================================
        // Initialization / Reset
        // ========================================================================

        /**
         * @brief Initialize the bit array with a value
         * @param bValue  The value to initialize all bits to
         * @param InNumBits  The number of bits to initialize
         */
        OLO_FINLINE void Init(bool bValue, i32 InNumBits)
        {
            NumBits = InNumBits;

            const u32 NumWords = GetNumWords();
            const u32 MaxWords = GetMaxWords();

            if (NumWords > 0)
            {
                if (NumWords > MaxWords)
                {
                    AllocatorInstance.ResizeAllocation(0, static_cast<i32>(NumWords), sizeof(u32));
                    MaxBits = static_cast<i32>(NumWords) * NumBitsPerDWORD;
                }

                SetWords(GetData(), static_cast<i32>(NumWords), bValue);
                ClearPartialSlackBits();
            }
        }

        /**
         * @brief Remove all bits, potentially preserving some capacity
         * @param ExpectedNumBits  Expected number of bits to be added
         */
        void Empty(i32 ExpectedNumBits = 0)
        {
            ExpectedNumBits = static_cast<i32>(FBitSet::CalculateNumWords(ExpectedNumBits)) * NumBitsPerDWORD;
            const i32 InitialMaxBits = AllocatorInstance.GetInitialCapacity() * NumBitsPerDWORD;

            NumBits = 0;

            // If we need more bits or can shrink our allocation, do so
            if (ExpectedNumBits > MaxBits || MaxBits > InitialMaxBits)
            {
                MaxBits = BitArrayMath::Max(ExpectedNumBits, InitialMaxBits);
                Realloc(0);
            }
        }

        /** Remove all bits but keep allocated memory as slack */
        void Reset()
        {
            NumBits = 0;
        }

        /**
         * @brief Reserve memory for at least the specified number of bits
         * @param Number  The number of bits to reserve space for
         */
        void Reserve(i32 Number)
        {
            if (Number > MaxBits)
            {
                const u32 MaxWords = AllocatorInstance.CalculateSlackGrow(
                    static_cast<i32>(FBitSet::CalculateNumWords(Number)),
                    GetMaxWords(),
                    sizeof(u32)
                );
                MaxBits = static_cast<i32>(MaxWords) * NumBitsPerDWORD;
                Realloc(NumBits);
            }
        }

        // ========================================================================
        // Add / Insert / Remove
        // ========================================================================

        /**
         * @brief Add a bit to the array
         * @param Value  The value of the bit to add
         * @return The index of the added bit
         */
        i32 Add(const bool Value)
        {
            const i32 Index = AddUninitialized(1);
            SetBitNoCheck(Index, Value);
            return Index;
        }

        /**
         * @brief Add multiple bits with the same value
         * @param Value  The value to set the bits to
         * @param NumBitsToAdd  The number of bits to add
         * @return The index of the first added bit
         */
        i32 Add(const bool Value, i32 NumBitsToAdd)
        {
            if (NumBitsToAdd < 0)
            {
                return NumBits;
            }
            const i32 Index = AddUninitialized(NumBitsToAdd);
            SetRange(Index, NumBitsToAdd, Value);
            return Index;
        }

        /**
         * @brief Add space for bits without initializing them
         * @param NumBitsToAdd  Number of bits to add
         * @return Index of the first added bit
         */
        i32 AddUninitialized(i32 NumBitsToAdd)
        {
            OLO_CORE_ASSERT(NumBitsToAdd >= 0, "NumBitsToAdd must be non-negative");
            i32 AddedIndex = NumBits;
            if (NumBitsToAdd > 0)
            {
                i32 OldLastWordIndex = NumBits == 0 ? -1 : (NumBits - 1) / NumBitsPerDWORD;
                i32 NewLastWordIndex = (NumBits + NumBitsToAdd - 1) / NumBitsPerDWORD;
                if (NewLastWordIndex == OldLastWordIndex)
                {
                    // Not extending into a new word
                    NumBits += NumBitsToAdd;
                }
                else
                {
                    Reserve(NumBits + NumBitsToAdd);
                    NumBits += NumBitsToAdd;
                    ClearPartialSlackBits();
                }
            }
            return AddedIndex;
        }

        /**
         * @brief Insert a bit at the specified index
         * @param Value  The value of the bit to insert
         * @param Index  The index at which to insert
         */
        void Insert(bool Value, i32 Index)
        {
            InsertUninitialized(Index, 1);
            SetBitNoCheck(Index, Value);
        }

        /**
         * @brief Insert multiple bits with the same value
         * @param Value  The value to set the bits to
         * @param Index  The index at which to insert
         * @param NumBitsToAdd  The number of bits to insert
         */
        void Insert(bool Value, i32 Index, i32 NumBitsToAdd)
        {
            InsertUninitialized(Index, NumBitsToAdd);
            SetRange(Index, NumBitsToAdd, Value);
        }

        /**
         * @brief Insert space for bits without initializing them
         * @param Index  The index at which to insert
         * @param NumBitsToAdd  Number of bits to insert
         */
        void InsertUninitialized(i32 Index, i32 NumBitsToAdd)
        {
            OLO_CORE_ASSERT(Index >= 0 && Index <= NumBits, "Insert index out of bounds");
            OLO_CORE_ASSERT(NumBitsToAdd >= 0, "NumBitsToAdd must be non-negative");

            if (NumBitsToAdd > 0)
            {
                u32 OldNumBits = NumBits;
                AddUninitialized(NumBitsToAdd);
                u32 NumToShift = OldNumBits - Index;
                if (NumToShift > 0)
                {
                    // Shift bits from end to beginning to handle overlap
                    for (i32 i = static_cast<i32>(OldNumBits) - 1; i >= Index; --i)
                    {
                        (*this)[i + NumBitsToAdd] = static_cast<bool>((*this)[i]);
                    }
                }
            }
        }

        /**
         * @brief Remove bits from the array
         * @param BaseIndex  Index of the first bit to remove
         * @param NumBitsToRemove  Number of bits to remove
         */
        void RemoveAt(i32 BaseIndex, i32 NumBitsToRemove = 1)
        {
            OLO_CORE_ASSERT(BaseIndex >= 0 && NumBitsToRemove >= 0 && BaseIndex + NumBitsToRemove <= NumBits, 
                           "RemoveAt: invalid index/count");

            if (BaseIndex + NumBitsToRemove != NumBits)
            {
                // Shift bits
                u32 NumToShift = NumBits - (BaseIndex + NumBitsToRemove);
                for (u32 i = 0; i < NumToShift; ++i)
                {
                    (*this)[BaseIndex + i] = static_cast<bool>((*this)[BaseIndex + NumBitsToRemove + i]);
                }
            }

            NumBits -= NumBitsToRemove;
            ClearPartialSlackBits();
        }

        /**
         * @brief Remove bits by swapping with bits at the end
         * @param BaseIndex  Index of the first bit to remove
         * @param NumBitsToRemove  Number of bits to remove
         */
        void RemoveAtSwap(i32 BaseIndex, i32 NumBitsToRemove = 1)
        {
            OLO_CORE_ASSERT(BaseIndex >= 0 && NumBitsToRemove >= 0 && BaseIndex + NumBitsToRemove <= NumBits,
                           "RemoveAtSwap: invalid index/count");

            if (BaseIndex < NumBits - NumBitsToRemove)
            {
                // Copy bits from the end to the region we are removing
                for (i32 Index = 0; Index < NumBitsToRemove; ++Index)
                {
                    (*this)[BaseIndex + Index] = static_cast<bool>((*this)[NumBits - NumBitsToRemove + Index]);
                }
            }

            NumBits -= NumBitsToRemove;
            ClearPartialSlackBits();
        }

        // ========================================================================
        // Set Range
        // ========================================================================

        /**
         * @brief Set a range of bits to a value
         * @param Index  The first bit index to set
         * @param NumBitsToSet  The number of bits to set
         * @param Value  The value to set them to
         */
        void SetRange(i32 Index, i32 NumBitsToSet, bool Value)
        {
            OLO_CORE_ASSERT(Index >= 0 && NumBitsToSet >= 0 && Index + NumBitsToSet <= NumBits,
                           "SetRange: invalid index/count");

            if (NumBitsToSet == 0)
            {
                return;
            }

            // Work out which word index to set from, and how many
            u32 StartIndex = Index / NumBitsPerDWORD;
            u32 Count = (Index + NumBitsToSet + (NumBitsPerDWORD - 1)) / NumBitsPerDWORD - StartIndex;

            // Work out masks for the start/end of the sequence
            u32 StartMask = FullWordMask << (Index % NumBitsPerDWORD);
            u32 EndMask = FullWordMask >> (NumBitsPerDWORD - (Index + NumBitsToSet) % NumBitsPerDWORD) % NumBitsPerDWORD;

            u32* Data = GetData() + StartIndex;
            if (Value)
            {
                if (Count == 1)
                {
                    *Data |= StartMask & EndMask;
                }
                else
                {
                    *Data++ |= StartMask;
                    Count -= 2;
                    while (Count != 0)
                    {
                        *Data++ = ~0u;
                        --Count;
                    }
                    *Data |= EndMask;
                }
            }
            else
            {
                if (Count == 1)
                {
                    *Data &= ~(StartMask & EndMask);
                }
                else
                {
                    *Data++ &= ~StartMask;
                    Count -= 2;
                    while (Count != 0)
                    {
                        *Data++ = 0;
                        --Count;
                    }
                    *Data &= ~EndMask;
                }
            }
        }

        /** Set number of bits without initializing new bits */
        void SetNumUninitialized(i32 InNumBits)
        {
            i32 PreviousNumBits = NumBits;
            NumBits = InNumBits;

            if (InNumBits > MaxBits)
            {
                const i32 PreviousNumWords = FBitSet::CalculateNumWords(PreviousNumBits);
                const u32 MaxWords = AllocatorInstance.CalculateSlackReserve(
                    static_cast<i32>(FBitSet::CalculateNumWords(InNumBits)), sizeof(u32));

                AllocatorInstance.ResizeAllocation(PreviousNumWords, MaxWords, sizeof(u32));

                MaxBits = static_cast<i32>(MaxWords) * NumBitsPerDWORD;
            }

            ClearPartialSlackBits();
        }

        /** Set the number of bits, initializing any added bits to the given value */
        template <typename ValueType>
        void SetNum(i32 InNumBits, ValueType bValue)
        {
            static_assert(std::is_same_v<ValueType, bool>, "TBitArray::SetNum: unexpected type passed as the bValue argument (expected bool)");
            i32 PreviousNumBits = NumBits;
            SetNumUninitialized(InNumBits);
            if (InNumBits > PreviousNumBits)
            {
                SetRange(PreviousNumBits, InNumBits - PreviousNumBits, bValue);
            }
        }

        // ========================================================================
        // Search
        // ========================================================================

        /**
         * @brief Find the first occurrence of a value
         * @param bValue  The value to search for
         * @return Index of first occurrence, or INDEX_NONE if not found
         */
        [[nodiscard]] i32 Find(bool bValue) const
        {
            return FindFromImpl(bValue, 0, NumBits);
        }

        /**
         * @brief Find from a starting index
         * @param bValue  The value to search for
         * @param StartIndex  Where to start searching
         * @return Index of first occurrence from StartIndex, or INDEX_NONE if not found
         */
        template <typename IndexType>
        [[nodiscard]] OLO_FINLINE i32 FindFrom(bool bValue, IndexType StartIndex) const
        {
            static_assert(!std::is_same_v<IndexType, bool>, "TBitArray::FindFrom: unexpected bool passed as StartIndex");
            OLO_CORE_ASSERT(StartIndex >= 0 && StartIndex <= NumBits, "StartIndex out of bounds");
            return FindFromImpl(bValue, static_cast<i32>(StartIndex), NumBits);
        }

        /**
         * @brief Find the last occurrence of a value
         * @param bValue  The value to search for
         * @return Index of last occurrence, or INDEX_NONE if not found
         */
        [[nodiscard]] i32 FindLast(bool bValue) const
        {
            return FindLastFromImpl(bValue, NumBits);
        }

        /**
         * @brief Check if the array contains a value
         * @param bValue  The value to search for
         * @return True if the value exists
         */
        [[nodiscard]] OLO_FINLINE bool Contains(bool bValue) const
        {
            return Find(bValue) != INDEX_NONE;
        }

        /**
         * @brief Find and set the first zero bit to one
         * @param StartIndex  Where to start searching
         * @return Index of the bit that was set, or INDEX_NONE if all bits are one
         */
        i32 FindAndSetFirstZeroBit(i32 StartIndex = 0)
        {
            const i32 FirstZeroBitIndex = FindFromImpl(false, StartIndex, NumBits);
            if (FirstZeroBitIndex != INDEX_NONE)
            {
                (*this)[FirstZeroBitIndex] = true;
            }
            return FirstZeroBitIndex;
        }

        /**
         * @brief Find and set the last zero bit to one
         * @return Index of the bit that was set, or INDEX_NONE if all bits are one
         */
        i32 FindAndSetLastZeroBit()
        {
            const i32 LastZeroBitIndex = FindLast(false);
            if (LastZeroBitIndex != INDEX_NONE)
            {
                (*this)[LastZeroBitIndex] = true;
            }
            return LastZeroBitIndex;
        }

        /**
         * @brief Count the number of set bits in the array
         * @param FromIndex  Starting index (inclusive)
         * @param ToIndex    Ending index (exclusive), or INDEX_NONE for end
         * @return Number of set bits in the range
         */
        [[nodiscard]] i32 CountSetBits(i32 FromIndex = 0, i32 ToIndex = INDEX_NONE) const
        {
            if (ToIndex == INDEX_NONE)
            {
                ToIndex = NumBits;
            }

            OLO_CORE_ASSERT(FromIndex >= 0 && FromIndex <= NumBits, "FromIndex out of bounds");
            OLO_CORE_ASSERT(ToIndex >= FromIndex && ToIndex <= NumBits, "ToIndex out of bounds");

            if (FromIndex == ToIndex)
            {
                return 0;
            }

            const u32* Data = GetData();
            i32 NumSetBits = 0;

            // Calculate word indices
            const i32 StartWord = FromIndex / NumBitsPerDWORD;
            const i32 EndWord = (ToIndex - 1) / NumBitsPerDWORD;
            
            // Handle the case where all bits are in one word
            if (StartWord == EndWord)
            {
                u32 Mask = FullWordMask;
                Mask &= FullWordMask << (FromIndex % NumBitsPerDWORD);
                Mask &= FullWordMask >> ((NumBitsPerDWORD - (ToIndex % NumBitsPerDWORD)) % NumBitsPerDWORD);
                NumSetBits = BitArrayMath::PopCount(Data[StartWord] & Mask);
            }
            else
            {
                // First partial word
                u32 StartMask = FullWordMask << (FromIndex % NumBitsPerDWORD);
                NumSetBits += BitArrayMath::PopCount(Data[StartWord] & StartMask);

                // Full words in the middle
                for (i32 WordIdx = StartWord + 1; WordIdx < EndWord; ++WordIdx)
                {
                    NumSetBits += BitArrayMath::PopCount(Data[WordIdx]);
                }

                // Last partial word
                u32 EndMask = FullWordMask >> ((NumBitsPerDWORD - (ToIndex % NumBitsPerDWORD)) % NumBitsPerDWORD);
                NumSetBits += BitArrayMath::PopCount(Data[EndWord] & EndMask);
            }

            return NumSetBits;
        }

        // ========================================================================
        // Bitwise Operations
        // ========================================================================

        /**
         * @brief Combine this bit array with another using bitwise AND
         * @param Other  The other bit array
         * 
         * Result size is min of both arrays. Missing bits treated as 0.
         */
        void CombineWithBitwiseAND(const TBitArray& Other)
        {
            if (Num() == 0)
            {
                return;
            }

            u32* Data = GetData();
            const u32* OtherData = Other.GetData();
            const i32 MinWords = static_cast<i32>((Num() < Other.Num() ? FBitSet::CalculateNumWords(Num()) : FBitSet::CalculateNumWords(Other.Num())));
            const i32 ThisWords = static_cast<i32>(GetNumWords());

            for (i32 i = 0; i < MinWords; ++i)
            {
                Data[i] &= OtherData[i];
            }

            // AND with 0 for any bits beyond Other's size
            for (i32 i = MinWords; i < ThisWords; ++i)
            {
                Data[i] = 0;
            }
        }

        /**
         * @brief Combine this bit array with another using bitwise OR
         * @param Other  The other bit array
         * 
         * Result size is max of both arrays.
         */
        void CombineWithBitwiseOR(const TBitArray& Other)
        {
            if (Other.Num() == 0)
            {
                return;
            }

            if (NumBits < Other.Num())
            {
                Add(false, Other.Num() - NumBits);
            }

            u32* Data = GetData();
            const u32* OtherData = Other.GetData();
            const i32 OtherWords = static_cast<i32>(FBitSet::CalculateNumWords(Other.Num()));

            for (i32 i = 0; i < OtherWords; ++i)
            {
                Data[i] |= OtherData[i];
            }
        }

        /**
         * @brief Combine this bit array with another using bitwise XOR
         * @param Other  The other bit array
         * 
         * Result size is max of both arrays. Missing bits treated as 0.
         */
        void CombineWithBitwiseXOR(const TBitArray& Other)
        {
            if (Other.Num() == 0)
            {
                return;
            }

            if (NumBits < Other.Num())
            {
                Add(false, Other.Num() - NumBits);
            }

            u32* Data = GetData();
            const u32* OtherData = Other.GetData();
            const i32 OtherWords = static_cast<i32>(FBitSet::CalculateNumWords(Other.Num()));

            for (i32 i = 0; i < OtherWords; ++i)
            {
                Data[i] ^= OtherData[i];
            }
        }

        /**
         * @brief Create a new bit array from bitwise AND of two arrays
         * @param A  First bit array
         * @param B  Second bit array
         * @return New bit array containing A AND B
         */
        [[nodiscard]] static TBitArray BitwiseAND(const TBitArray& A, const TBitArray& B)
        {
            const i32 MinNum = (A.Num() < B.Num()) ? A.Num() : B.Num();
            if (MinNum == 0)
            {
                return TBitArray();
            }

            TBitArray Result(false, MinNum);
            u32* ResultData = Result.GetData();
            const u32* DataA = A.GetData();
            const u32* DataB = B.GetData();
            const i32 NumWords = static_cast<i32>(FBitSet::CalculateNumWords(MinNum));

            for (i32 i = 0; i < NumWords; ++i)
            {
                ResultData[i] = DataA[i] & DataB[i];
            }

            Result.ClearPartialSlackBits();
            return Result;
        }

        /**
         * @brief Create a new bit array from bitwise OR of two arrays
         * @param A  First bit array
         * @param B  Second bit array
         * @return New bit array containing A OR B
         */
        [[nodiscard]] static TBitArray BitwiseOR(const TBitArray& A, const TBitArray& B)
        {
            const i32 MaxNum = (A.Num() > B.Num()) ? A.Num() : B.Num();
            if (MaxNum == 0)
            {
                return TBitArray();
            }

            TBitArray Result(false, MaxNum);
            u32* ResultData = Result.GetData();
            const u32* DataA = A.GetData();
            const u32* DataB = B.GetData();
            const i32 WordsA = static_cast<i32>(FBitSet::CalculateNumWords(A.Num()));
            const i32 WordsB = static_cast<i32>(FBitSet::CalculateNumWords(B.Num()));
            const i32 MinWords = (WordsA < WordsB) ? WordsA : WordsB;
            const i32 MaxWords = (WordsA > WordsB) ? WordsA : WordsB;

            for (i32 i = 0; i < MinWords; ++i)
            {
                ResultData[i] = DataA[i] | DataB[i];
            }

            // Copy remaining bits from the larger array
            const u32* LargerData = (WordsA > WordsB) ? DataA : DataB;
            for (i32 i = MinWords; i < MaxWords; ++i)
            {
                ResultData[i] = LargerData[i];
            }

            Result.ClearPartialSlackBits();
            return Result;
        }

        /**
         * @brief Create a new bit array from bitwise XOR of two arrays
         * @param A  First bit array
         * @param B  Second bit array
         * @return New bit array containing A XOR B
         */
        [[nodiscard]] static TBitArray BitwiseXOR(const TBitArray& A, const TBitArray& B)
        {
            const i32 MaxNum = (A.Num() > B.Num()) ? A.Num() : B.Num();
            if (MaxNum == 0)
            {
                return TBitArray();
            }

            TBitArray Result(false, MaxNum);
            u32* ResultData = Result.GetData();
            const u32* DataA = A.GetData();
            const u32* DataB = B.GetData();
            const i32 WordsA = static_cast<i32>(FBitSet::CalculateNumWords(A.Num()));
            const i32 WordsB = static_cast<i32>(FBitSet::CalculateNumWords(B.Num()));
            const i32 MinWords = (WordsA < WordsB) ? WordsA : WordsB;
            const i32 MaxWords = (WordsA > WordsB) ? WordsA : WordsB;

            for (i32 i = 0; i < MinWords; ++i)
            {
                ResultData[i] = DataA[i] ^ DataB[i];
            }

            // Copy remaining bits from the larger array (XOR with 0)
            const u32* LargerData = (WordsA > WordsB) ? DataA : DataB;
            for (i32 i = MinWords; i < MaxWords; ++i)
            {
                ResultData[i] = LargerData[i];
            }

            Result.ClearPartialSlackBits();
            return Result;
        }

        /**
         * @brief Create a new bit array with all bits inverted (NOT)
         * @return New bit array with all bits flipped
         */
        [[nodiscard]] TBitArray BitwiseNOT() const
        {
            if (NumBits == 0)
            {
                return TBitArray();
            }

            TBitArray Result(false, NumBits);
            u32* ResultData = Result.GetData();
            const u32* Data = GetData();
            const i32 NumWords = static_cast<i32>(GetNumWords());

            for (i32 i = 0; i < NumWords; ++i)
            {
                ResultData[i] = ~Data[i];
            }

            Result.ClearPartialSlackBits();
            return Result;
        }

        // ========================================================================
        // Memory
        // ========================================================================

        /** Returns the amount of memory allocated by this container */
        [[nodiscard]] u32 GetAllocatedSize() const
        {
            return FBitSet::CalculateNumWords(MaxBits) * sizeof(u32);
        }

        // ========================================================================
        // Word Iterators
        // ========================================================================

    private:
        /**
         * @brief Base class for word-level iteration over bit arrays
         * 
         * Iterates over the underlying u32 words, applying proper masking
         * for partial words at the start and end of the iteration range.
         */
        template <typename WordType>
        struct TWordIteratorBase
        {
            [[nodiscard]] explicit operator bool() const
            {
                return CurrentIndex < NumWords;
            }

            [[nodiscard]] i32 GetIndex() const
            {
                return CurrentIndex;
            }

            [[nodiscard]] u32 GetWord() const
            {
                OLO_CORE_ASSERT(CurrentIndex < NumWords, "Word iterator out of bounds");

                if (CurrentMask == ~0u)
                {
                    return Data[CurrentIndex];
                }
                else if (MissingBitsFill == 0)
                {
                    return Data[CurrentIndex] & CurrentMask;
                }
                else
                {
                    return (Data[CurrentIndex] & CurrentMask) | (MissingBitsFill & ~CurrentMask);
                }
            }

            void operator++()
            {
                ++CurrentIndex;
                if (CurrentIndex == NumWords - 1)
                {
                    CurrentMask = FinalMask;
                }
                else
                {
                    CurrentMask = ~0u;
                }
            }

            void FillMissingBits(u32 InMissingBitsFill)
            {
                MissingBitsFill = InMissingBitsFill;
            }

        protected:
            [[nodiscard]] explicit TWordIteratorBase(WordType* InData, i32 InStartBitIndex, i32 InEndBitIndex)
                : Data(InData)
                , CurrentIndex(InStartBitIndex / NumBitsPerDWORD)
                , NumWords(BitArrayMath::DivideAndRoundUp(InEndBitIndex, NumBitsPerDWORD))
                , CurrentMask(~0u << (InStartBitIndex % NumBitsPerDWORD))
                , FinalMask(~0u)
                , MissingBitsFill(0)
            {
                const i32 Shift = NumBitsPerDWORD - (InEndBitIndex % NumBitsPerDWORD);
                if (Shift < NumBitsPerDWORD)
                {
                    FinalMask = ~0u >> Shift;
                }

                if (CurrentIndex == NumWords - 1)
                {
                    CurrentMask &= FinalMask;
                    FinalMask = CurrentMask;
                }
            }

            WordType* OLO_RESTRICT Data;

            i32 CurrentIndex;
            i32 NumWords;

            u32 CurrentMask;
            u32 FinalMask;
            u32 MissingBitsFill;
        };

    public:
        /**
         * @brief Const iterator over the underlying u32 words
         */
        struct FConstWordIterator : TWordIteratorBase<const u32>
        {
            [[nodiscard]] explicit FConstWordIterator(const TBitArray& InArray)
                : TWordIteratorBase<const u32>(InArray.GetData(), 0, InArray.Num())
            {
            }

            [[nodiscard]] explicit FConstWordIterator(const TBitArray& InArray, i32 InStartBitIndex, i32 InEndBitIndex)
                : TWordIteratorBase<const u32>(InArray.GetData(), InStartBitIndex, InEndBitIndex)
            {
                OLO_CORE_ASSERT(InStartBitIndex <= InEndBitIndex && InStartBitIndex <= InArray.Num() && InEndBitIndex <= InArray.Num(),
                    "Invalid bit range for FConstWordIterator");
                OLO_CORE_ASSERT(InStartBitIndex >= 0 && InEndBitIndex >= 0,
                    "Bit indices must be non-negative");
            }
        };

        /**
         * @brief Mutable iterator over the underlying u32 words
         */
        struct FWordIterator : TWordIteratorBase<u32>
        {
            [[nodiscard]] explicit FWordIterator(TBitArray& InArray)
                : TWordIteratorBase<u32>(InArray.GetData(), 0, InArray.Num())
            {
            }

            void SetWord(u32 InWord)
            {
                OLO_CORE_ASSERT(this->CurrentIndex < this->NumWords, "Word iterator out of bounds");

                if (this->CurrentIndex == this->NumWords - 1)
                {
                    this->Data[this->CurrentIndex] = InWord & this->FinalMask;
                }
                else
                {
                    this->Data[this->CurrentIndex] = InWord;
                }
            }
        };

        // ========================================================================
        // AddRange
        // ========================================================================

        /**
         * @brief Append bits from another bit array
         * @param Source The bit array to copy from
         * @param SourceStartBit Starting bit index in the source (default 0)
         * @param NumBitsToAdd Number of bits to add (default: all remaining bits from source)
         * @return Index of the first added bit
         */
        i32 AddRange(const TBitArray& Source, i32 SourceStartBit = 0, i32 NumBitsToAdd = INDEX_NONE)
        {
            if (NumBitsToAdd == INDEX_NONE)
            {
                NumBitsToAdd = Source.Num() - SourceStartBit;
            }

            OLO_CORE_ASSERT(SourceStartBit >= 0 && SourceStartBit <= Source.Num(), 
                "SourceStartBit out of bounds");
            OLO_CORE_ASSERT(NumBitsToAdd >= 0 && SourceStartBit + NumBitsToAdd <= Source.Num(), 
                "NumBitsToAdd out of bounds");

            if (NumBitsToAdd == 0)
            {
                return NumBits;
            }

            const i32 DestStartBit = NumBits;
            AddUninitialized(NumBitsToAdd);

            // Use FBitArrayMemory to copy bits
            FBitArrayMemory::MemmoveBitsWordOrder(
                GetData(), DestStartBit,
                Source.GetData(), SourceStartBit,
                static_cast<u32>(NumBitsToAdd)
            );

            ClearPartialSlackBits();
            return DestStartBit;
        }

    private:
        // ========================================================================
        // Internal Helper Methods
        // ========================================================================

        [[nodiscard]] OLO_FINLINE u32 GetNumWords() const
        {
            return FBitSet::CalculateNumWords(NumBits);
        }

        [[nodiscard]] OLO_FINLINE u32 GetMaxWords() const
        {
            return FBitSet::CalculateNumWords(MaxBits);
        }

        [[nodiscard]] OLO_FINLINE u32 GetLastWordMask(i32 EndIndexExclusive) const
        {
            const u32 UnusedBits = (FBitSet::BitsPerWord - static_cast<u32>(EndIndexExclusive) % FBitSet::BitsPerWord) % FBitSet::BitsPerWord;
            return ~0u >> UnusedBits;
        }

        OLO_FINLINE static void SetWords(u32* Words, i32 NumWords, bool bValue)
        {
            if (NumWords > 8)
            {
                std::memset(Words, bValue ? 0xff : 0, static_cast<sizet>(NumWords) * sizeof(u32));
            }
            else
            {
                u32 Word = bValue ? ~0u : 0u;
                for (i32 Idx = 0; Idx < NumWords; ++Idx)
                {
                    Words[Idx] = Word;
                }
            }
        }

        template <typename BitArrayType>
        static OLO_FINLINE void Move(BitArrayType& ToArray, BitArrayType& FromArray)
        {
            ToArray.AllocatorInstance.MoveToEmpty(FromArray.AllocatorInstance);

            ToArray.NumBits = FromArray.NumBits;
            ToArray.MaxBits = FromArray.MaxBits;
            FromArray.NumBits = 0;
            FromArray.MaxBits = 0;
        }

        template <typename OtherAllocator>
        void Assign(const TBitArray<OtherAllocator>& Other)
        {
            Empty(Other.Num());
            NumBits = Other.Num();
            if (NumBits)
            {
                std::memcpy(GetData(), Other.GetData(), GetNumWords() * sizeof(u32));
            }
        }

        void Realloc(i32 PreviousNumBits)
        {
            const u32 PreviousNumWords = FBitSet::CalculateNumWords(PreviousNumBits);
            const u32 MaxWords = FBitSet::CalculateNumWords(MaxBits);

            AllocatorInstance.ResizeAllocation(static_cast<i32>(PreviousNumWords), static_cast<i32>(MaxWords), sizeof(u32));
            ClearPartialSlackBits();
        }

        void SetBitNoCheck(i32 Index, bool Value)
        {
            u32& Word = GetData()[Index / NumBitsPerDWORD];
            u32 BitOffset = (Index % NumBitsPerDWORD);
            Word = (Word & ~(1u << BitOffset)) | ((static_cast<u32>(Value)) << BitOffset);
        }

        /** Clears the slack bits within the final partially relevant word */
        void ClearPartialSlackBits()
        {
            const i32 UsedBits = NumBits % NumBitsPerDWORD;
            if (UsedBits != 0)
            {
                const i32 LastWordIndex = NumBits / NumBitsPerDWORD;
                const u32 SlackMask = FullWordMask >> (NumBitsPerDWORD - UsedBits);

                u32* LastWord = (GetData() + LastWordIndex);
                *LastWord = *LastWord & SlackMask;
            }
        }

        i32 FindFromImpl(bool bValue, i32 StartIndex, i32 EndIndexExclusive) const
        {
            // Produce a mask for the first iteration
            u32 Mask = ~0u << (StartIndex % static_cast<i32>(FBitSet::BitsPerWord));

            // Iterate over the array until we see a word with a matching bit
            const u32 Test = bValue ? 0u : ~0u;

            const u32* DwordArray = GetData();
            const i32 DwordCount = static_cast<i32>(FBitSet::CalculateNumWords(EndIndexExclusive));
            i32 DwordIndex = BitArrayMath::DivideAndRoundDown(StartIndex, NumBitsPerDWORD);
            while (DwordIndex < DwordCount && (DwordArray[DwordIndex] & Mask) == (Test & Mask))
            {
                ++DwordIndex;
                Mask = ~0u;
            }

            if (DwordIndex < DwordCount)
            {
                // If we're looking for a false, then we flip the bits - then we only need to find the first one bit
                const u32 Bits = (bValue ? DwordArray[DwordIndex] : ~DwordArray[DwordIndex]) & Mask;
                OLO_CORE_ASSERT(Bits != 0, "Expected non-zero bits");
                const i32 LowestBitIndex = BitArrayMath::CountTrailingZeros(Bits) + (DwordIndex << NumBitsPerDWORDLogTwo);
                if (LowestBitIndex < EndIndexExclusive)
                {
                    return LowestBitIndex;
                }
            }

            return INDEX_NONE;
        }

        i32 FindLastFromImpl(bool bValue, i32 EndIndexExclusive) const
        {
            if (NumBits == 0)
            {
                return INDEX_NONE;
            }

            // Produce a mask for the first iteration
            u32 Mask = GetLastWordMask(EndIndexExclusive);

            // Iterate over the array until we see a word with a matching bit
            const u32* DwordArray = GetData();
            u32 DwordIndex = FBitSet::CalculateNumWords(EndIndexExclusive);
            const u32 Test = bValue ? 0u : ~0u;
            for (;;)
            {
                if (DwordIndex == 0)
                {
                    return INDEX_NONE;
                }
                --DwordIndex;
                if ((DwordArray[DwordIndex] & Mask) != (Test & Mask))
                {
                    break;
                }
                Mask = ~0u;
            }

            // If we're looking for a false, then we flip the bits - then we only need to find the last one bit
            const u32 Bits = (bValue ? DwordArray[DwordIndex] : ~DwordArray[DwordIndex]) & Mask;
            OLO_CORE_ASSERT(Bits != 0, "Expected non-zero bits");

            u32 BitIndex = (NumBitsPerDWORD - 1) - BitArrayMath::CountLeadingZeros(Bits);

            i32 Result = static_cast<i32>(BitIndex + (DwordIndex << NumBitsPerDWORDLogTwo));
            return Result;
        }

        // ========================================================================
        // Member Variables
        // ========================================================================

        AllocatorType AllocatorInstance;
        i32 NumBits;
        i32 MaxBits;

        // Allow other TBitArray specializations to access private members
        template <typename OtherAllocator>
        friend class TBitArray;
    };

    // ============================================================================
    // TConstSetBitIterator
    // ============================================================================

    /**
     * @class TConstSetBitIterator
     * @brief An iterator which only iterates over set bits (bits with value true)
     */
    template <typename Allocator>
    class TConstSetBitIterator : public FRelativeBitReference
    {
    public:
        [[nodiscard]] explicit TConstSetBitIterator(const TBitArray<Allocator>& InArray)
            : FRelativeBitReference(0)
            , Array(InArray)
            , UnvisitedBitMask(~0u)
            , CurrentBitIndex(0)
            , BaseBitIndex(0)
        {
            if (Array.Num())
            {
                FindFirstSetBit();
            }
        }

        [[nodiscard]] explicit TConstSetBitIterator(const TBitArray<Allocator>& InArray, i32 StartIndex)
            : FRelativeBitReference(StartIndex)
            , Array(InArray)
            , UnvisitedBitMask((~0u) << (StartIndex & (NumBitsPerDWORD - 1)))
            , CurrentBitIndex(StartIndex)
            , BaseBitIndex(StartIndex & ~(NumBitsPerDWORD - 1))
        {
            OLO_CORE_ASSERT(StartIndex >= 0 && StartIndex <= Array.Num(), "StartIndex out of bounds");
            if (StartIndex != Array.Num())
            {
                FindFirstSetBit();
            }
        }

        /** Advance to the next set bit */
        OLO_FINLINE TConstSetBitIterator& operator++()
        {
            // Mark the current bit as visited
            UnvisitedBitMask &= ~this->Mask;

            // Find the first set bit that hasn't been visited yet
            FindFirstSetBit();

            return *this;
        }

        [[nodiscard]] OLO_FINLINE bool operator==(const TConstSetBitIterator& Rhs) const
        {
            return CurrentBitIndex == Rhs.CurrentBitIndex && &Array == &Rhs.Array;
        }

        [[nodiscard]] OLO_FINLINE bool operator!=(const TConstSetBitIterator& Rhs) const
        {
            return !(*this == Rhs);
        }

        /** Conversion to bool - true if iterator is valid */
        [[nodiscard]] OLO_FINLINE explicit operator bool() const
        {
            return CurrentBitIndex < Array.Num();
        }

        /** Inverse of bool operator */
        [[nodiscard]] OLO_FINLINE bool operator!() const
        {
            return !(bool)*this;
        }

        /** Returns the current bit index */
        [[nodiscard]] OLO_FINLINE i32 GetIndex() const
        {
            return CurrentBitIndex;
        }

    private:
        /** Find the first set bit starting with the current bit */
        void FindFirstSetBit()
        {
            const u32* ArrayData = Array.GetData();
            const i32 ArrayNum = Array.Num();
            const i32 LastWordIndex = (ArrayNum - 1) / NumBitsPerDWORD;

            // Advance to the next non-zero word
            u32 RemainingBitMask = ArrayData[this->WordIndex] & UnvisitedBitMask;
            while (!RemainingBitMask)
            {
                ++this->WordIndex;
                BaseBitIndex += NumBitsPerDWORD;
                if (this->WordIndex > LastWordIndex)
                {
                    // We've advanced past the end of the array
                    CurrentBitIndex = ArrayNum;
                    return;
                }

                RemainingBitMask = ArrayData[this->WordIndex];
                UnvisitedBitMask = ~0u;
            }

            // Isolate the lowest set bit
            const u32 NewRemainingBitMask = RemainingBitMask & (RemainingBitMask - 1);
            this->Mask = NewRemainingBitMask ^ RemainingBitMask;

            // Calculate the bit index
            CurrentBitIndex = BaseBitIndex + NumBitsPerDWORD - 1 - BitArrayMath::CountLeadingZeros(this->Mask);

            // Handle iteration past the end within the same word
            if (CurrentBitIndex > ArrayNum)
            {
                CurrentBitIndex = ArrayNum;
            }
        }

        const TBitArray<Allocator>& Array;
        u32 UnvisitedBitMask;
        i32 CurrentBitIndex;
        i32 BaseBitIndex;
    };

    // ============================================================================
    // TConstDualSetBitIterator
    // ============================================================================

    /**
     * @class TConstDualSetBitIterator
     * @brief Iterator over bits set in BOTH of two bit arrays (intersection)
     * 
     * Used to efficiently iterate over the intersection of two sets' allocation flags.
     */
    template <typename Allocator = FDefaultAllocator, typename OtherAllocator = FDefaultAllocator>
    class TConstDualSetBitIterator : public FRelativeBitReference
    {
    public:
        [[nodiscard]] TConstDualSetBitIterator(
            const TBitArray<Allocator>& InArrayA,
            const TBitArray<OtherAllocator>& InArrayB)
            : FRelativeBitReference(0)
            , ArrayA(InArrayA)
            , ArrayB(InArrayB)
            , UnvisitedBitMask(~0u)
            , CurrentBitIndex(0)
            , BaseBitIndex(0)
        {
            if (ArrayA.Num() && ArrayB.Num())
            {
                FindFirstSetBit();
            }
            else
            {
                CurrentBitIndex = 0;
            }
        }

        /** Advance to the next bit set in both arrays */
        OLO_FINLINE TConstDualSetBitIterator& operator++()
        {
            OLO_CORE_ASSERT(ArrayA.Num() > 0 && ArrayB.Num() > 0, "Incrementing invalid iterator");

            // Mark the current bit as visited
            UnvisitedBitMask &= ~this->Mask;

            // Find the next set bit
            FindFirstSetBit();

            return *this;
        }

        /** Conversion to bool - true if iterator is valid */
        [[nodiscard]] OLO_FINLINE explicit operator bool() const
        {
            return CurrentBitIndex < ArrayA.Num() && CurrentBitIndex < ArrayB.Num();
        }

        /** Inverse of bool operator */
        [[nodiscard]] OLO_FINLINE bool operator!() const
        {
            return !(bool)*this;
        }

        /** Returns the current bit index */
        [[nodiscard]] OLO_FINLINE i32 GetIndex() const
        {
            return CurrentBitIndex;
        }

    private:
        /** Find the first bit that's set in both arrays */
        void FindFirstSetBit()
        {
            const i32 ArrayANum = ArrayA.Num();
            const i32 ArrayBNum = ArrayB.Num();
            const i32 MinNum = (ArrayANum < ArrayBNum) ? ArrayANum : ArrayBNum;
            
            if (MinNum == 0)
            {
                CurrentBitIndex = 0;
                return;
            }

            const u32* ArrayDataA = ArrayA.GetData();
            const u32* ArrayDataB = ArrayB.GetData();
            const i32 LastWordIndex = (MinNum - 1) / NumBitsPerDWORD;

            // Find the intersection of both arrays
            u32 RemainingBitMask = ArrayDataA[this->WordIndex] & ArrayDataB[this->WordIndex] & UnvisitedBitMask;
            while (!RemainingBitMask)
            {
                ++this->WordIndex;
                BaseBitIndex += NumBitsPerDWORD;
                if (this->WordIndex > LastWordIndex)
                {
                    CurrentBitIndex = MinNum;
                    return;
                }

                RemainingBitMask = ArrayDataA[this->WordIndex] & ArrayDataB[this->WordIndex];
                UnvisitedBitMask = ~0u;
            }

            // Isolate the lowest set bit
            const u32 NewRemainingBitMask = RemainingBitMask & (RemainingBitMask - 1);
            this->Mask = NewRemainingBitMask ^ RemainingBitMask;

            // Calculate the bit index
            CurrentBitIndex = BaseBitIndex + NumBitsPerDWORD - 1 - BitArrayMath::CountLeadingZeros(this->Mask);

            // Handle iteration past the end
            if (CurrentBitIndex >= MinNum)
            {
                CurrentBitIndex = MinNum;
            }
        }

        const TBitArray<Allocator>& ArrayA;
        const TBitArray<OtherAllocator>& ArrayB;
        u32 UnvisitedBitMask;
        i32 CurrentBitIndex;
        i32 BaseBitIndex;
    };

    // ============================================================================
    // Hash Function
    // ============================================================================

    template <typename Allocator>
    [[nodiscard]] OLO_FINLINE u32 GetTypeHash(const TBitArray<Allocator>& BitArray)
    {
        u32 NumWords = FBitSet::CalculateNumWords(BitArray.Num());
        u32 Hash = NumWords;
        const u32* Data = BitArray.GetData();
        for (u32 i = 0; i < NumWords; ++i)
        {
            Hash ^= Data[i];
        }
        return Hash;
    }

} // namespace OloEngine
