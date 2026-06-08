#pragma once

#include "OloEngine/Core/Base.h"

#include <array>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

namespace OloEngine
{
    // @brief Opaque named handle for a GPU resource participating in the RenderGraph.
    //
    // Resources are identified by stable names — passes declare their reads
    // and writes with these handles, and the RenderGraph's hazard validator
    // (see RenderGraph::ValidateResourceHazards) enforces the contract that
    // every reader of a resource has a transitive execution dependency on
    // the resource's producer.
    //
    // Resources are typed (see `Kind`) so diagnostics and future transient
    // resource management can distinguish textures from buffers. The Kind
    // is informational today — validation is currently name-based — but the
    // type system is in place for the next iteration (automatic barriers,
    // transient memory aliasing, etc.).
    struct ResourceHandle
    {
        enum class Kind : u8
        {
            Unknown = 0,
            Texture2D,
            Texture2DArray,
            TextureCube,
            TextureCubeArray,
            Framebuffer,
            UniformBuffer,
            StorageBuffer,
        };

        std::string Name;
        Kind Type = Kind::Unknown;

        ResourceHandle() = default;
        ResourceHandle(std::string_view name, Kind type = Kind::Unknown)
            : Name(name), Type(type)
        {
        }

        [[nodiscard]] bool operator==(const ResourceHandle& other) const
        {
            // Name is the identity. Kind is metadata for diagnostics and
            // future barrier synthesis; two declarations of the same name
            // with different kinds are still the same resource (and should
            // be reported as a bug by a future stricter validator).
            return Name == other.Name;
        }
    };

    struct RGTextureHandle
    {
        static constexpr u32 InvalidIndex = std::numeric_limits<u32>::max();

        u32 Index = InvalidIndex;
        u32 Generation = 0;

        [[nodiscard]] auto IsValid() const -> bool
        {
            return Index != InvalidIndex && Generation > 0;
        }

        [[nodiscard]] auto operator==(const RGTextureHandle& other) const -> bool = default;
    };

    struct RGBufferHandle
    {
        static constexpr u32 InvalidIndex = std::numeric_limits<u32>::max();

        u32 Index = InvalidIndex;
        u32 Generation = 0;

        [[nodiscard]] auto IsValid() const -> bool
        {
            return Index != InvalidIndex && Generation > 0;
        }

        [[nodiscard]] auto operator==(const RGBufferHandle& other) const -> bool = default;
    };

    struct RGFramebufferHandle
    {
        static constexpr u32 InvalidIndex = std::numeric_limits<u32>::max();

        u32 Index = InvalidIndex;
        u32 Generation = 0;

        [[nodiscard]] auto IsValid() const -> bool
        {
            return Index != InvalidIndex && Generation > 0;
        }

        [[nodiscard]] auto operator==(const RGFramebufferHandle& other) const -> bool = default;
    };

    // API-neutral RenderGraph resource vocabulary. These enums intentionally
    // avoid backend-native types so the graph contract can lower to GL today
    // and Vulkan / D3D12 later without leaking API details through pass code.
    enum class RGResourceFormat : u16
    {
        Unknown = 0,
        R8UNorm,
        R32Float,
        RG16Float,
        RGBA8UNorm,
        RGBA16Float,
        RGBA32Float,
        Depth24Stencil8,
        Depth32Float,
        R32Int,
    };

    enum class RGLoadAction : u8
    {
        DontCare = 0,
        Load,
        Clear,
    };

    enum class RGStoreAction : u8
    {
        DontCare = 0,
        Store,
    };

    enum class RGAccessMode : u8
    {
        Unknown = 0,
        ShaderRead,
        ColorAttachmentRead,
        ColorAttachmentWrite,
        DepthStencilRead,
        DepthStencilWrite,
        StorageRead,
        StorageWrite,
        TransferRead,
        TransferWrite,
        UniformRead,
        IndirectRead,
        Present,
    };

    enum class RGQueueType : u8
    {
        Graphics = 0,
        Compute,
        Copy,
    };

