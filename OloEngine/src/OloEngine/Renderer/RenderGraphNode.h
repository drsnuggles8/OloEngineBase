#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace OloEngine
{
    class RGBuilder;
    class RGCommandContext;
    struct FrameBlackboard;

    enum class RenderGraphNodeFlags : u32
    {
        None = 0,
        Graphics = 1u << 0u,
        Compute = 1u << 1u,
        Copy = 1u << 2u,
        Present = 1u << 3u,
        Readback = 1u << 4u,
        NeverCull = 1u << 5u,
        UsesCommandBucket = 1u << 6u,
        ExternalSideEffect = 1u << 7u,
        AsyncCandidateMetadata = 1u << 8u,
    };

    [[nodiscard]] constexpr auto operator|(RenderGraphNodeFlags lhs, RenderGraphNodeFlags rhs) -> RenderGraphNodeFlags
    {
        return static_cast<RenderGraphNodeFlags>(std::to_underlying(lhs) | std::to_underlying(rhs));
    }

    [[nodiscard]] constexpr auto operator&(RenderGraphNodeFlags lhs, RenderGraphNodeFlags rhs) -> RenderGraphNodeFlags
    {
        return static_cast<RenderGraphNodeFlags>(std::to_underlying(lhs) & std::to_underlying(rhs));
    }

    [[nodiscard]] constexpr auto HasRenderGraphNodeFlag(RenderGraphNodeFlags flags, RenderGraphNodeFlags flag) -> bool
    {
        return (std::to_underlying(flags) & std::to_underlying(flag)) != 0u;
    }

    enum class RenderGraphPassWorkType : u8
    {
        Graphics = 0,
        Compute = 1,
        Copy = 2,
    };

    // Per-frame primary I/O handles selected by Setup() and consumed by Execute(). Reset by the graph between frames via ResetPrimaryHandlesForFrame().
    struct RGPrimaryHandleSet
    {
        RGFramebufferHandle InputFramebuffer;
        RGTextureHandle InputTexture;
        RGFramebufferHandle OutputFramebuffer;
        RGTextureHandle OutputTexture;
    };

    // RenderGraphNode is the single base class for all graph entries. It provides
    // storage-backed defaults for the framebuffer slot, primary I/O handles,
    // side-effect flags, work-type, and DRS viewport state — i.e. the surface
    // that was previously split out into `RenderPass`. Concrete passes override
    // `Setup(RGBuilder&, FrameBlackboard&)` and `Execute(RGCommandContext&)`.
    class RenderGraphNode : public RefCounted
    {
      public:
        using PassWorkType = RenderGraphPassWorkType;
        enum class SideEffect : u8
        {
            None = 0,              // Pass is culled if unreachable
            Readback = 1 << 0,     // Pass performs GPU readback (keep for GPU counter/picking)
            Present = 1 << 1,      // Pass writes to swap-chain/backbuffer
            DebugCapture = 1 << 2, // Pass captures debug data (RenderDoc, profiler)
            Timestamp = 1 << 3,    // Pass records GPU timestamp
            NeverCull = 1 << 4,    // Pass is always executed regardless of reachability
        };

        ~RenderGraphNode() override = default;

        [[nodiscard]] virtual const std::string& GetName() const
        {
            return m_Name;
        }

        void SetName(std::string_view name)
        {
            m_Name = name;
        }

        // Setup declares this node's resource accesses through RGBuilder. The
        // default impl resets the primary I/O handle cache so a node that does
        // nothing in Setup still ends up with cleared handles for the frame.
        virtual void Setup(RGBuilder& builder, FrameBlackboard& blackboard)
        {
            (void)builder;
            (void)blackboard;
        }

        // Per-frame reset of pass-local handle state — called by the graph
        // builder before Setup runs. Previously the default Setup() impl
        // silently performed this, which meant any pass that forgot to chain
        // to RenderGraphNode::Setup carried stale handles. Now it's a graph
        // invariant, not a passive contract on the override.
        void ResetPrimaryHandlesForFrame()
        {
            m_PrimaryHandles = {};
        }

        virtual void Execute(RGCommandContext& context) = 0;

        // Init is the resource-loading hook (shaders, UBOs, owned framebuffers).
        // The default implementation stores the framebuffer spec so resize/setup
        // hooks can update it. Passes that need to compile shaders, create UBOs,
        // or own framebuffers override this — they typically call the base
        // implementation first, then do their resource setup. Passes whose only
        // Init work is storing the spec (scene modifiers that render into a
        // borrowed framebuffer) can rely on the default impl.
        virtual void Init(const FramebufferSpecification& spec)
        {
            m_FramebufferSpec = spec;
        }

        // GetTarget returns the framebuffer this node renders into for the
        // current frame. The default impl returns m_Target, which Execute() sets
        // to the resolved output framebuffer (or nullptr when the pass passes
        // through / no-ops). Passes that write into a borrowed framebuffer
        // (scene modifiers, shadow) or have no GPU target (OIT prepare/resolve,
        // compute-only) override this to return their pass-specific framebuffer
        // or nullptr directly.
        [[nodiscard]] virtual Ref<Framebuffer> GetTarget() const
        {
            return m_Target;
        }

        [[nodiscard]] const FramebufferSpecification& GetFramebufferSpecification() const
        {
            return m_FramebufferSpec;
        }

        // Stores the setup-authoritative upstream framebuffer choice for the
        // current frame. Execute() paths resolve this handle directly instead
        // of repeating the setup-time fallback ladder.
        void SetPrimaryInputFramebufferHandle(const RGFramebufferHandle handle)
        {
            m_PrimaryHandles.InputFramebuffer = handle;
        }

        void SetPrimaryInputTextureHandle(const RGTextureHandle handle)
        {
            m_PrimaryHandles.InputTexture = handle;
        }

        void SetPrimaryOutputFramebufferHandle(const RGFramebufferHandle handle)
        {
            m_PrimaryHandles.OutputFramebuffer = handle;
        }

        void SetPrimaryOutputTextureHandle(const RGTextureHandle handle)
        {
            m_PrimaryHandles.OutputTexture = handle;
        }

        [[nodiscard]] auto GetPrimaryInputFramebufferHandle() const -> RGFramebufferHandle
        {
            return m_PrimaryHandles.InputFramebuffer;
        }

        [[nodiscard]] auto GetPrimaryInputTextureHandle() const -> RGTextureHandle
        {
            return m_PrimaryHandles.InputTexture;
        }

        [[nodiscard]] auto GetPrimaryOutputFramebufferHandle() const -> RGFramebufferHandle
        {
            return m_PrimaryHandles.OutputFramebuffer;
        }

        [[nodiscard]] auto GetPrimaryOutputTextureHandle() const -> RGTextureHandle
        {
            return m_PrimaryHandles.OutputTexture;
        }

        [[nodiscard]] virtual RenderGraphNodeFlags GetFlags() const
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

            if ((std::to_underlying(m_SideEffects) & std::to_underlying(SideEffect::Present)) != 0u)
                flags = flags | RenderGraphNodeFlags::Present;
            if ((std::to_underlying(m_SideEffects) & std::to_underlying(SideEffect::Readback)) != 0u)
                flags = flags | RenderGraphNodeFlags::Readback;
            if ((std::to_underlying(m_SideEffects) & std::to_underlying(SideEffect::NeverCull)) != 0u)
                flags = flags | RenderGraphNodeFlags::NeverCull;
            if ((std::to_underlying(m_SideEffects) & (std::to_underlying(SideEffect::DebugCapture) | std::to_underlying(SideEffect::Timestamp))) != 0u)
                flags = flags | RenderGraphNodeFlags::ExternalSideEffect;

            return flags;
        }

        // Debug-only introspection: lets the Render Graph Debugger ask any
        // pass whether it's user-enabled and whether it has the resources it
        // needs to actually execute this frame. Default-true so non-overriding
        // passes (compute helpers, scene modifiers without an explicit gate)
        // report "ready". Per-pass overrides reflect feature toggles and
        // shader/UBO readiness so the inspector can show why a pass is a no-op.
        [[nodiscard]] virtual bool IsEnabled() const noexcept
        {
            return true;
        }
        [[nodiscard]] virtual bool IsReadyForExecution() const noexcept
        {
            return true;
        }

        [[nodiscard]] virtual bool IsSideEffecting() const
        {
            if (std::to_underlying(m_SideEffects) != 0u)
                return true;
            const auto flags = GetFlags();
            return HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::Present) ||
                   HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::Readback) ||
                   HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::NeverCull) ||
                   HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::ExternalSideEffect);
        }

        [[nodiscard]] virtual RenderGraphPassWorkType GetPassWorkType() const
        {
            // Derive from GetFlags() so subclasses that override GetFlags()
            // directly (test stubs that set the flag bits in one place) stay
            // consistent with the storage-driven defaults.
            const auto flags = GetFlags();
            if (HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::Compute))
                return RenderGraphPassWorkType::Compute;
            if (HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::Copy))
                return RenderGraphPassWorkType::Copy;
            return RenderGraphPassWorkType::Graphics;
        }

        [[nodiscard]] virtual bool IsAsyncComputeCandidate() const
        {
            return m_AsyncComputeCandidate ||
                   HasRenderGraphNodeFlag(GetFlags(), RenderGraphNodeFlags::AsyncCandidateMetadata);
        }

        virtual void SetupFramebuffer(u32 /*width*/, u32 /*height*/) {}
        virtual void ResizeFramebuffer(u32 /*width*/, u32 /*height*/) {}

        // Dynamic Resolution Scaling support.
        // Called by RenderGraph::SetRenderScale() to set the active render
        // viewport on this node's framebuffer(s) WITHOUT reallocating GPU
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

        // Side-effect storage and accessors.
        void SetSideEffects(SideEffect effects)
        {
            m_SideEffects = effects;
        }

        [[nodiscard]] SideEffect GetSideEffects() const
        {
            return m_SideEffects;
        }

        // Work-type classification.
        void SetPassWorkType(PassWorkType type)
        {
            m_PassWorkType = type;
        }

        [[nodiscard]] bool IsComputeOnly() const
        {
            return m_PassWorkType == PassWorkType::Compute;
        }

        // Async-compute candidate flag — only meaningful on Compute nodes when
        // the backend supports asynchronous compute (Vulkan / DX12). GL 4.6
        // ignores this flag.
        void SetAsyncComputeCandidate(bool candidate)
        {
            m_AsyncComputeCandidate = candidate;
        }

      protected:
        std::string m_Name = "RenderGraphNode";
        Ref<Framebuffer> m_Target;
        FramebufferSpecification m_FramebufferSpec;

        // DRS render-viewport dimensions set by ApplyRenderViewport().
        // Zero means "use physical framebuffer size" (no DRS override active).
        u32 m_RenderViewportWidth = 0;
        u32 m_RenderViewportHeight = 0;

        // Per-frame setup-authoritative upstream fullscreen/post input.
        RGPrimaryHandleSet m_PrimaryHandles{};

        SideEffect m_SideEffects = SideEffect::None;
        PassWorkType m_PassWorkType = PassWorkType::Graphics;
        bool m_AsyncComputeCandidate = false;
    };
} // namespace OloEngine
