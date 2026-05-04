#pragma once

#include "OloEngine/Core/Base.h"

#include <glad/gl.h>
#include <imgui.h>

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
            SceneColor = 0,        // Live HDR scene framebuffer (Renderer3D::GetScenePass()->GetTarget())
            GBufferAlbedo,         // Deferred G-Buffer RT0 (albedo + metallic)
            GBufferNormal,         // Deferred G-Buffer RT1 (normal + roughness + material AO)
            GBufferEmissive,       // Deferred G-Buffer RT2 (emissive + flags)
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
            PrecipitationColor,    // Screen-space precipitation output
            FogColor,              // Fog output
            ChromAbColor,          // Chromatic-aberration output
            ColorGradingColor,     // Color-grading output
            ToneMapColor,          // Tone-mapped output
            VignetteColor,         // Vignette output
            FXAAColor,             // FXAA output
            SelectionOutlineColor, // Selection-outline pass output
            UIComposite,           // UI composite pass output
            Backbuffer,            // Default framebuffer after FinalPass
            COUNT
        };

        struct CaptureEntry
        {
            std::string PassName;
            std::string ResourceName;
            Source SourceKind = Source::SceneColor;
            u32 TextureID = 0; // GL texture name (RGBA8)
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

        // No copy / move — owns GL handles.
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
        // Allocate (or recycle) a GL texture for (passName, source) at
        // (width, height). Returns the GL handle. The texture is stored
        // in m_TextureCache keyed on (passName + source).
        [[nodiscard]] u32 AcquireTexture(const std::string& passName, Source source, u32 width, u32 height);

        struct GraphMetadata
        {
            u32 PassOrderIndex = std::numeric_limits<u32>::max();
            u32 CulledPassCount = 0;
            u32 PlannedBarrierCount = 0;
            u32 ResourceCount = 0;
        };

        // Copy a color-renderable texture into the per-pass texture.
        void CaptureFramebuffer(const std::string& passName, Source source, u32 sourceTextureID, u32 width, u32 height,
                                std::string_view resourceName, u32 sourceFramebufferID, const GraphMetadata& metadata);

        // Copy the default framebuffer after FinalPass into the per-pass texture.
        void CaptureDefaultFramebuffer(const std::string& passName, Source source, u32 width, u32 height,
                                       std::string_view resourceName, const GraphMetadata& metadata);

        void RecordCapture(const std::string& passName, Source source, std::string_view resourceName,
                           u32 sourceTextureID, u32 sourceFramebufferID, u32 dstTexture,
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
                return std::hash<std::string>{}(k.PassName) ^ (static_cast<sizet>(k.SourceKind) * 0x9e3779b97f4a7c15ULL);
            }
        };

        struct CachedTexture
        {
            u32 TextureID = 0;
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
    };
} // namespace OloEngine