    [[nodiscard]] inline auto ToString(ResourceHandle::Kind kind) -> std::string_view
    {
        switch (kind)
        {
            case ResourceHandle::Kind::Unknown:
                return "Unknown";
            case ResourceHandle::Kind::Texture2D:
                return "Texture2D";
            case ResourceHandle::Kind::Texture2DArray:
                return "Texture2DArray";
            case ResourceHandle::Kind::TextureCube:
                return "TextureCube";
            case ResourceHandle::Kind::TextureCubeArray:
                return "TextureCubeArray";
            case ResourceHandle::Kind::Framebuffer:
                return "Framebuffer";
            case ResourceHandle::Kind::UniformBuffer:
                return "UniformBuffer";
            case ResourceHandle::Kind::StorageBuffer:
                return "StorageBuffer";
        }

        return "Unknown";
    }

    struct RGResourceDesc
    {
        ResourceHandle::Kind Kind = ResourceHandle::Kind::Unknown;
        RGResourceFormat Format = RGResourceFormat::Unknown;
        RGLoadAction LoadAction = RGLoadAction::DontCare;
        RGStoreAction StoreAction = RGStoreAction::Store;
        RGQueueType Queue = RGQueueType::Graphics;
        u32 Width = 0;
        u32 Height = 0;
        u32 DepthOrLayers = 1;
        u32 MipLevels = 1;
        u32 Samples = 1;
        // For framebuffers with multiple render targets (MRT), list the format of each
        // colour attachment here in order (RT0, RT1, ...). When non-empty, `Format` is
        // ignored for the per-attachment layouts and is only retained for alias-group
        // keying and backward compatibility with single-attachment paths that do not
        // fill `Attachments`. A depth attachment, if required, should still be
        // represented as a FramebufferTextureFormat::Depth24Stencil8 entry appended
        // after the colour targets (matching FramebufferAttachmentSpecification order).
        std::vector<RGResourceFormat> Attachments;
        bool Imported = false;
        bool IsPlaceholder = false;
        std::string PlaceholderReason;
        std::string DebugName;

        [[nodiscard]] static auto FromHandleKind(ResourceHandle::Kind kind,
                                                 std::string_view debugName = {}) -> RGResourceDesc
        {
            RGResourceDesc desc;
            desc.Kind = kind;
            desc.DebugName = std::string(debugName);
            return desc;
        }

        [[nodiscard]] auto IsCompatibleWith(const RGResourceDesc& other) const -> bool
        {
            return Kind == other.Kind &&
                   Format == other.Format &&
                   Width == other.Width &&
                   Height == other.Height &&
                   DepthOrLayers == other.DepthOrLayers &&
                   MipLevels == other.MipLevels &&
                   Samples == other.Samples &&
                   Queue == other.Queue &&
                   Attachments == other.Attachments;
        }
    };
} // namespace OloEngine

namespace std
{
    template<>
    struct hash<OloEngine::ResourceHandle>
    {
        std::size_t operator()(const OloEngine::ResourceHandle& h) const noexcept
        {
            return std::hash<std::string>{}(h.Name);
        }
    };

    template<>
    struct hash<OloEngine::RGTextureHandle>
    {
        std::size_t operator()(const OloEngine::RGTextureHandle& h) const noexcept
        {
            auto key = (static_cast<u64>(h.Generation) << 32u) | static_cast<u64>(h.Index);
            return std::hash<u64>{}(key);
        }
    };

    template<>
    struct hash<OloEngine::RGBufferHandle>
    {
        std::size_t operator()(const OloEngine::RGBufferHandle& h) const noexcept
        {
            auto key = (static_cast<u64>(h.Generation) << 32u) | static_cast<u64>(h.Index);
            return std::hash<u64>{}(key);
        }
    };

    template<>
    struct hash<OloEngine::RGFramebufferHandle>
    {
        std::size_t operator()(const OloEngine::RGFramebufferHandle& h) const noexcept
        {
            auto key = (static_cast<u64>(h.Generation) << 32u) | static_cast<u64>(h.Index);
            return std::hash<u64>{}(key);
        }
    };
} // namespace std

namespace OloEngine::ResourceNames
{
    // =============================================================================
    // Canonical resource names used by production passes.
    //
    // Centralising the strings here means a typo in a pass declaration becomes
    // a linker-visible constant mismatch instead of a silently-ignored hazard.
    // Tests and passes MUST use these constants for all shared resources.
    // =============================================================================

