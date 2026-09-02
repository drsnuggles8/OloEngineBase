#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <imgui.h>

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OloEngine
{
    class RenderGraph;

    // @brief Per-pass GPU snapshot capture for render-graph debugging.
    //
    // After each render pass executes, this class copies the current contents
    // of the SceneColor (and selected post-process / UI / final) framebuffers
    // into per-pass GL textures so the user can scrub through the pipeline
    // and see exactly which pass introduced a particular visual artifact
    // (ghosting, missing geometry, wrong tone-map, etc.).
    //
    // Usage (from RenderGraphDebugger):
    //   1. User clicks "Capture Frame" in the debugger panel.
    //   2. Debugger calls `frameCapture.RequestCapture()`.
    //   3. On the next render-graph execution the post-pass hook fires
    //      `OnPassExecuted()` after every pass — captures are stored.
    //   4. Debugger calls `GetCaptures()` to display the thumbnails.
    //
    // The capture uses GPU blits, so the CPU never reads full images back
    // (only a tiny 3x3 probe grid for diagnostics). Each capture texture is allocated once
    // per (passName + sourceLabel) at the SceneColor's current resolution.
    // Resolution changes invalidate stored captures.
    //
    // This is debug-only tooling — register the hook only when capture is
    // active to avoid the per-frame overhead.
    class RenderGraphFrameCapture
    {
      public:
        // What the capture entry represents — multiple per pass (e.g. scene
        // color + post-process color + final).
        enum class Source : u8
        {
            SceneColor = 0,        // Canonical SceneColor framebuffer resolved from the render graph
            GBufferAlbedo,         // Deferred G-Buffer RT0 (albedo + metallic)
            GBufferNormal,         // Deferred G-Buffer RT1 (normal + roughness + material AO)
            GBufferEmissive,       // Deferred G-Buffer RT2 (emissive + flags)
            GBufferBakedGI,        // Deferred G-Buffer RT5 (baked lightmap irradiance + coverage, issue #865)
            Velocity,              // Motion-vector buffer
            SceneNormals,          // Scene FB color attachment 2 (view-space normals, RG16F octahedral)
            HZBDepth,              // GTAO HZB texture (mip 0)
            SSSColor,              // SSS pass output (subsurface scattering blur), if active
            OITResolveColor,       // OIT resolve pass output, if active
            AOTexture,             // GTAO/SSAO output (R8 single channel, captured to RGBA8)
            AOApplyColor,          // AO-composited scene color
            BloomColor,            // Bloom composite output
            DOFColor,              // Depth-of-field output
            MotionBlurColor,       // Motion-blur output
            TAAColor,              // TAA resolve output
            CloudsColor,           // Cloudscape composite output -- the sky+deck the fog
                                   // pass composites OVER, and the only way to ask whether
                                   // there was anything behind the fog (issue #1008)
            PrecipitationColor,    // Screen-space precipitation output
            FogColor,              // Fog output
            ChromAbColor,          // Chromatic-aberration output
            ColorGradingColor,     // Color-grading output
            ToneMapColor,          // Tone-mapped output
            VignetteColor,         // Vignette output
            FXAAColor,             // FXAA output
            SelectionOutlineColor, // Selection-outline pass output
            UIComposite,           // UI composite pass output
            ColorBlindColor,       // Colour-vision adaptation output (issue #458), the last stage before present
            Backbuffer,            // Default framebuffer after FinalPass
            COUNT
        };

        struct CaptureEntry
        {
            std::string PassName;
            std::string ResourceName;
            Source SourceKind = Source::SceneColor;
            // Native GPU object names, deliberately (native-currency debug
            // info, ADR 0011 amendment (77)): TextureID feeds the debugger's
            // ImGui thumbnails as an ImTextureID, and the source ids exist
            // purely for diagnostics output. The capture path itself works in
            // RHI::ResourceHandle currency.
            u32 TextureID = 0; // native texture name of the RGBA8 capture
            u32 SourceTextureID = 0;
            u32 SourceFramebufferID = 0;
            u32 Width = 0;
            u32 Height = 0;
            u32 PassOrderIndex = std::numeric_limits<u32>::max();
            u32 CulledPassCount = 0;
            u32 PlannedBarrierCount = 0;
            u32 ResourceCount = 0;
            // Quick visibility diagnostics from a 3x3 probe grid over the
            // captured texture.
            u32 NonBlackSamples = 0;       // samples where any RGB channel > 0
            u32 NonTransparentSamples = 0; // samples where A > 0
            std::array<u8, 4> CenterRGBA{ 0, 0, 0, 0 };
        };

        RenderGraphFrameCapture() = default;
        ~RenderGraphFrameCapture();

        // No copy / move — owns GPU capture textures.
        RenderGraphFrameCapture(const RenderGraphFrameCapture&) = delete;
        RenderGraphFrameCapture& operator=(const RenderGraphFrameCapture&) = delete;

        // Arm capture for the next render-graph execution. The post-pass hook
        // must be registered on the graph (see InstallHook below).
        void RequestCapture()
        {
            m_PendingCapture = true;
        }

        // Returns true if a capture pass is in flight (between request and
        // first hook call) or has just completed.
        [[nodiscard]] bool HasCapture() const
        {
            return !m_Captures.empty();
        }

        // Installs the post-pass hook on the supplied render graph.
        // Pass nullptr to uninstall. Safe to call multiple times.
        void InstallHook(RenderGraph* graph);

        // Whether THIS tool's hook is installed on `graph`. The graph's own
        // HasPostPassHook() reports ANY listener (the MCP afterPass snapshot
        // registers one too, issue #607), so the debugger must ask here.
        [[nodiscard]] bool IsHookInstalled(const RenderGraph* graph) const
        {
            return graph != nullptr && m_InstalledGraph == graph;
        }

        // Hook entry-point — invoked from RenderGraph::Execute once per pass
        // after that pass returns. Public because the hook itself is a
        // std::function captured by InstallHook().
        void OnPassExecuted(const std::string& passName, RenderGraph& graph);

        // Drop all captured textures. Called automatically on resolution
        // change or capture restart.
        void ClearCaptures();

        [[nodiscard]] const std::vector<CaptureEntry>& GetCaptures() const
        {
            return m_Captures;
        }

        [[nodiscard]] static const char* SourceName(Source s);

      private:
        // Allocate (or recycle) an RGBA8 capture texture for (passName,
        // source) at (width, height). Returns its identity (RHI::NullResource
        // on failure). The texture is stored in m_TextureCache keyed on
        // (passName + source).
        [[nodiscard]] RHI::ResourceHandle AcquireTexture(const std::string& passName, Source source, u32 width, u32 height);

        struct GraphMetadata
        {
            u32 PassOrderIndex = std::numeric_limits<u32>::max();
            u32 CulledPassCount = 0;
            u32 PlannedBarrierCount = 0;
            u32 ResourceCount = 0;
        };

        // Copy a color-renderable texture into the per-pass texture.
        // `sourceFramebufferID` is the source FB's native name, carried purely
        // as diagnostic info for CaptureEntry / logging.
        void CaptureFramebuffer(const std::string& passName, Source source, RHI::ResourceHandle sourceTexture,
                                u32 width, u32 height,
                                std::string_view resourceName, u32 sourceFramebufferID, const GraphMetadata& metadata);

        // Copy the default framebuffer after FinalPass into the per-pass texture.
        void CaptureDefaultFramebuffer(const std::string& passName, Source source, u32 width, u32 height,
                                       std::string_view resourceName, const GraphMetadata& metadata);

        void RecordCapture(const std::string& passName, Source source, std::string_view resourceName,
                           RHI::ResourceHandle sourceTexture, u32 sourceFramebufferID, RHI::ResourceHandle dstTexture,
                           u32 width, u32 height, const GraphMetadata& metadata);

        struct CacheKey
        {
            std::string PassName;
            Source SourceKind = Source::SceneColor;

            bool operator==(const CacheKey& other) const noexcept
            {
                return SourceKind == other.SourceKind && PassName == other.PassName;
            }
        };

        struct CacheKeyHash
        {
            sizet operator()(const CacheKey& k) const noexcept
            {
                return std::hash<std::string>{}(k.PassName) ^ (static_cast<sizet>(std::to_underlying(k.SourceKind)) * 0x9e3779b97f4a7c15ULL);
            }
        };

        struct CachedTexture
        {
            RHI::ResourceHandle Texture;
            u32 Width = 0;
            u32 Height = 0;
        };

        bool m_PendingCapture = false;
        bool m_CapturingActive = false; // True while a frame's captures are being collected
        bool m_DiagLogged = false;      // True after the per-capture one-shot diagnostic line fires
        u32 m_FrameStartCaptureCount = 0;
        std::vector<CaptureEntry> m_Captures;
        std::unordered_map<CacheKey, CachedTexture, CacheKeyHash> m_TextureCache;
        std::unordered_set<std::string> m_PassesSeenThisCapture;

        RenderGraph* m_InstalledGraph = nullptr;

        // Key under which the hook registers on the graph's keyed post-pass
        // listener list (issue #607).
        static constexpr const char* kPostPassHookKey = "framecapture";
    };
} // namespace OloEngine
