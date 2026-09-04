#pragma once

// =============================================================================
// RayTracingTypes.h — the backend-neutral vocabulary of the ray-tracing scene.
//
// Issue #978. This header is API-neutral and stays that way: no volk, no
// Vulkan types, no GL. It is the half of the RT subsystem that CI can compile
// and test on a machine with no GPU at all, which is most of the policy —
// geometry classification, the refit-vs-rebuild heuristic, instance packing
// limits, the stale-record guard, and every telemetry counter.
//
// The device half lives in Platform/Vulkan/VulkanRayTracingBackend.{h,cpp}
// behind IRayTracingBackend (RayTracingScene.h), the same shape
// RHI::DescriptorHeap uses for IDescriptorHeapBackend.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <string_view>

namespace OloEngine::RayTracing
{
    // -------------------------------------------------------------------------
    // Capability
    // -------------------------------------------------------------------------

    // Why hardware ray tracing is unavailable. The issue requires that an
    // unsupported device "reports the capability reason" rather than silently
    // producing an incomplete RT scene, so the reason is part of the capability
    // value, not a log line that scrolls away.
    //
    // Ordered most-fundamental first: a device that fails an earlier row cannot
    // be asked about a later one.
    enum class UnsupportedReason : u32
    {
        None = 0,           ///< Supported. Nothing is wrong.
        BackendNotVulkan,   ///< The OpenGL backend has no hardware RT path at all.
        NoDevice,           ///< Vulkan selected but no logical device is up (headless / teardown).
        ExtensionMissing,   ///< VK_KHR_acceleration_structure and/or VK_KHR_ray_query not exposed.
        FeatureUnsupported, ///< Extension listed, but its feature bit is VK_FALSE.
        EntryPointMissing,  ///< Feature enabled, but volk left the entry point null (loader/ICD gap).
        DisabledByLever,    ///< OLO_VULKAN_NO_RAY_TRACING=1 forced it off for an A/B.
    };

    [[nodiscard]] constexpr std::string_view ToString(UnsupportedReason reason)
    {
        switch (reason)
        {
            case UnsupportedReason::None:
                return "supported";
            case UnsupportedReason::BackendNotVulkan:
                return "the active RHI backend is not Vulkan (hardware ray tracing is Vulkan-only here)";
            case UnsupportedReason::NoDevice:
                return "no Vulkan logical device is available";
            case UnsupportedReason::ExtensionMissing:
                return "VK_KHR_acceleration_structure / VK_KHR_ray_query are not exposed by this device";
            case UnsupportedReason::FeatureUnsupported:
                return "the ray-tracing extensions are present but their feature bits are unsupported";
            case UnsupportedReason::EntryPointMissing:
                return "the ray-tracing entry points were not exported by the loader/ICD";
            case UnsupportedReason::DisabledByLever:
                return "disabled by OLO_VULKAN_NO_RAY_TRACING=1";
        }
        return "unknown";
    }

    // Device properties the AS lifecycle needs. Captured ONCE, from
    // VkPhysicalDeviceAccelerationStructurePropertiesKHR and
    // VkPhysicalDeviceRayTracingPipelinePropertiesKHR, at logical-device
    // creation — never re-queried at a use site.
    //
    // Every field is a hard limit or an alignment the builder must honour; a
    // zero here means "not captured", which is why the capability's Supported
    // flag and these values are published together as one value.
    struct DeviceProperties
    {
        // VkPhysicalDeviceAccelerationStructurePropertiesKHR
        u64 MaxGeometryCount = 0;          ///< Geometries in one BLAS build.
        u64 MaxInstanceCount = 0;          ///< Instances in one TLAS build.
        u64 MaxPrimitiveCount = 0;         ///< Triangles across one build.
        u32 MinScratchOffsetAlignment = 0; ///< minAccelerationStructureScratchOffsetAlignment.

        // VkPhysicalDeviceRayTracingPipelinePropertiesKHR. Zero when the
        // ray-tracing-PIPELINE extension is absent — ray query alone does not
        // need an SBT, and this subsystem is usable without one.
        u32 ShaderGroupHandleSize = 0;
        u32 ShaderGroupBaseAlignment = 0;
        u32 ShaderGroupHandleAlignment = 0;

        // VkPhysicalDeviceAccelerationStructureFeaturesKHR, the two bits whose
        // absence changes what the builder may DO rather than whether it runs.
        bool SupportsHostCommands = false;  ///< accelerationStructureHostCommands (false on NVIDIA).
        bool SupportsIndirectBuild = false; ///< accelerationStructureIndirectBuild (false on NVIDIA).

        [[nodiscard]] auto operator==(const DeviceProperties&) const -> bool = default;
    };

