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
        RG16Float,
        RGBA8UNorm,
        RGBA16Float,
        Depth24Stencil8,
        Depth32Float,
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
        bool Imported = false;
        std::string DebugName;

        [[nodiscard]] static auto FromLegacy(ResourceHandle::Kind kind,
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
                   Queue == other.Queue;
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
    inline constexpr std::string_view SceneColor = "SceneColor";     // HDR RGBA16F after main lighting
    inline constexpr std::string_view SceneDepth = "SceneDepth";     // Shared depth buffer
    inline constexpr std::string_view SceneNormals = "SceneNormals"; // GBuffer-style normals, sampled by SSAO/GTAO

    // G-Buffer attachments (deferred path).
    inline constexpr std::string_view GBufferAlbedo = "GBufferAlbedo";     // RT0 — sRGB albedo + metallic
    inline constexpr std::string_view GBufferNormal = "GBufferNormal";     // RT1 — octahedral normal + roughness + AO
    inline constexpr std::string_view GBufferEmissive = "GBufferEmissive"; // RT2 — emissive HDR (RT3 velocity exposed separately)
    inline constexpr std::string_view Velocity = "Velocity";               // TAA motion vectors (screen-space)

    // Multisample companion attachments (deferred + MSAA). Same backing
    // memory as the resolved attachments above but exposed to the graph
    // as separate handles so per-sample shading paths
    // (`DeferredLightingPass` MSAA branch) can resolve them through
    // `RGCommandContext::ResolveTexture`.
    inline constexpr std::string_view GBufferAlbedoMS = "GBufferAlbedoMS";
    inline constexpr std::string_view GBufferNormalMS = "GBufferNormalMS";
    inline constexpr std::string_view GBufferEmissiveMS = "GBufferEmissiveMS";
    inline constexpr std::string_view VelocityMS = "VelocityMS";
    inline constexpr std::string_view SceneDepthMS = "SceneDepthMS";

    // Indirect occlusion outputs.
    inline constexpr std::string_view AOBuffer = "AOBuffer"; // Either SSAO or GTAO output

    // IBL resources (sampled read-only by Scene/PostProcess).
    inline constexpr std::string_view IrradianceMap = "IrradianceMap";
    inline constexpr std::string_view PrefilterMap = "PrefilterMap";
    inline constexpr std::string_view BrdfLut = "BrdfLut";

    // Post-process chain.
    inline constexpr std::string_view SSSColor = "SSSColor";                           // Output of SSS stage (or passthrough scene color)
    inline constexpr std::string_view AOApplyColor = "AOApplyColor";                   // After AO apply (only valid when SSAO or GTAO is enabled)
    inline constexpr std::string_view PostProcessColor = "PostProcessColor";           // Dynamic post-chain input (typically AOApply/SSS/Scene fallback)
    inline constexpr std::string_view BloomColor = "BloomColor";                       // After Bloom composite (only valid when Bloom is enabled)
    inline constexpr std::string_view DOFColor = "DOFColor";                           // After depth-of-field (only valid when DOF is enabled)
    inline constexpr std::string_view MotionBlurColor = "MotionBlurColor";             // After motion blur (only valid when motion blur is enabled)
    inline constexpr std::string_view TAAColor = "TAAColor";                           // After temporal AA resolve (only valid when TAA is enabled)
    inline constexpr std::string_view PrecipitationColor = "PrecipitationColor";       // After screen-space precipitation overlay (only valid when precipitation screen FX enabled)
    inline constexpr std::string_view FogColor = "FogColor";                           // After volumetric fog composite (only valid when fog is enabled)
    inline constexpr std::string_view ChromAbColor = "ChromAbColor";                   // After chromatic aberration
    inline constexpr std::string_view ColorGradingColor = "ColorGradingColor";         // After colour grading
    inline constexpr std::string_view ToneMapColor = "ToneMapColor";                   // After tone mapping (HDR→LDR boundary)
    inline constexpr std::string_view VignetteColor = "VignetteColor";                 // After vignette
    inline constexpr std::string_view FXAAColor = "FXAAColor";                         // Anti-aliased post-process output
    inline constexpr std::string_view SelectionOutlineColor = "SelectionOutlineColor"; // Post-process + selection outline composite
    inline constexpr std::string_view UIComposite = "UIComposite";                     // UI composite over post-processed scene
    inline constexpr std::string_view Backbuffer = "Backbuffer";                       // External present target (default framebuffer / swapchain)

    // Temporal histories — imported each frame from the previous frame's output,
    // consumed by TAA (TAAHistory) and volumetric fog (FogHistory).
    inline constexpr std::string_view TAAHistory = "TAAHistory"; // Previous TAA accumulation buffer
    inline constexpr std::string_view FogHistory = "FogHistory"; // Previous volumetric fog integration

    // Weighted-blended OIT accumulation targets (Water / Particle OIT modes
    // write these; OITResolvePass reads them and composites back onto
    // SceneColor). Modelled as separate resources so the L5 hazard
    // validator can catch a missing Water -> OITResolve handoff (a RAW on
    // OITAccum rather than the older "both write SceneColor" approximation,
    // which let the real bug slip through).
    inline constexpr std::string_view OITAccum = "OITAccum";         // RGBA16F accumulation attachment
    inline constexpr std::string_view OITRevealage = "OITRevealage"; // RG16F revealage attachment
} // namespace OloEngine::ResourceNames
