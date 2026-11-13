#include "OloEnginePCH.h"
#include "OloEngine/Renderer/UniformBlockContract.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <atomic>
#include <mutex>
#include <unordered_set>

namespace OloEngine::BindingContract
{
    namespace
    {
        std::atomic<u64> s_Violations{ 0 };
        std::atomic<u64> s_WorkerViolations{ 0 };
        std::atomic<ProgramRetiredHandler> s_ProgramRetired{ nullptr };

        // Process lifetime and deliberately leaked: violations can be reported
        // from recording workers and during teardown, after ordinary statics
        // are gone (registries-must-outlive-their-registrants.md).
        struct ReportedSet
        {
            std::mutex Mutex;
            std::unordered_set<u64> Keys;
        };

        // Which refusal this is, for "log it once": program, binding point,
        // verdict and occupant. A hash, so that a refusal that repeats (one
        // per draw, from every recording worker, while the fault lasts) costs
        // no allocation.
        [[nodiscard]] u64 ViolationKey(std::string_view program, u32 binding, Verdict verdict, std::string_view occupantBlock)
        {
            u64 hash = 14695981039346656037ull;
            const auto mix = [&hash](std::string_view bytes)
            {
                for (const char c : bytes)
                {
                    hash ^= static_cast<u8>(c);
                    hash *= 1099511628211ull;
                }
                hash ^= 0xFFu; // a separator no name byte sequence ends with
                hash *= 1099511628211ull;
            };
            mix(program);
            mix(occupantBlock);
            hash ^= (static_cast<u64>(binding) << 8) | static_cast<u64>(verdict);
            hash *= 1099511628211ull;
            return hash;
        }
        ReportedSet& Reported()
        {
            static auto* s_Reported = new ReportedSet;
            return *s_Reported;
        }
    } // namespace

    bool Enabled()
    {
        return Levers::BindingContract() != Levers::Tristate::Off;
    }

    void ReportViolation(std::string_view program, const BlockRequirement& required, std::string_view requiredBlock,
                         u32 occupantSize, std::string_view occupantBlock, Verdict verdict)
    {
        s_Violations.fetch_add(1, std::memory_order_relaxed);
        if (RenderCommand::IsRecordingParallelItem())
            s_WorkerViolations.fetch_add(1, std::memory_order_relaxed);

        // The same refusal again on this thread, which is what a persistent
        // fault looks like: no lock, so recording workers do not queue up
        // behind one mutex to learn that it was logged already.
        const u64 key = ViolationKey(program, required.Binding, verdict, occupantBlock);
        thread_local u64 t_LastKey = 0;
        if (t_LastKey == key)
            return;
        t_LastKey = key;
        {
            auto& reported = Reported();
            const std::lock_guard lock(reported.Mutex);
            if (!reported.Keys.insert(key).second)
                return;
        }
        OLO_CORE_ERROR("[BindingContract] '{}' reads uniform block '{}' ({} B) at binding {}, but the occupant is '{}' ({} B): {}. "
                       "The shader is given the zero block instead. Bind the block's own buffer before this draw or dispatch "
                       "(docs/agent-rules/shared-uniform-binding-bind-before-dispatch.md).",
                       program, requiredBlock, required.Size, required.Binding,
                       occupantBlock.empty() ? std::string_view{ "<undeclared>" } : occupantBlock, occupantSize, ToString(verdict));
    }

    void SetProgramRetiredHandler(ProgramRetiredHandler handler)
    {
        s_ProgramRetired.store(handler, std::memory_order_relaxed);
    }

    void NotifyProgramRetired(u32 programID)
    {
        if (const ProgramRetiredHandler handler = s_ProgramRetired.load(std::memory_order_relaxed))
            handler(programID);
    }

    u64 ViolationCount()
    {
        return s_Violations.load(std::memory_order_relaxed);
    }

    u64 WorkerViolationCount()
    {
        return s_WorkerViolations.load(std::memory_order_relaxed);
    }

    void ResetViolationCount()
    {
        s_WorkerViolations.store(0, std::memory_order_relaxed);
        s_Violations.store(0, std::memory_order_relaxed);
    }
} // namespace OloEngine::BindingContract
