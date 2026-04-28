#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    class RGCommandContext;

    // @brief Base class for all render passes.
    //
    // Provides the minimal interface: name, framebuffer lifecycle, and execution.
    // Passes that need a command bucket should inherit CommandBufferRenderPass instead.
    //
    // Resource declarations (DeclareRead / DeclareWrite) are optional opt-in
    // metadata consumed by `RenderGraph::ValidateResourceHazards()`. Passes
    // that don't declare resources are skipped by the validator — their
    // ordering still has to be correct, just unchecked. Migrating a pass is
    // as simple as calling `DeclareRead(ResourceNames::ShadowMapCSM)` etc.
    // in `Init()`.
    class RenderPass : public RefCounted
    {
      public:
        enum class SubmissionModel : u8
        {
            Unknown = 0,
            BucketOnly,
            ImmediateOnly,
            Mixed,
        };
        enum class SideEffect : u8
        {
            None = 0,              // Pass is culled if unreachable
            Readback = 1 << 0,     // Pass performs GPU readback (keep for GPU counter/picking)
            Present = 1 << 1,      // Pass writes to swap-chain/backbuffer
            DebugCapture = 1 << 2, // Pass captures debug data (RenderDoc, profiler)
            Timestamp = 1 << 3,    // Pass records GPU timestamp
        };

        virtual ~RenderPass() = default;

        virtual void Init(const FramebufferSpecification& spec) = 0;
        virtual void Execute() = 0;
        virtual void Execute(RGCommandContext& context)
        {
            (void)context;
            Execute();
        }
        [[nodiscard]] virtual Ref<Framebuffer> GetTarget() const = 0;

        void SetName(std::string_view name)
        {
            m_Name = name;
        }
        [[nodiscard]] const std::string& GetName() const
        {
            return m_Name;
        }

        virtual void SetupFramebuffer(u32 /*width*/, u32 /*height*/) {}
        virtual void ResizeFramebuffer(u32 /*width*/, u32 /*height*/) {}
        virtual void OnReset() {}
        [[nodiscard]] virtual SubmissionModel GetSubmissionModel() const
        {
            return SubmissionModel::Unknown;
        }

        // Called by RenderGraph to pipe the output framebuffer of a previous pass as input.
        // Passes that accept an input framebuffer should override this.
        virtual void SetInputFramebuffer(const Ref<Framebuffer>& /*input*/) {}

        // Mark this pass as having side effects that prevent culling.
        // Multiple SideEffect flags can be combined with bitwise OR.
        void SetSideEffects(SideEffect effects)
        {
            m_SideEffects = effects;
        }
        [[nodiscard]] SideEffect GetSideEffects() const
        {
            return m_SideEffects;
        }
        [[nodiscard]] bool IsSideEffecting() const
        {
            return static_cast<u8>(m_SideEffects) != 0;
        }

        // -------------------------------------------------------------------
        // Resource-aware RDG API (opt-in)
        // -------------------------------------------------------------------
        // Passes declare what they sample (reads) and what they produce
        // (writes). RenderGraph::ValidateResourceHazards() uses these
        // declarations to ensure every reader has a transitive execution
        // dependency on the writer, catching missing `AddExecutionDependency`
        // calls at graph construction time.
        //
        // Declarations should be made during `Init()` (or immediately
        // afterwards), and are immutable from the graph's perspective once
        // validation runs.

        [[nodiscard]] const std::vector<ResourceHandle>& GetReads() const
        {
            return m_Reads;
        }
        [[nodiscard]] const std::vector<ResourceHandle>& GetWrites() const
        {
            return m_Writes;
        }

        void ClearResourceDeclarations()
        {
            m_Reads.clear();
            m_Writes.clear();
        }

      protected:
        // Derived passes call these from Init() (or helper setup methods).
        // Duplicates are silently deduped — a pass may legitimately declare
        // the same read from multiple shaders during setup.
        void DeclareRead(std::string_view name, ResourceHandle::Kind kind = ResourceHandle::Kind::Unknown)
        {
            for (const auto& existing : m_Reads)
            {
                if (existing.Name == name)
                    return;
            }
            m_Reads.emplace_back(name, kind);
        }

        void DeclareWrite(std::string_view name, ResourceHandle::Kind kind = ResourceHandle::Kind::Unknown)
        {
            for (const auto& existing : m_Writes)
            {
                if (existing.Name == name)
                    return;
            }
            m_Writes.emplace_back(name, kind);
        }

        std::string m_Name = "RenderPass";
        Ref<Framebuffer> m_Target;
        FramebufferSpecification m_FramebufferSpec;

        // Resource declarations (opt-in). Empty = unchecked by validator.
        std::vector<ResourceHandle> m_Reads;
        std::vector<ResourceHandle> m_Writes;

        // Side-effect flags (Phase E). Determine if pass is culled when unreachable.
        SideEffect m_SideEffects = SideEffect::None;
    };
} // namespace OloEngine
