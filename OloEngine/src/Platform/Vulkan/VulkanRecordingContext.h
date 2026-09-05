#pragma once

// =============================================================================
// VulkanRecordingContext — everything VulkanRendererAPI keeps PER COMMAND
// BUFFER, extracted so a parallel region can hand each work item its own
// (issue #806, ADR 0011 amendment (92)).
//
// The render thread owns one of these (VulkanRendererAPI::m_Main) for the
// frame's primary command buffer. RecordParallel hands out N more — one per
// work item, each wrapping a SECONDARY command buffer — to task workers.
// Every facade entry point resolves "the current context" through
// VulkanRendererAPI::Ctx(): the worker context whose item is running on this
// thread, else the main context. That is the whole threading model: the API
// object has no per-command-buffer members of its own any more, and nothing
// on the draw path needs to know which thread it is on.
//
// Two mirrors that are process-global on the single-threaded path become
// per-context here for the same reason: the bind-point mirror
// (VulkanBindingState) and the currently bound program (VulkanShader /
// VulkanComputeShader). Their static accessors consult the active worker
// context first and fall back to the process-wide object, so the main
// context keeps the singleton semantics every test fixture relies on.
// =============================================================================

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Templates/UnrealTemplate.h"

#include <atomic>
#include "Platform/Vulkan/VulkanBindingState.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"

#include <volk.h>

#include <glm/glm.hpp>

