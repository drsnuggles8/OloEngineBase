#pragma once

#include "OloEngine/Core/Base.h"

#include <optional>

namespace OloEngine
{
    /**
     * @brief Where a byte figure came from (issue #1365).
     *
     * The same rule #1342 states for renderer allocations: a figure is either a
     * prediction, a measurement, or unknown, and the three are never conflated.
     */
    enum class EAssetByteSizeSource : u8
    {
        Unknown,  // no figure exists; never read as zero
        Estimate, // predicted before the bytes were resident (e.g. a pack's packed size)
        Actual    // measured from the resident object
    };

    /**
     * @brief A byte size tagged with its source.
     *
     * An unknown size has no byte count at all: GetBytes() returns std::nullopt,
     * so a caller cannot sum an unknown as zero without writing that decision down.
     */
    class FAssetByteSize
    {
      public:
        FAssetByteSize() = default;

        [[nodiscard]] static constexpr FAssetByteSize Unknown() noexcept
        {
            return FAssetByteSize{};
        }
        [[nodiscard]] static constexpr FAssetByteSize Estimate(u64 bytes) noexcept
        {
            return FAssetByteSize{ EAssetByteSizeSource::Estimate, bytes };
        }
        [[nodiscard]] static constexpr FAssetByteSize Actual(u64 bytes) noexcept
        {
            return FAssetByteSize{ EAssetByteSizeSource::Actual, bytes };
        }

        [[nodiscard]] constexpr EAssetByteSizeSource GetSource() const noexcept
        {
            return m_Source;
        }
        [[nodiscard]] constexpr bool IsKnown() const noexcept
        {
            return m_Source != EAssetByteSizeSource::Unknown;
        }
        [[nodiscard]] constexpr bool IsEstimate() const noexcept
        {
            return m_Source == EAssetByteSizeSource::Estimate;
        }
        [[nodiscard]] constexpr bool IsActual() const noexcept
        {
            return m_Source == EAssetByteSizeSource::Actual;
        }
        [[nodiscard]] constexpr std::optional<u64> GetBytes() const noexcept
        {
            if (!IsKnown())
                return std::nullopt;
            return m_Bytes;
        }

        auto operator==(const FAssetByteSize&) const -> bool = default;

      private:
        constexpr FAssetByteSize(EAssetByteSizeSource source, u64 bytes) noexcept
            : m_Source(source), m_Bytes(bytes) {}

        EAssetByteSizeSource m_Source = EAssetByteSizeSource::Unknown;
        u64 m_Bytes = 0;
    };

    /**
     * @brief A sum of FAssetByteSize values that keeps its unknowns visible.
     *
     * KnownBytes is the sum of every known figure; UnknownCount says how many
     * entries contributed nothing because their size is unknown. A report that
     * shows KnownBytes without UnknownCount would present "unknown" as "zero".
     */
    struct FAssetByteTotal
    {
        u64 KnownBytes = 0;
        u64 ActualBytes = 0;   // the part of KnownBytes that was measured
        u64 EstimateBytes = 0; // the part of KnownBytes that was predicted
        u32 Count = 0;
        u32 UnknownCount = 0;

        void Add(const FAssetByteSize& size) noexcept
        {
            ++Count;
            switch (size.GetSource())
            {
                case EAssetByteSizeSource::Unknown:
                    ++UnknownCount;
                    return;
                case EAssetByteSizeSource::Estimate:
                    EstimateBytes += *size.GetBytes();
                    break;
                case EAssetByteSizeSource::Actual:
                    ActualBytes += *size.GetBytes();
                    break;
            }
            KnownBytes += *size.GetBytes();
        }

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return UnknownCount == 0;
        }

        auto operator==(const FAssetByteTotal&) const -> bool = default;
    };
} // namespace OloEngine
