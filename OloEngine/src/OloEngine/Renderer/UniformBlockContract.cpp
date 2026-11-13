#include "OloEnginePCH.h"
#include "OloEngine/Renderer/UniformBlockContract.h"

#include "OloEngine/Core/DebugLevers.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>

namespace OloEngine::BindingContract
{
    namespace
    {
        std::atomic<u64> s_Violations{ 0 };
        std::atomic<ProgramRetiredHandler> s_ProgramRetired{ nullptr };

        // Process lifetime and deliberately leaked: violations can be reported
        // from recording workers and during teardown, after ordinary statics
        // are gone (registries-must-outlive-their-registrants.md).
        struct ReportedSet
        {
            std::mutex Mutex;
            std::unordered_set<std::string> Keys;
        };
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

        std::string key;
        key.reserve(program.size() + occupantBlock.size() + 24u);
        key.append(program).append("|").append(std::to_string(required.Binding)).append("|");
        key.append(std::to_string(static_cast<u32>(verdict))).append("|").append(occupantBlock);
        {
            auto& reported = Reported();
            const std::lock_guard lock(reported.Mutex);
            if (!reported.Keys.insert(std::move(key)).second)
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

    void ResetViolationCount()
    {
        s_Violations.store(0, std::memory_order_relaxed);
    }
} // namespace OloEngine::BindingContract
