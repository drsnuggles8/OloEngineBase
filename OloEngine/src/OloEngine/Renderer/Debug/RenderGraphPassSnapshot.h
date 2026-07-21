#pragma once

#include "OloEngine/Core/Base.h"

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
    // (glCopyImageSubData — every mip and layer, identical internal format)
    // into a scratch texture owned here. Capture/probe/stats/compare tools
    // then read the scratches instead of the live resources. Multiple
    // resources are cloned in the SAME hook firing so a bitwise compare of
    // two targets (olo_render_validate) sees one consistent frame.
    //
    // The copy must happen AT HOOK TIME, not by remembering the GL id: the
    // transient pool memory-aliases same-descriptor resources, so a mid-frame
    // id may be recycled for a different logical resource later in the frame.
    //
    // Threading: Arm/Disarm/GetResults run on the main thread (the MCP server
    // marshals them there); the hook fires on the main thread inside
    // RenderGraph::Execute(). GL state hygiene: glCopyImageSubData binds
    // nothing — no FBO, program, sampler, or UBO state is touched, so the
    // engine-global publications the render-pass-published-state rules
    // protect are unaffected.
    class RenderGraphPassSnapshot
    {
      public:
        // Resolves a source GL texture id at hook time (main thread,
        // mid-frame). Returning 0 records a resolve failure in the result.
        using Resolver = std::function<u32()>;

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
            u32 TextureID = 0; // the scratch clone (owned by this class)
            u32 SourceTextureID = 0;
            u32 Width = 0;
            u32 Height = 0;
            u32 DepthOrLayers = 1;
            u32 MipLevels = 1;
            u32 GLInternalFormat = 0;
            u32 GLTarget = 0;
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

        // Free all scratch GL textures. Call at renderer shutdown (needs a
        // live GL context) — the destructor deliberately does NOT (the
        // instance is a process-static outliving the GL context).
        void ReleaseScratch();

        // Hook entry point — public because the std::function installed by
        // Arm() captures `this` and forwards here.
        void OnPassExecuted(const std::string& passName, RenderGraph& graph);

      private:
        struct ScratchSlot
        {
            u32 Texture = 0;
            u32 Target = 0;
            u32 Format = 0;
            u32 Width = 0;
            u32 Height = 0;
            u32 Depth = 0;
            u32 Mips = 0;
        };

        // Reuse (or reallocate) scratch slot `slot` for the given shape.
        // Returns 0 when the target/format cannot be allocated.
        [[nodiscard]] u32 AcquireScratch(sizet slot, u32 glTarget, u32 glInternalFormat, u32 width, u32 height,
                                         u32 depthOrLayers, u32 mipLevels);

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
