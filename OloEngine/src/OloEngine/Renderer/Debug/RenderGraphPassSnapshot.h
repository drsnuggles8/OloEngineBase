#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RendererAPI.h"

#include <functional>
#include <string>
#include <vector>

namespace OloEngine
{
    class RenderGraph;

    // @brief One-shot mid-frame snapshot of render-graph resources AS OF a
    // given pass's execution (issue #607 — the MCP tools' 'afterPass' param).
    //
    // End-of-frame readbacks cannot show what a mid-frame consumer sampled:
    // ParticlePass re-exports SceneDepth after GTAOPass, so by the time an MCP
    // capture reads "SceneDepth" the texels GTAO actually consumed are gone —
    // that single blindness cost six instrumented C++ rebuild rounds during
    // the GTAO black-sky hunt. Arm() installs a keyed post-pass hook on the
    // graph (coexisting with RenderGraphFrameCapture's hook); when the named
    // pass finishes executing, every requested resource is cloned bitwise
    // (every mip and layer, identical storage) into a scratch texture owned
    // here. Capture/probe/stats/compare tools then read the scratches instead
    // of the live resources. Multiple resources are cloned in the SAME hook
    // firing so a bitwise compare of two targets (olo_render_validate) sees
    // one consistent frame.
    //
    // The copy must happen AT HOOK TIME, not by remembering the resource: the
    // transient pool memory-aliases same-descriptor resources, so a mid-frame
    // object may be recycled for a different logical resource later in the
    // frame.
    //
    // BACKEND-NEUTRAL SINCE #810, and the currency change is the whole of it.
    // This used to speak native u32 texture names end to end, behind the
    // PassSnapshotBackend.h seam, on the stated grounds that `native -> handle`
    // is not recoverable. That reasoning holds for CONVERTING a name somebody
    // else minted — it does not apply to the scratch, which this code creates
    // itself and can therefore mint WITH an identity from birth. So the
    // resolver hands in an RHI::ResourceHandle, the clone is allocated by
    // RenderCommand::CreateMatchingTextureHandle (which reproduces the source's
    // own native storage description without translating it through a neutral
    // format vocabulary), copied by RenderCommand::CopyImageSubDataFull, and
    // read back through the facade readback spine like everything else. Both
    // backends run the same code; nothing here is GL.
    //
    // On Vulkan the copy lands in the CURRENT FRAME's command buffer, which is
    // what makes "as of that pass" true — CopyImageSubDataFull records into
    // `m_Cmd`. A one-shot submit would execute BEFORE the still-recording
    // frame (ADR 0011 amendment (72)) and would silently clone the PREVIOUS
    // frame, which is the same picture for a static scene and wrong for every
    // reason you would reach for this tool.
    //
    // Threading: Arm/Disarm/GetResults run on the main thread (the MCP server
    // marshals them there); the hook fires on the main thread inside
    // RenderGraph::Execute(). State hygiene: an image copy binds nothing — no
    // FBO, program, sampler, or UBO state is touched, so the engine-global
    // publications the render-pass-published-state rules protect are
    // unaffected.
    class RenderGraphPassSnapshot
    {
      public:
        // Resolves the source resource's IDENTITY at hook time (main thread,
        // mid-frame). Returning a null handle records a resolve failure.
        using Resolver = std::function<RHI::ResourceHandle()>;

        struct Request
        {
            std::string ResourceName;
            Resolver Resolve;
        };

        struct Result
        {
            bool Captured = false;
            std::string Error; // non-empty when the pass fired but this copy failed
            std::string ResourceName;
            // The scratch clone, owned by this class. Read it through the
            // facade like any other texture; do not cache it across an Arm().
            RHI::ResourceHandle Handle;
            RHI::ResourceHandle SourceHandle;
            u32 Width = 0;
            u32 Height = 0;
            u32 DepthOrLayers = 1;
            u32 MipLevels = 1;
            // BOTH currencies, per ADR 0011 amendment (77): the identity above
            // is what the tools read through, the native id below is what a
            // RenderDoc / RGP capture shows. Native is 0 on a backend whose
            // object has no 32-bit name.
            u32 NativeCloneId = 0;
            u32 NativeSourceId = 0;
            // The clone's storage format, in the neutral diagnostic vocabulary
            // (token + native enum value); the tools report the token and
            // decode with the rest.
            RHI::TextureFormatInfo Format;
        };

        RenderGraphPassSnapshot() = default;
        ~RenderGraphPassSnapshot();

        RenderGraphPassSnapshot(const RenderGraphPassSnapshot&) = delete;
        RenderGraphPassSnapshot& operator=(const RenderGraphPassSnapshot&) = delete;

        // Arm a one-shot snapshot: after `passName` executes during the next
        // RenderGraph::Execute(), clone every requested resource. Installs
        // this tool's keyed post-pass hook on `graph` (replacing any previous
        // armed request). Results are reset to empty.
        void Arm(RenderGraph* graph, std::string passName, std::vector<Request> requests);

        // Uninstall the hook and drop any pending request. The scratch
        // textures and the last results stay valid until the next Arm() or
        // ReleaseScratch().
        void Disarm();

        // True after Arm() until the named pass fires (or Disarm()).
        [[nodiscard]] bool IsPending() const
        {
            return m_Pending;
        }

        [[nodiscard]] const std::string& GetPassName() const
        {
            return m_PassName;
        }

        // One Result per Arm() request, in request order. Empty while pending
        // (the pass has not fired since arming).
        [[nodiscard]] const std::vector<Result>& GetResults() const
        {
            return m_Results;
        }

        // Free all scratch textures. Call at renderer shutdown (needs a live
        // device/context) — the destructor deliberately does NOT (the instance
        // is a process-static outliving both).
        void ReleaseScratch();

        // Hook entry point — public because the std::function installed by
        // Arm() captures `this` and forwards here.
        void OnPassExecuted(const std::string& passName, RenderGraph& graph);

      private:
        struct ScratchSlot
        {
            RHI::ResourceHandle Texture;
            u64 NativeFormat = 0;
            u32 Width = 0;
            u32 Height = 0;
            u32 Depth = 0;
            u32 Mips = 0;
            RendererAPI::TextureTargetType Target = RendererAPI::TextureTargetType::Texture2D;
        };

        // Reuse (or reallocate) scratch slot `slot` for the shape `source`
        // has. Returns a null handle when the source cannot be reproduced.
        // Reuse is keyed on the full storage description, so a resource that
        // changes shape between arms gets a fresh allocation rather than a
        // copy into a mismatched destination.
        [[nodiscard]] RHI::ResourceHandle AcquireScratch(sizet slot, RHI::ResourceHandle source,
                                                         const RHI::TextureFormatInfo& format, u32 width, u32 height,
                                                         u32 depthOrLayers,
                                                         RendererAPI::TextureTargetType target);

        // Clone one resolved source into scratch slot `slot`, filling `out`.
        void CaptureOne(sizet slot, const Request& request, Result& out);

        RenderGraph* m_InstalledGraph = nullptr;
        bool m_Pending = false;
        std::string m_PassName;
        std::vector<Request> m_Requests;
        std::vector<Result> m_Results;
        std::vector<ScratchSlot> m_Scratch;

        static constexpr const char* kPostPassHookKey = "mcp-afterpass-snapshot";
    };
} // namespace OloEngine
