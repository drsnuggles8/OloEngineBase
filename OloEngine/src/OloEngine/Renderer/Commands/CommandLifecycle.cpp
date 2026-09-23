#include "OloEnginePCH.h"
#include "CommandLifecycle.h"

#include "OloEngine/Core/DebugLevers.h"

#include <array>
#include <atomic>
#include <bit>

namespace OloEngine::CommandLifecycle
{
    namespace
    {
        constexpr sizet kViolationKinds = static_cast<sizet>(Violation::Count);

        std::array<std::atomic<u64>, kViolationKinds> s_Counts{};
        std::atomic<i32> s_ExpectedScopes{ 0 };
        std::atomic<u64> s_UnexpectedCount{ 0 };

#ifdef OLO_DEBUG
        constexpr bool kValidationByDefault = true;
#else
        constexpr bool kValidationByDefault = false;
#endif
    } // namespace

    const char* ToString(Violation violation)
    {
        switch (violation)
        {
            case Violation::PacketMutatedAfterFreeze:
                return "PacketMutatedAfterFreeze";
            case Violation::PacketChangedAfterFreeze:
                return "PacketChangedAfterFreeze";
            case Violation::PacketChangedDuringReplay:
                return "PacketChangedDuringReplay";
            case Violation::BucketMutatedAfterFreeze:
                return "BucketMutatedAfterFreeze";
            case Violation::ReplayOfUnfrozenBucket:
                return "ReplayOfUnfrozenBucket";
            case Violation::PayloadWrittenAfterPublish:
                return "PayloadWrittenAfterPublish";
            case Violation::Count:
                break;
        }
        return "Unknown";
    }

    namespace
    {
        [[nodiscard]] const char* Explain(Violation violation)
        {
            switch (violation)
            {
                case Violation::PacketMutatedAfterFreeze:
                    return "a mutating accessor was called on a packet already frozen for replay; read it through a "
                           "const pointer, or Clone it and edit the copy";
                case Violation::PacketChangedAfterFreeze:
                    return "a frozen packet's bytes changed before a replay; the replay was skipped";
                case Violation::PacketChangedDuringReplay:
                    return "a frozen packet's bytes changed while a replay was reading them";
                case Violation::BucketMutatedAfterFreeze:
                    return "a bucket already frozen for replay was asked to change; the operation did nothing";
                case Violation::ReplayOfUnfrozenBucket:
                    return "a bucket was replayed without being frozen first; nothing was replayed";
                case Violation::PayloadWrittenAfterPublish:
                    return "a FrameDataBuffer range published for replay was written; the write did nothing";
                case Violation::Count:
                    break;
            }
            return "unknown";
        }
    } // namespace

    void ReportViolation(Violation violation, const char* where)
    {
        const auto index = static_cast<sizet>(violation);
        if (index >= kViolationKinds)
            return;

        const u64 occurrence = s_Counts[index].fetch_add(1, std::memory_order_relaxed) + 1;
        const bool expected = s_ExpectedScopes.load(std::memory_order_relaxed) > 0;

        // Powers of two: the first report of each kind is always logged, and a
        // fault that repeats every frame keeps reappearing, exponentially less
        // often, rather than either flooding the log or going quiet after one.
        if (std::has_single_bit(occurrence))
        {
            OLO_CORE_ERROR("Command lifecycle violation {} in {} (occurrence {}): {}. See "
                           "Renderer/Commands/CommandLifecycle.h.",
                           ToString(violation), where ? where : "<unknown>", occurrence, Explain(violation));
        }

        if (!expected)
        {
            s_UnexpectedCount.fetch_add(1, std::memory_order_relaxed);
            OLO_CORE_ASSERT(false, "Command lifecycle violation: {} in {}", ToString(violation), where ? where : "<unknown>");
        }
    }

    u64 GetViolationCount(Violation violation)
    {
        const auto index = static_cast<sizet>(violation);
        return index < kViolationKinds ? s_Counts[index].load(std::memory_order_relaxed) : 0;
    }

    u64 GetTotalViolationCount()
    {
        u64 total = 0;
        for (const auto& count : s_Counts)
            total += count.load(std::memory_order_relaxed);
        return total;
    }

    bool IsValidationEnabled()
    {
        switch (Levers::CommandLifecycleValidation())
        {
            case Levers::Tristate::On:
                return true;
            case Levers::Tristate::Off:
                return false;
            case Levers::Tristate::Unset:
                break;
        }
        return kValidationByDefault;
    }

    void SetValidationEnabled(bool enabled)
    {
        Levers::SetCommandLifecycleValidation(enabled ? Levers::Tristate::On : Levers::Tristate::Off);
    }

    ScopedExpectedViolations::ScopedExpectedViolations()
    {
        s_ExpectedScopes.fetch_add(1, std::memory_order_relaxed);
    }

    ScopedExpectedViolations::~ScopedExpectedViolations()
    {
        s_ExpectedScopes.fetch_sub(1, std::memory_order_relaxed);
    }

    u64 GetUnexpectedViolationCount()
    {
        return s_UnexpectedCount.load(std::memory_order_relaxed);
    }
} // namespace OloEngine::CommandLifecycle
