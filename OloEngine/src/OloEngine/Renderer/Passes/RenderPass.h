#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <functional>
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
    class RenderPass : public RenderGraphNode
    {
      public:
        using SubmissionModel = RenderGraphSubmissionModel;
        enum class SideEffect : u8
        {
            None = 0,              // Pass is culled if unreachable
            Readback = 1 << 0,     // Pass performs GPU readback (keep for GPU counter/picking)
            Present = 1 << 1,      // Pass writes to swap-chain/backbuffer
            DebugCapture = 1 << 2, // Pass captures debug data (RenderDoc, profiler)
            Timestamp = 1 << 3,    // Pass records GPU timestamp
            NeverCull = 1 << 4,    // Pass is always executed regardless of reachability
        };

        // Phase G — Work-type classification for queue scheduling.
        // A pass is exactly one of: Graphics (rasterization), Compute (dispatch-only),
        // or Copy (transfer/blit only). Used by the scheduler to determine which
        // hardware queue the pass can run on.
        using PassWorkType = RenderGraphPassWorkType;
        using SetupCallback = std::function<void(RGBuilder&, FrameBlackboard&)>;

        virtual ~RenderPass() = default;

        [[nodiscard]] const std::string& GetName() const override
        {
            return m_Name;
        }

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override
        {
            if (m_SetupCallback)
                m_SetupCallback(builder, blackboard);
        }

        virtual void Init(const FramebufferSpecification& spec) = 0;
        virtual void Execute() = 0;
        virtual void Execute(RGCommandContext& context)
        {
            (void)context;
            Execute();
        }
        [[nodiscard]] virtual Ref<Framebuffer> GetTarget() const = 0;
        [[nodiscard]] const FramebufferSpecification& GetFramebufferSpecification() const
        {
            return m_FramebufferSpec;
        }

        void SetName(std::string_view name)
        {
            m_Name = name;
        }

        void SetSetupCallback(SetupCallback callback)
        {
            m_SetupCallback = std::move(callback);
        }

        void ClearSetupCallback()
        {
            m_SetupCallback = {};
        }

        [[nodiscard]] RenderGraphNodeFlags GetFlags() const override
        {
            auto flags = RenderGraphNodeFlags::None;
            switch (m_PassWorkType)
            {
                case PassWorkType::Compute:
                    flags = flags | RenderGraphNodeFlags::Compute;
                    break;
                case PassWorkType::Copy:
                    flags = flags | RenderGraphNodeFlags::Copy;
                    break;
                case PassWorkType::Graphics:
                default:
                    flags = flags | RenderGraphNodeFlags::Graphics;
                    break;
            }

            if ((static_cast<u8>(m_SideEffects) & static_cast<u8>(SideEffect::Present)) != 0u)
                flags = flags | RenderGraphNodeFlags::Present;
            if ((static_cast<u8>(m_SideEffects) & static_cast<u8>(SideEffect::Readback)) != 0u)
                flags = flags | RenderGraphNodeFlags::Readback;
            if ((static_cast<u8>(m_SideEffects) & static_cast<u8>(SideEffect::NeverCull)) != 0u)
                flags = flags | RenderGraphNodeFlags::NeverCull;
            if ((static_cast<u8>(m_SideEffects) & (static_cast<u8>(SideEffect::DebugCapture) | static_cast<u8>(SideEffect::Timestamp))) != 0u)
                flags = flags | RenderGraphNodeFlags::ExternalSideEffect;

            return flags;
        }

        virtual void SetupFramebuffer(u32 /*width*/, u32 /*height*/) {}
        virtual void ResizeFramebuffer(u32 /*width*/, u32 /*height*/) {}

        // Dynamic Resolution Scaling support.
        // Called by RenderGraph::SetRenderScale() to set the active render
        // viewport on this pass's framebuffer(s) WITHOUT reallocating GPU
        // memory (contrast with ResizeFramebuffer which reallocates).
        // The default implementation updates m_Target when present. Passes
        // with multiple framebuffers (e.g. BloomRenderPass) should override
        // this to cover all their targets.
        virtual void ApplyRenderViewport(u32 width, u32 height)
        {
            m_RenderViewportWidth = width;
            m_RenderViewportHeight = height;
            if (m_Target)
                m_Target->SetRenderViewportSize(width, height);
        }

        virtual void OnReset() {}
        [[nodiscard]] virtual SubmissionModel GetSubmissionModel() const
        {
            return SubmissionModel::Unknown;
        }

        [[nodiscard]] std::span<const ResourceHandle> GetDeclaredReads() const override
        {
            return { m_Reads.data(), m_Reads.size() };
        }

        [[nodiscard]] std::span<const ResourceHandle> GetDeclaredWrites() const override
        {
            return { m_Writes.data(), m_Writes.size() };
        }

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
        // Phase G — Pass work-type and async-compute scheduling
        // -------------------------------------------------------------------
        void SetPassWorkType(PassWorkType type)
        {
            m_PassWorkType = type;
        }
        [[nodiscard]] PassWorkType GetPassWorkType() const
        {
            return m_PassWorkType;
        }
        [[nodiscard]] bool IsComputeOnly() const
        {
            return m_PassWorkType == PassWorkType::Compute;
        }

        // Mark this pass as a candidate for running on the async-compute queue.
        // Only meaningful when PassWorkType == Compute and the backend supports
        // asynchronous compute (Vulkan / DX12). GL 4.6 ignores this flag.
        void SetAsyncComputeCandidate(bool candidate)
        {
            m_AsyncComputeCandidate = candidate;
        }
        [[nodiscard]] bool IsAsyncComputeCandidate() const
        {
            return m_AsyncComputeCandidate;
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
        SetupCallback m_SetupCallback;

        // DRS render-viewport dimensions set by ApplyRenderViewport().
        // Zero means "use physical framebuffer size" (no DRS override active).
        u32 m_RenderViewportWidth = 0;
        u32 m_RenderViewportHeight = 0;

        // Resource declarations (opt-in). Empty = unchecked by validator.
        std::vector<ResourceHandle> m_Reads;
        std::vector<ResourceHandle> m_Writes;

        // Side-effect flags (Phase E). Determine if pass is culled when unreachable.
        SideEffect m_SideEffects = SideEffect::None;

        // Phase G — work-type and async-compute scheduling flags.
        PassWorkType m_PassWorkType = PassWorkType::Graphics;
        bool m_AsyncComputeCandidate = false;
    };
} // namespace OloEngine