    // The single capability value. ONE predicate, one owner
    // (rhi-abstraction-boundary.md §13c): every consumer asks
    // RenderCommand::GetRayTracingCapabilities(), and the bool predicate
    // RenderCommand::SupportsRayTracing() forwards to this struct's Supported
    // rather than re-deriving the test.
    struct Capabilities
    {
        bool Supported = false;          ///< Ray QUERY is usable right now. The gate.
        bool RayTracingPipeline = false; ///< VK_KHR_ray_tracing_pipeline also enabled (SBT path).
        UnsupportedReason Reason = UnsupportedReason::BackendNotVulkan;
        DeviceProperties Properties{};

        [[nodiscard]] std::string_view ReasonText() const
        {
            return ToString(Reason);
        }

        [[nodiscard]] auto operator==(const Capabilities&) const -> bool = default;
    };

    // -------------------------------------------------------------------------
    // Geometry classes
    // -------------------------------------------------------------------------

    // The issue's five explicit classes. This is the class a BLAS is COUNTED
    // and REPORTED as; what it implies for updates is a separate question
    // answered by UpdatePolicyFor below, because "masked" says something about
    // opacity while "rigid dynamic" says something about motion, and a masked
    // rigid mesh is both.
    //
    // Classification is a total order over the record: the first row that
    // matches wins, most-restrictive first.
    enum class GeometryClass : u32
    {
        // The record cannot be traced at all — no device address, an unknown
        // vertex/index format, a degenerate triangle count, or a geometry the
        // canonical scene never produces (skinned, cloth, virtualized clusters,
        // particles, fluids). Counted and reported; the raster fallback stays
        // visible. This is a real, expected population, not an error bucket.
        Unsupported = 0,

        // Vertices move every frame. Refit while the refit budget and the
        // documented quality heuristic allow it; rebuild otherwise.
        Deformed,

        // Alpha-tested. Its TLAS INSTANCE carries FORCE_NO_OPAQUE, so a ray
        // query stops on it as a CANDIDATE and the shared confirmation helper
        // decides. Deliberately an instance flag rather than a BLAS geometry
        // flag: alpha-testing is a property of the MATERIAL, and one mesh can
        // be an opaque wall for one entity and a cutout for another, so baking
        // it into the shared structure would force two BLASes for one mesh.
        Masked,

        // The BLAS is built once and never rebuilt; the object moves by
        // rewriting its TLAS instance transform only.
        RigidDynamic,

        // Built once, compacted, and its source build storage retired.
        Static,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GeometryClass geometryClass)
    {
        switch (geometryClass)
        {
            case GeometryClass::Unsupported:
                return "Unsupported";
            case GeometryClass::Deformed:
                return "Deformed";
            case GeometryClass::Masked:
                return "Masked";
            case GeometryClass::RigidDynamic:
                return "RigidDynamic";
            case GeometryClass::Static:
                return "Static";
            case GeometryClass::Count:
                break;
        }
        return "Unknown";
    }

    // What a class implies for the BLAS itself, independent of how the object
    // moves through the world. Deliberately three values, not five: the whole
    // point of the split is that Masked and RigidDynamic differ from Static in
    // their FLAGS and their TLAS handling, not in whether the BLAS is rebuilt.
    enum class UpdatePolicy : u32
    {
        Never,          ///< No BLAS exists. Unsupported only.
        BuildOnce,      ///< Build, compact, then never touch the BLAS again.
        RefitOrRebuild, ///< Vertices move: refit while legal, rebuild past the heuristic.
    };

    [[nodiscard]] constexpr UpdatePolicy UpdatePolicyFor(GeometryClass geometryClass)
    {
        switch (geometryClass)
        {
            case GeometryClass::Unsupported:
                return UpdatePolicy::Never;
            case GeometryClass::Deformed:
                return UpdatePolicy::RefitOrRebuild;
            case GeometryClass::Masked:
            case GeometryClass::RigidDynamic:
            case GeometryClass::Static:
                return UpdatePolicy::BuildOnce;
            case GeometryClass::Count:
                break;
        }
        return UpdatePolicy::Never;
    }

    // True when the class must be traced as a candidate rather than committed
    // on intersection — i.e. when the BLAS geometry may not be flagged opaque.
    [[nodiscard]] constexpr bool RequiresCandidateConfirmation(GeometryClass geometryClass)
    {
        return geometryClass == GeometryClass::Masked;
    }

    // Only a class that compacts retires its source build storage. Deformed
    // geometry must stay refittable, and a compacted BLAS cannot be refitted
    // (VUID-vkCmdBuildAccelerationStructuresKHR-mode-04663 territory: a
    // compaction copy drops the ALLOW_UPDATE-ness the refit needs), so the two
    // are mutually exclusive by construction rather than by convention.
    [[nodiscard]] constexpr bool AllowsCompaction(GeometryClass geometryClass)
    {
        return UpdatePolicyFor(geometryClass) == UpdatePolicy::BuildOnce;
    }