    // Shadow maps written by ShadowRenderPass, sampled everywhere.
    inline constexpr std::string_view ShadowMapCSM = "ShadowMapCSM";
    inline constexpr std::string_view ShadowMapSpot = "ShadowMapSpot";
    inline constexpr std::string_view ShadowMapCSMCascade0 = "ShadowMapCSMCascade0";
    inline constexpr std::string_view ShadowMapCSMCascade1 = "ShadowMapCSMCascade1";
    inline constexpr std::string_view ShadowMapCSMCascade2 = "ShadowMapCSMCascade2";
    inline constexpr std::string_view ShadowMapCSMCascade3 = "ShadowMapCSMCascade3";
    inline constexpr std::array<std::string_view, 4> ShadowMapCSMCascade = {
        ShadowMapCSMCascade0, ShadowMapCSMCascade1, ShadowMapCSMCascade2, ShadowMapCSMCascade3
    };
    inline constexpr std::string_view ShadowMapSpotLayer0 = "ShadowMapSpotLayer0";
    inline constexpr std::string_view ShadowMapSpotLayer1 = "ShadowMapSpotLayer1";
    inline constexpr std::string_view ShadowMapSpotLayer2 = "ShadowMapSpotLayer2";
    inline constexpr std::string_view ShadowMapSpotLayer3 = "ShadowMapSpotLayer3";
    inline constexpr std::array<std::string_view, 4> ShadowMapSpotLayer = {
        ShadowMapSpotLayer0, ShadowMapSpotLayer1, ShadowMapSpotLayer2, ShadowMapSpotLayer3
    };
    // Point-light shadow cubemaps — one per light slot (max 4, matches UBOStructures::ShadowUBO::MAX_POINT_SHADOWS).
    inline constexpr std::string_view ShadowMapPoint0 = "ShadowMapPoint0";
    inline constexpr std::string_view ShadowMapPoint1 = "ShadowMapPoint1";
    inline constexpr std::string_view ShadowMapPoint2 = "ShadowMapPoint2";
    inline constexpr std::string_view ShadowMapPoint3 = "ShadowMapPoint3";
    // Convenience array indexed by light slot (0..3).
    inline constexpr std::array<std::string_view, 4> ShadowMapPoint = {
        ShadowMapPoint0, ShadowMapPoint1, ShadowMapPoint2, ShadowMapPoint3
    };

    // Scene rendering outputs.
    inline constexpr std::string_view SceneColor = "SceneColor";                     // HDR scene framebuffer (MRT root)
    inline constexpr std::string_view SceneColorTexture = "SceneColorTexture";       // Live SceneColor RT0 attachment view
    inline constexpr std::string_view SceneEntityID = "SceneEntityID";               // Live SceneColor entity-ID attachment view (RT1)
    inline constexpr std::string_view SceneViewNormals = "SceneViewNormals";         // Live SceneColor view-space normals attachment view (RT2)
    inline constexpr std::string_view SceneDepthAttachment = "SceneDepthAttachment"; // Live SceneColor depth attachment view
    inline constexpr std::string_view SceneDepth = "SceneDepth";                     // Semantic scene depth (forward snapshot or deferred G-Buffer depth)
    inline constexpr std::string_view SceneNormals = "SceneNormals";                 // Semantic AO/deferred normals input

    // G-Buffer attachments (deferred path).
    inline constexpr std::string_view GBufferResolved = "GBufferResolved"; // Internal single-sample deferred G-Buffer root backing attachment views (graph-declared transient with explicit frame-local backing)
    inline constexpr std::string_view GBufferMS = "GBufferMS";             // Internal multisample deferred G-Buffer root backing attachment views (graph-declared transient with explicit frame-local backing)
    inline constexpr std::string_view GBufferAlbedo = "GBufferAlbedo";     // RT0 — canonical single-sample albedo + metallic view (direct attachment or MSAA resolve view)
    inline constexpr std::string_view GBufferNormal = "GBufferNormal";     // RT1 — canonical single-sample normal + roughness + AO view (direct attachment or MSAA resolve view)
    inline constexpr std::string_view GBufferEmissive = "GBufferEmissive"; // RT2 — canonical single-sample emissive HDR view (direct attachment or MSAA resolve view)
    inline constexpr std::string_view Velocity = "Velocity";               // canonical single-sample motion vectors (direct attachment or MSAA resolve view)

