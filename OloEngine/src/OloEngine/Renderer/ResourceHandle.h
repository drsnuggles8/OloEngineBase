#pragma once

#include "OloEngine/Core/Base.h"

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
    inline constexpr std::string_view ShadowMapPoint = "ShadowMapPoint";

    // Scene rendering outputs.
    inline constexpr std::string_view SceneColor = "SceneColor";     // HDR RGBA16F after main lighting
    inline constexpr std::string_view SceneDepth = "SceneDepth";     // Shared depth buffer
    inline constexpr std::string_view SceneNormals = "SceneNormals"; // GBuffer-style normals, sampled by SSAO/GTAO

    // Indirect occlusion outputs.
    inline constexpr std::string_view AOBuffer = "AOBuffer"; // Either SSAO or GTAO output

    // IBL resources (sampled read-only by Scene/PostProcess).
    inline constexpr std::string_view IrradianceMap = "IrradianceMap";
    inline constexpr std::string_view PrefilterMap = "PrefilterMap";
    inline constexpr std::string_view BrdfLut = "BrdfLut";

    // Post-process chain.
    inline constexpr std::string_view PostProcessColor = "PostProcessColor"; // Tonemapped/bloom/etc output
    inline constexpr std::string_view UIComposite = "UIComposite";           // UI composite over post-processed scene
    inline constexpr std::string_view FinalColor = "FinalColor";             // Final swapchain image
} // namespace OloEngine::ResourceNames