    // -------------------------------------------------------------------------
    // Why a build happened
    // -------------------------------------------------------------------------

    // Telemetry asks for a "rebuild reason". A build with no reason is a bug,
    // so the enum has no None row that a counter could quietly accumulate.
    enum class BuildReason : u32
    {
        FirstBuild = 0,      ///< The record had no BLAS yet.
        GeometryChanged,     ///< The GPU Scene geometry record's bytes moved (new buffers, new range).
        DeformedRefit,       ///< A refit of an existing Deformed BLAS.
        DeformedRefitBudget, ///< Refit was legal but the refit-run heuristic forced a rebuild.
        ClassChanged,        ///< The record's GeometryClass changed under a stable identity.
        Compaction,          ///< The compaction copy itself.
        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(BuildReason reason)
    {
        switch (reason)
        {
            case BuildReason::FirstBuild:
                return "FirstBuild";
            case BuildReason::GeometryChanged:
                return "GeometryChanged";
            case BuildReason::DeformedRefit:
                return "DeformedRefit";
            case BuildReason::DeformedRefitBudget:
                return "DeformedRefitBudget";
            case BuildReason::ClassChanged:
                return "ClassChanged";
            case BuildReason::Compaction:
                return "Compaction";
            case BuildReason::Count:
                break;
        }
        return "Unknown";
    }

    // Why the TLAS was rebuilt rather than updated. Same no-None rule.
    enum class TlasBuildReason : u32
    {
        FirstBuild = 0,      ///< No TLAS existed.
        InstanceCountGrew,   ///< More instances than the current build was sized for.
        InstanceCountShrank, ///< Enough instances left that a rebuild is cheaper than a sparse update.
        TopologyChanged,     ///< An instance's BLAS identity changed — a refit cannot express that.
        RenderOriginRebased, ///< Camera-relative origin moved; every transform changed at once.
        Update,              ///< Not a rebuild: an ordinary transform-only refit.
        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(TlasBuildReason reason)
    {
        switch (reason)
        {
            case TlasBuildReason::FirstBuild:
                return "FirstBuild";
            case TlasBuildReason::InstanceCountGrew:
                return "InstanceCountGrew";
            case TlasBuildReason::InstanceCountShrank:
                return "InstanceCountShrank";
            case TlasBuildReason::TopologyChanged:
                return "TopologyChanged";
            case TlasBuildReason::RenderOriginRebased:
                return "RenderOriginRebased";
            case TlasBuildReason::Update:
                return "Update";
            case TlasBuildReason::Count:
                break;
        }
        return "Unknown";
    }

    // -------------------------------------------------------------------------
    // Hard limits imposed by VkAccelerationStructureInstanceKHR itself
    // -------------------------------------------------------------------------

    // The TLAS instance record is a packed struct with two bitfields far
    // narrower than the GPU Scene lanes that feed them. Straight assignment
    // silently truncates, which is a wrong-hit rather than an error, so both
    // conversions go through a checked helper.
    inline constexpr u32 kMaxInstanceCustomIndex = (1u << 24) - 1u; ///< instanceCustomIndex is 24 bits.
    inline constexpr u32 kInstanceMaskBits = 8u;                    ///< mask is 8 bits.
    inline constexpr u32 kInstanceMaskAll = 0xFFu;

    // GPU Scene's VisibilityMask is a u32; the RT instance mask is 8 bits.
    // Fold the high bits down rather than truncating them away, so an effect
    // that only ever sets a high bit is not silently invisible to every ray.
    // The all-ones input (GPU Scene's default) maps to all-ones out.
    [[nodiscard]] constexpr u8 PackInstanceMask(u32 visibilityMask)
    {
        const u32 folded = (visibilityMask & 0xFFu) | ((visibilityMask >> 8) & 0xFFu) |
                           ((visibilityMask >> 16) & 0xFFu) | ((visibilityMask >> 24) & 0xFFu);
        // A zero mask makes an instance unhittable by every ray, which is
        // never what "no visibility bits were set" is meant to express here —
        // GPU Scene's own default is all-ones and nothing sets it otherwise
        // today. Preserve a deliberate zero only when the input really was 0.
        return static_cast<u8>(folded);
    }

    // True when a GPU Scene slot index still fits instanceCustomIndex. A scene
    // past 16.7M instance slots cannot round-trip its identity through the TLAS,
    // and the honest answer is to count the overflow as Unsupported rather than
    // to hand the shader a wrapped index that resolves to the wrong material.
    [[nodiscard]] constexpr bool FitsInstanceCustomIndex(u32 slotIndex)
    {
        return slotIndex <= kMaxInstanceCustomIndex;
    }
} // namespace OloEngine::RayTracing