    // Multisample deferred attachments (deferred + MSAA). When MSAA is active,
    // the canonical single-sample handles above are modeled as explicit
    // resolve views over these sources, while per-sample shading paths
    // (`DeferredLightingPass` MSAA branch) still sample these multisample
    // handles directly through `RGCommandContext::ResolveTexture`.
    inline constexpr std::string_view GBufferAlbedoMS = "GBufferAlbedoMS";
    inline constexpr std::string_view GBufferNormalMS = "GBufferNormalMS";
    inline constexpr std::string_view GBufferEmissiveMS = "GBufferEmissiveMS";
    inline constexpr std::string_view VelocityMS = "VelocityMS";
    inline constexpr std::string_view SceneDepthMS = "SceneDepthMS";

    // Indirect occlusion outputs.
    inline constexpr std::string_view AOBuffer = "AOBuffer";               // Either SSAO or GTAO output
    inline constexpr std::string_view SSAOBlur = "SSAOBlur";               // SSAO bilateral blur scratch framebuffer
    inline constexpr std::string_view GTAODenoisePing = "GTAODenoisePing"; // GTAO main output / denoise ping scratch
    inline constexpr std::string_view GTAODenoisePong = "GTAODenoisePong"; // GTAO denoise pong scratch
    inline constexpr std::string_view HZBDepth = "HZBDepth";               // GTAO hierarchical depth pyramid scratch (R32F mip chain)

    // IBL resources (sampled read-only by Scene/PostProcess).
    inline constexpr std::string_view IrradianceMap = "IrradianceMap";
    inline constexpr std::string_view PrefilterMap = "PrefilterMap";
    inline constexpr std::string_view BrdfLut = "BrdfLut";