#include <array>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class VulkanFramebuffer;
    class VulkanVertexArray;
    class VulkanShader;
    class VulkanComputeShader;

    // The render-target half of a thin PSO's baked state (attachment
    // formats + sample count). Lives HERE beside VulkanRecordedPipelineState
    // — both are recorded state VulkanPipelineBuilder consumes, and the
    // builder's header includes this one (declaring it there would cycle).
    struct VulkanRenderTargetDesc
    {
        u32 ColorCount = 0;
        std::array<VkFormat, 8> ColorFormats{};
        VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
        u32 Samples = 1;
    };

    // Pipeline-key state recorded from the facade's state-setter entry
    // points. Vulkan bakes nearly all of this into the VkPipeline, so
    // this backend RECORDS it for pipeline-key derivation instead of
    // stubbing the setters — a state packet is a real dispatch, not a
    // loss (the execution test pins that state packets never touch
    // the stub counter).
    struct VulkanRecordedPipelineState
    {
        static constexpr u32 kMaxAttachments = 8;

        glm::vec4 ClearColor{ 0.0f };
        f32 ClearDepth = 1.0f;
        bool DepthTest = true;
        bool DepthWrite = true;
        RHI::CompareOp DepthFunc = RHI::CompareOp::Less;
        bool Blend = false;
        RHI::BlendFactor BlendSrcRGB = RHI::BlendFactor::One;
        RHI::BlendFactor BlendDstRGB = RHI::BlendFactor::Zero;
        RHI::BlendFactor BlendSrcAlpha = RHI::BlendFactor::One;
        RHI::BlendFactor BlendDstAlpha = RHI::BlendFactor::Zero;
        RHI::BlendOp BlendEquation = RHI::BlendOp::Add;
        bool StencilTest = false;
        RHI::CompareOp StencilFunc = RHI::CompareOp::Always;
        i32 StencilRef = 0;
        u32 StencilReadMask = 0xFFFFFFFFu;
        u32 StencilWriteMask = 0xFFFFFFFFu;
        RHI::StencilOp StencilFail = RHI::StencilOp::Keep;
        RHI::StencilOp StencilDepthFail = RHI::StencilOp::Keep;
        RHI::StencilOp StencilPass = RHI::StencilOp::Keep;
        bool Culling = false;
        RHI::CullMode CullFace = RHI::CullMode::Back;
        RHI::FrontFace FrontFaceWinding = RHI::FrontFace::CounterClockwise;
        RHI::PolygonMode PolygonMode = RHI::PolygonMode::Fill;
        bool PolygonOffsetEnabled = false;
        f32 PolygonOffsetFactor = 0.0f;
        f32 PolygonOffsetUnits = 0.0f;
        bool ScissorTest = false;
        bool Multisampling = true;
        f32 LineWidth = 1.0f;
        bool ColorMask[4] = { true, true, true, true };
        u32 PatchVertexCount = 3;

        // THE COLOUR MASK AND THE BLEND ENABLE COMPOSE DIFFERENTLY HERE, on
        // purpose -- do not "make them consistent" without re-reading this.
        //
        // AttachmentColorMask is the lowering's ONLY input: SetColorMask
        // overwrites every entry (glColorMask is defined as glColorMaski for
        // every draw buffer) and an indexed call then diverges one. It has to
        // work that way because CommandDispatch::ApplyRenderState only ever
        // DISABLES the attachments a command names and relies on the global
        // call to have re-enabled the rest. Before that fill existed, a
        // narrowing indexed call was permanent for the PROCESS, and one
        // Renderer3D::DrawLine killed every G-Buffer attachment above 0 --
        // issue #823.
        //
        // AttachmentBlend does NOT work that way. It is a TRI-STATE per
        // attachment and it OUTRANKS the global `Blend`, in BOTH directions:
        //
        //   Inherit  -- no pass has an opinion; the attachment follows `Blend`.
        //   ForceOn  -- blend this attachment even if `Blend` is false.
        //   ForceOff -- do not blend it even if `Blend` is true.
        //
        // so SetBlendState writes `Blend` and leaves this array alone, and only
        // SetBlendStateForAttachment / ResetBlendStateForAttachment move it.
        // GL's global glEnable(GL_BLEND) would flatten the array instead; the
        // GL backend re-asserts the standing opinions on top of the global call
        // so both backends implement THIS rule rather than two different ones
        // (OpenGLRendererAPI::ReassertAttachmentBlendOpinions).
        //
        // Both arms are load-bearing and each was a bug once:
        //
        //   ForceOn  -- DecalRenderPass enables RT2 per-attachment for an
        //               Emissive decal whose PODRenderState carries
        //               blendEnabled=false, and the additive One/One
        //               accumulation depends on that enable surviving the
        //               global disable ApplyPODRenderState then issues. The
        //               naive "match GL, let the global win" version was
        //               written during #823 and reverted because it deleted
        //               exactly this. Pinned by the Emissive arm of
        //               VulkanPassSuite.DecalGBufferModeMatrixMasksItsTargetRenderTargets.
        //   ForceOff -- OITResolveRenderPass disables RT1 (entity ID, an
        //               integer target) and RT2 (view normals) per attachment
        //               while compositing. Under the OR this array used to be,
        //               those two calls were no-ops the moment anything had
        //               enabled blending globally, so the pass silently did not
        //               get the disables it asked for -- issue #896. Pinned by
        //               VulkanDrawPath.PerAttachmentBlendOpinionOutranksTheGlobalEnable.
        //
        // THE PRICE, and it is the #823 archetype pointing the other way: an
        // opinion left behind outlives the pass that stated it, for the rest of
        // the process. A pass that turns an attachment on or off MUST call
        // ResetBlendStateForAttachment before it returns -- restoring by
        // passing `false` to the setter is NOT a withdrawal, it is a ForceOff
        // that would kill blending on that attachment permanently.
        enum class AttachmentBlendOpinion : u8
        {
            Inherit = 0,
            ForceOff,
            ForceOn,
        };
        AttachmentBlendOpinion AttachmentBlend[kMaxAttachments] = {};
        RHI::BlendFactor AttachmentBlendSrc[kMaxAttachments] = {};
        RHI::BlendFactor AttachmentBlendDst[kMaxAttachments] = {};
        // The same "did a pass state an opinion for attachment i" idea, one arm
        // narrower -- the FUNC has no on/off, only diverted or inherited, so
        // Inherit/Diverted is the whole state space.
        //
        // GL parity (#691, found by the OITResolve tenant):
        // glEnablei(GL_BLEND, i) alone does NOT give buffer i its own blend
        // func -- the GLOBAL glBlendFunc applies until glBlendFunci names the
        // buffer. A pass that per-attachment-ENABLES but sets only the global
        // func (OITResolve) must therefore blend with the global factors, not
        // the never-written per-attachment defaults (Zero/Zero).
        // SetBlendFuncForAttachment diverts buffer i; the global
        // SetBlendFunc/SetBlendFuncSeparate withdraw every divert (glBlendFunc
        // overwrites every buffer's func in GL). AttachmentBlendSrc/Dst are
        // meaningful only where this says Diverted.
        //
        // Note the asymmetry with AttachmentBlend directly above: the global
        // func setter DOES flatten this array while the global enable does not
        // flatten that one. That is not an oversight -- no caller wants a
        // surviving per-attachment FUNC (the passes here always re-state theirs
        // next to the enable), and the global-flatten is what makes a missed
        // withdrawal self-healing for the one of the pair where it can be.
        enum class AttachmentBlendFuncOpinion : u8
        {
            Inherit = 0,
            Diverted,
        };
        AttachmentBlendFuncOpinion AttachmentBlendFunc[kMaxAttachments] = {};
        u8 AttachmentColorMask[kMaxAttachments] = { 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF };
    };

    struct VulkanRecordingContext
    {
        // Lazy dynamic-rendering scope + draw assembly ----------
        //
        // GL passes freely interleave framebuffer binds, state calls, clears,
        // draws, barriers and copies; Vulkan forbids most non-draw commands
        // inside a vkCmdBeginRendering scope. The scope is therefore LAZY:
        // vkCmdBeginRendering is deferred to the first draw against the
        // currently-published framebuffer (VulkanBindingState), Clear()
        // before that folds into the attachments' loadOp, and any barrier /
        // clear-resource / copy / dispatch / target-change ENDS the scope
        // (the next draw resumes with LOAD_OP_LOAD). This is what lets
        // unmodified GL-shaped pass bodies record legal Vulkan.
        struct RenderingScope
        {
            bool Active = false;
            VulkanFramebuffer* Target = nullptr; ///< The scope's framebuffer (null iff TargetIsBackbuffer).
            /// The scope renders into the published swapchain image rather
            /// than a VulkanFramebuffer (see VulkanRendererAPI::FrameBackbuffer).
            /// BackbufferView is part of the scope's identity for the same
            /// reason DepthArrayView is: the publication changes per frame.
            bool TargetIsBackbuffer = false;
            VkImageView BackbufferView = VK_NULL_HANDLE;
            /// The per-layer depth view this scope was opened with, or
            /// VK_NULL_HANDLE when the target's OWN depth attachment was used
            /// (#691 §4). A shadow pass walks N cascades against ONE
            /// framebuffer object, so "same Target" is NOT enough to reuse the
            /// scope — see VulkanRendererAPI::ScopeMatchesCurrentTarget.
            VkImageView DepthArrayView = VK_NULL_HANDLE;
        };

        // A clear requested while no scope was open, waiting to fold into the
        // next scope-open's loadOp (#691). GL clears the BOUND
        // framebuffer eagerly, so the request must remember WHO asked:
        // The old target-blind bool pair meant `Bind(A); Clear(); Bind(B);
        // Draw()` cleared B and left A untouched, and a clear whose target
        // never drew again (a shadow-atlas entry culled to zero draws)
        // survived into the NEXT pass as that pass's loadOp — a pass that
        // intended LOAD lost its whole depth buffer. The identity is the
        // ScopeMatchesCurrentTarget triple, and the clear VALUES are captured
        // at request time (GL uses the state at glClear time, not at first
        // draw). A pending clear that stops matching the bound target is
        // MATERIALIZED as an eager transfer clear (MaterializePendingClear)
        // — exactly what GL did all along.
        struct PendingClear
        {
            bool Color = false;
            bool Depth = false;
            VulkanFramebuffer* Target = nullptr;
            bool TargetIsBackbuffer = false;
            VkImageView BackbufferView = VK_NULL_HANDLE;
            VkImageView DepthArrayView = VK_NULL_HANDLE;
            // Materialization needs the depth-array selection AS OF the
            // request — the pass may re-attach a different layer before the
            // pending is flushed (the CSM cascade walk), and the view alone
            // cannot recover image/layer.
            VkImage DepthArrayImage = VK_NULL_HANDLE;
            u32 DepthArrayLayer = 0;
            RHI::ResourceHandle DepthArrayHandle{};
            glm::vec4 ClearColor{ 0.0f };
            f32 ClearDepth = 1.0f;

            [[nodiscard]] bool Any() const
            {
                return Color || Depth;
            }
        };

        // GL's glNamedFramebufferDrawBuffers / ReadBuffer are PER-FRAMEBUFFER
        // PERSISTENT state, and both the bound form (SetDrawBuffers) and the
        // raw-handle form (SetFramebufferDrawAttachments) mutate the SAME
        // state on GL — so this backend models one map, keyed by the FB's
        // RHI handle, that both forms write (#691; replaces
        // the earlier scope-transient DrawList, which forgot the selection on
        // every target switch and could not express the raw form at all).
        // The scope build consumes DrawList at vkCmdBeginRendering; blits
        // consume ReadAttachment (src) and DrawList (dst fan-out).
        // Entries for destroyed framebuffers linger harmlessly: the key packs
        // the generation, so a recycled FB index gets a fresh entry.
        //
        // The depth-array LAYER selection (Framebuffer::AttachDepthTextureArrayLayer,
        // GL's glNamedFramebufferTextureLayer) is the same kind of per-
        // framebuffer state and rides in the same map (#806): it decides
        // which view the NEXT scope opens against, so it belongs to the
        // recording context that opens the scope, not to the framebuffer
        // object every item shares.
        struct FramebufferAttachmentSelection
        {
            // DrawList[i] = attachment index feeding fragment output location
            // i (RHI::NoAttachment → UNUSED). Count 0 = identity over every
            // color attachment (the engine-FB creation default on GL).
            std::array<u32, 8> DrawList{};
            u32 DrawListCount = 0;
            u32 ReadAttachment = 0; ///< glNamedFramebufferReadBuffer's selection.

            // The selected single-layer depth view, or Active = false when
            // the framebuffer's own depth attachment is the target.
            struct DepthArrayLayer
            {
                bool Active = false;
                VkImage Image = VK_NULL_HANDLE;
                VkImageView View = VK_NULL_HANDLE;
                VkFormat Format = VK_FORMAT_UNDEFINED;
                RHI::ResourceHandle Handle{};
                u32 Layer = 0;
            };
            DepthArrayLayer DepthArray{};
        };

        // The occlusion query BeginQuery opened (GL keeps one per target;
        // EndQuery carries no handle, so the pair is tracked here).
        struct ActiveQuery
        {
            VkQueryPool Pool = VK_NULL_HANDLE;
            u32 Index = 0;
            // TimeElapsed brackets stamp a second timestamp at Index+1 on
            // EndQuery; the handle re-Looks-up the entry there (never a cached
            // Entry* — the registry map can rehash between Begin and End).
            bool IsElapsed = false;
            RHI::ResourceHandle Handle{};
        };

        VulkanRecordingContext() = default;
        ~VulkanRecordingContext() = default;
        // The tracker self-registers by address; a context never moves.
        VulkanRecordingContext(const VulkanRecordingContext&) = delete;
        VulkanRecordingContext& operator=(const VulkanRecordingContext&) = delete;
        VulkanRecordingContext(VulkanRecordingContext&&) = delete;
        VulkanRecordingContext& operator=(VulkanRecordingContext&&) = delete;

        // --- identity ------------------------------------------------------
        u32 ItemIndex = 0; ///< Work-item index inside a parallel region (0 for the main context).
        u64 RegionId = 0;  ///< The region this item context was seeded for (0 for the main context).

        // --- the command buffer and what has been recorded into it ---------
        VkCommandBuffer Cmd = VK_NULL_HANDLE;
        RenderingScope Scope;
        VulkanRenderTargetDesc ScopeTargets; ///< Valid while Scope.Active.
        PendingClear Pending;                ///< At most one outstanding clear request (see PendingClear).
        VkBuffer BoundIndexBuffer = VK_NULL_HANDLE;
        /// Extent the cached bind used (#809's real-size vkCmdBindIndexBuffer2).
        /// Part of the cache key because a RAW element arena (issue #1052) can be
        /// re-allocated at a new size under the same identity, and VMA may hand
        /// back the same VkBuffer for the replacement — in which case the buffer
        /// alone no longer distinguishes the two binds.
        VkDeviceSize BoundIndexBufferSize = 0;
        bool HeapBoundThisRecording = false;
        VulkanVertexArray* BoundVertexArray = nullptr; ///< BindVertexArrayRaw's publication.
        std::vector<u8> RootScratch;
        VkDeviceAddress NextDrawRootDataAddress = 0;
        // Recorded scissor box (the WITH_COUNT dynamic state is emitted by
        // the draw front-end, not by the setter — see SetScissorBox).
        VkRect2D ScissorRect{};
        bool ScissorRectSet = false;
        ActiveQuery Query{};
        // Host-side conditional-render predicate: true between
        // BeginConditionalRender(occluded query) and EndConditionalRender, and
        // every draw in that window is skipped (see BeginConditionalRender for
        // why the predicate is evaluated on the host, not in a VK_EXT buffer).
        bool ConditionalRenderSkip = false;

        // --- recorded GL-shaped state, consumed at draw time -----------------
        VulkanRecordedPipelineState State;
        Viewport RecordedViewport{};
        std::unordered_map<u64, FramebufferAttachmentSelection> Selections;

        // --- per-recording tallies -----------------------------------------
        u32 PreparedDraws = 0;
        u32 DroppedDraws = 0;
        u32 GpuWrittenRootDraws = 0;
        u32 ConditionallySkippedDraws = 0;

        // --- the two formerly process-global mirrors ------------------------
        // Meaningful on WORKER contexts only: the main context keeps using
        // the process-wide VulkanBindingState singleton and the shader
        // classes' file statics, so a test-local API instance still shares
        // bind state with the process (the fixture contract documented in
        // VulkanBindingState.h). A worker's copies are seeded from those at
        // the fork.
        VulkanBindingState Binding;
        VulkanShader* CurrentShader = nullptr;
        VulkanComputeShader* CurrentComputeShader = nullptr;

        // --- image layouts ------------------------------------------------
        // Main: the frame's tracker. Worker: an overlay over the main
        // tracker (VulkanImageLayoutTracker::SetReadThroughBase), merged
        // back in item order at the join.
        VulkanImageLayoutTracker Tracker;

        // --- frame-arena block (workers) ----------------------------------
        // A worker claims 64 KiB from the slot's shared cursor once and bumps
        // inside it, so the per-draw root-data push touches no shared cache
        // line (VulkanFrameArena::Allocate). Offsets are absolute within the
        // slot buffer. Dropped at every fork; the tail of a block is waste.
        struct ArenaBlock
        {
            u64 Cursor = 0;
            u64 End = 0;
        };
        ArenaBlock Arena{};
        u64 ArenaAllocations = 0; ///< Folded into the arena's frame tally at the join.

        // --- telemetry (workers) -------------------------------------------
        f64 RecordMs = 0.0;
        bool Recorded = false; ///< The item body ran and the secondary was ended.

        // Command-buffer bind caches: what a NEW command buffer holds. Called
        // when the primary resumes after a flush and after
        // vkCmdExecuteCommands (state is undefined on the primary after it).
        void ForgetCommandBufferBinds()
        {
            BoundIndexBuffer = VK_NULL_HANDLE;
            BoundIndexBufferSize = 0;
            HeapBoundThisRecording = false;
            ScissorRectSet = false;
        }

        // BeginRecording's reset list: everything that is command-buffer
        // state. Frame-scoped state (selections, recorded pipeline state,
        // viewport) deliberately survives — a worker context receives those
        // from the fork's seed instead.
        void ResetForCommandBuffer(VkCommandBuffer cmd)
        {
            Cmd = cmd;
            ForgetCommandBufferBinds();
            Scope = RenderingScope{};
            // Defensive: EndRecording materializes an unconsumed pending clear,
            // so this should already be empty — but a stale record would carry
            // dead per-frame pointers (backbuffer view) into this recording.
            Pending = PendingClear{};
            PreparedDraws = 0;
            DroppedDraws = 0;
            GpuWrittenRootDraws = 0;
            ConditionallySkippedDraws = 0;
            NextDrawRootDataAddress = 0;
            Query = {};
            ConditionalRenderSkip = false;
            Arena = {};
            ArenaAllocations = 0;
            Recorded = false;
            RecordMs = 0.0;
        }
    };

    // The worker context whose RecordParallel item is running on the calling
    // thread, or nullptr: on the render thread outside a region, and on any
    // thread that is not executing an item. VulkanBindingState::Get(),
    // VulkanShader::GetCurrentlyBound() and VulkanRendererAPI::Ctx() all key
    // off this one thread-local.
    [[nodiscard]] VulkanRecordingContext* CurrentVulkanWorkerContext();

    // Amendment (92) rule 6 at record time: one writer per resource object per
    // region. `stamp` is the object's (region << 32 | item) writer token.
    // Off a worker context this is always true. On one it claims the token
    // for the calling item and returns false when a DIFFERENT item of the
    // same region already holds it — leaving that first writer's token in
    // place, so every later write from the second item is refused the same
    // way. The caller must skip its write on false in EVERY build: the
    // assert inside is compiled out of Release, and a write that went ahead
    // is exactly the interleaving that renders the wrong bytes.
    [[nodiscard]] bool ClaimParallelWriter(std::atomic<u64>& stamp, const char* objectKind);

    // RAII: make `context` the calling thread's worker context for the
    // scope, restoring whatever was current before (null on every existing
    // path; the nesting support is defensive). A TGuardValue over the
    // thread-local, wrapped only because the thread-local lives in the .cpp.
    class ScopedVulkanWorkerContext
    {
      public:
        explicit ScopedVulkanWorkerContext(VulkanRecordingContext* context);
        ~ScopedVulkanWorkerContext() = default;

      private:
        TGuardValue<VulkanRecordingContext*> m_Guard;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