    // Post-process chain.
    inline constexpr std::string_view SSSColor = "SSSColor";                                         // Full-resolution SSS output when the blur stage is enabled and ready
    inline constexpr std::string_view SSSColorTexture = "SSSColorTexture";                           // Color attachment view of SSSColor
    inline constexpr std::string_view AOApplyColor = "AOApplyColor";                                 // After AO apply (only valid when SSAO or GTAO is enabled)
    inline constexpr std::string_view AOApplyColorTexture = "AOApplyColorTexture";                   // Color attachment view of AOApplyColor
    inline constexpr std::string_view SSGIColor = "SSGIColor";                                       // After screen-space GI composite (only valid when SSGI is enabled, deferred path)
    inline constexpr std::string_view SSGIColorTexture = "SSGIColorTexture";                         // Color attachment view of SSGIColor
    inline constexpr std::string_view SSRColor = "SSRColor";                                         // After screen-space reflections composite (only valid when SSR is enabled, deferred path)
    inline constexpr std::string_view SSRColorTexture = "SSRColorTexture";                           // Color attachment view of SSRColor
    inline constexpr std::string_view PostProcessColor = "PostProcessColor";                         // Alias for the latest upstream full-resolution post-chain source (SSR, AOApply, SSS, or SceneColor)
    inline constexpr std::string_view PostProcessColorTexture = "PostProcessColorTexture";           // Color attachment view alias matching PostProcessColor
    inline constexpr std::string_view BloomColor = "BloomColor";                                     // After Bloom composite (only valid when Bloom is enabled)
    inline constexpr std::string_view BloomColorTexture = "BloomColorTexture";                       // Color attachment view of BloomColor
    inline constexpr std::string_view DOFColor = "DOFColor";                                         // After depth-of-field (only valid when DOF is enabled)
    inline constexpr std::string_view DOFColorTexture = "DOFColorTexture";                           // Color attachment view of DOFColor
    inline constexpr std::string_view MotionBlurColor = "MotionBlurColor";                           // After motion blur (only valid when motion blur is enabled)
    inline constexpr std::string_view MotionBlurColorTexture = "MotionBlurColorTexture";             // Color attachment view of MotionBlurColor
    inline constexpr std::string_view TAAColor = "TAAColor";                                         // After temporal AA resolve (only valid when TAA is enabled)
    inline constexpr std::string_view TAAColorTexture = "TAAColorTexture";                           // Color attachment view of TAAColor
    inline constexpr std::string_view PrecipitationColor = "PrecipitationColor";                     // After screen-space precipitation overlay (only valid when precipitation screen FX enabled)
    inline constexpr std::string_view PrecipitationColorTexture = "PrecipitationColorTexture";       // Color attachment view of PrecipitationColor
    inline constexpr std::string_view FogColor = "FogColor";                                         // After volumetric fog composite (only valid when fog is enabled)
    inline constexpr std::string_view FogColorTexture = "FogColorTexture";                           // Color attachment view of FogColor
    inline constexpr std::string_view FogHalfRes = "FogHalfRes";                                     // Half-resolution volumetric fog integration scratch
    inline constexpr std::string_view ChromAbColor = "ChromAbColor";                                 // After chromatic aberration
    inline constexpr std::string_view ChromAbColorTexture = "ChromAbColorTexture";                   // Color attachment view of ChromAbColor
    inline constexpr std::string_view ColorGradingColor = "ColorGradingColor";                       // After colour grading
    inline constexpr std::string_view ColorGradingColorTexture = "ColorGradingColorTexture";         // Color attachment view of ColorGradingColor
    inline constexpr std::string_view ToneMapColor = "ToneMapColor";                                 // After tone mapping (HDR→LDR boundary)
    inline constexpr std::string_view ToneMapColorTexture = "ToneMapColorTexture";                   // Color attachment view of ToneMapColor
    inline constexpr std::string_view VignetteColor = "VignetteColor";                               // After vignette
    inline constexpr std::string_view VignetteColorTexture = "VignetteColorTexture";                 // Color attachment view of VignetteColor
    inline constexpr std::string_view FXAAColor = "FXAAColor";                                       // Anti-aliased post-process output
    inline constexpr std::string_view FXAAColorTexture = "FXAAColorTexture";                         // Color attachment view of FXAAColor
    inline constexpr std::string_view SelectionOutlineColor = "SelectionOutlineColor";               // Post-process + selection outline composite
    inline constexpr std::string_view SelectionOutlineColorTexture = "SelectionOutlineColorTexture"; // Color attachment view of SelectionOutlineColor
    inline constexpr std::string_view UIComposite = "UIComposite";                                   // UI composite over post-processed scene
    inline constexpr std::string_view UICompositeTexture = "UICompositeTexture";                     // Color attachment view of UIComposite RT0
    inline constexpr std::string_view Backbuffer = "Backbuffer";                                     // External present target (default framebuffer / swapchain)

    // Temporal histories — imported each frame from the previous frame's output,
    // consumed by TAA (TAAHistory) and volumetric fog (FogHistory).
    inline constexpr std::string_view TAAHistory = "TAAHistory"; // Previous TAA accumulation buffer
    inline constexpr std::string_view FogHistory = "FogHistory"; // Previous volumetric fog integration

    // Weighted-blended OIT accumulation targets (particles and forward
    // transparent decals write these; OITResolvePass reads them and
    // composites back onto SceneColor). Modelled as separate resources so
    // the L5 hazard validator can catch a missing contributor ->
    // OITResolve handoff on OITAccum / OITRevealage instead of falling back
    // to the older "both write SceneColor" approximation.
    // `OITBuffer` is the shared transient MRT framebuffer that backs both
    // OITAccum (RT0 = RGBA16F) and OITRevealage (RT1 = RG16F), plus a
    // graph-owned DEPTH24_STENCIL8 attachment seeded from SceneColor before
    // transparent contributors execute. `OITAccum` / `OITRevealage` are now
    // texture/depth attachment views derived from that framebuffer, not
    // duplicated framebuffer handles.
    inline constexpr std::string_view OITBuffer = "OITBuffer";                   // Transient MRT FB (RT0=RGBA16F accum, RT1=RG16F revealage, D=DEPTH24_STENCIL8)
    inline constexpr std::string_view OITAccum = "OITAccum";                     // RGBA16F accumulation attachment
    inline constexpr std::string_view OITRevealage = "OITRevealage";             // RG16F revealage attachment
    inline constexpr std::string_view OITDepthAttachment = "OITDepthAttachment"; // DEPTH24_STENCIL8 depth attachment view
} // namespace OloEngine::ResourceNames
