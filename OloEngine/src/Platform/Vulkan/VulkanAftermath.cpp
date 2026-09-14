#include "OloEnginePCH.h"

#include "Platform/Vulkan/VulkanAftermath.h"

#include "OloEngine/Core/DebugLevers.h"

#if OLO_WITH_VULKAN
// volk owns the Vulkan function pointers and the VK_NV_device_diagnostics_config
// enums this file names (ADR 0011 amendment 41a: never <vulkan/vulkan.h>).
#include "Platform/Vulkan/VulkanDevice.h"
#endif

#if OLO_WITH_AFTERMATH

#include <GFSDK_Aftermath.h>
#include <GFSDK_Aftermath_GpuCrashDump.h>
#include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <unordered_map>
#include <string>
#include <mutex>

#endif

namespace OloEngine::VulkanAftermath
{
#if OLO_WITH_AFTERMATH
    namespace
    {
        std::atomic<bool> s_Initialized{ false };
        std::atomic<bool> s_DumpReceived{ false };

        // Shader-binary hash -> our own name for it. Written while shaders are
        // created (any thread), read once from the crash-dump callback.
        // Aftermath invokes its callbacks from driver threads and does not
        // serialize them; two concurrent dumps would race the same filename and
        // interleave their decode output.
        std::mutex s_CrashDumpMutex;
        std::mutex s_ShaderNameMutex;
        std::unordered_map<u64, std::string> s_ShaderNames;

        [[nodiscard]] bool Requested()
        {
            // Latched once: the driver reads Aftermath's state at device
            // creation, so a value that changed later would describe a device
            // that was not created that way.
            static const bool s_On = Levers::VulkanAftermathCrashDumps();
            return s_On;
        }

        [[nodiscard]] const char* ResidencyName(const GFSDK_Aftermath_ResourceResidency residency)
        {
            switch (residency)
            {
                case GFSDK_Aftermath_ResourceResidency_FullyResident:
                    return "fully resident";
                case GFSDK_Aftermath_ResourceResidency_Evicted:
                    return "evicted";
                case GFSDK_Aftermath_ResourceResidency_MemoryFreed:
                    return "MEMORY FREED";
                case GFSDK_Aftermath_ResourceResidency_MemoryUnbound:
                    return "MEMORY UNBOUND";
                default:
                    return "unknown";
            }
        }

        [[nodiscard]] const char* AccessTypeName(const GFSDK_Aftermath_AccessType access)
        {
            switch (access)
            {
                case GFSDK_Aftermath_AccessType_Read:
                    return "READ";
                case GFSDK_Aftermath_AccessType_Write:
                    return "WRITE";
                case GFSDK_Aftermath_AccessType_Atomic:
                    return "ATOMIC";
                default:
                    return "unknown access";
            }
        }

        // Decode the dump in-process rather than only writing the .nv-gpudmp.
        // A file needs Nsight Graphics and a human; the interesting fields are
        // three calls away and belong in the log next to the fault report that
        // prompted them.
        void DecodeAndLog(const void* dump, const u32 dumpSize)
        {
            GFSDK_Aftermath_GpuCrashDump_Decoder decoder = {};
            if (GFSDK_Aftermath_GpuCrashDump_CreateDecoder(GFSDK_Aftermath_Version_API, dump, dumpSize, &decoder) !=
                GFSDK_Aftermath_Result_Success)
            {
                OLO_CORE_ERROR("[Aftermath] could not create a decoder for the crash dump");
                return;
            }

            GFSDK_Aftermath_GpuCrashDump_PageFaultInfo pageFault = {};
            if (GFSDK_Aftermath_GpuCrashDump_GetPageFaultInfo(decoder, &pageFault) ==
                GFSDK_Aftermath_Result_Success)
            {
                OLO_CORE_ERROR("[Aftermath] PAGE FAULT at {:#x} — {} faultType={} (engine={}, client={}, {} resource(s) "
                               "named)",
                               pageFault.faultingGpuVA, AccessTypeName(pageFault.accessType),
                               static_cast<int>(pageFault.faultType), static_cast<int>(pageFault.engine),
                               static_cast<int>(pageFault.client), pageFault.resourceInfoCount);

                if (pageFault.resourceInfoCount > 0u)
                {
                    std::vector<GFSDK_Aftermath_GpuCrashDump_ResourceInfo> resources(pageFault.resourceInfoCount);
                    if (GFSDK_Aftermath_GpuCrashDump_GetPageFaultResourceInfo(
                            decoder, pageFault.resourceInfoCount, resources.data()) ==
                        GFSDK_Aftermath_Result_Success)
                    {
                        for (const auto& r : resources)
                        {
                            // `bWasDestroyed` and the Vulkan-only "MemoryFreed"
                            // residency are the whole reason this integration
                            // exists: they say outright whether the GPU read a
                            // resource the CPU had already destroyed.
                            OLO_CORE_ERROR("[Aftermath]   resource: apiHandle={:#x} gpuVa={:#x} size={} "
                                           "{}x{}x{} mips={} vkFormat={} residency='{}' destroyed={} "
                                           "createTick={} destroyTick={} name='{}'",
                                           r.apiResource, r.gpuVa, r.size, r.width, r.height, r.depth, r.mipLevels,
                                           r.format, ResidencyName(r.residency), r.bWasDestroyed != 0,
                                           r.createTick, r.destroyTick, r.debugName);
                        }
                    }
                }
            }
            else
            {
                OLO_CORE_ERROR("[Aftermath] the crash dump carries no page-fault section — the device loss was "
                               "not an invalid memory access (a hang or a shader exception looks like this)");
            }

            u32 shaderCount = 0;
            if (GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfoCount(decoder, &shaderCount) ==
                    GFSDK_Aftermath_Result_Success &&
                shaderCount > 0u)
            {
                std::vector<GFSDK_Aftermath_GpuCrashDump_ShaderInfo> shaders(shaderCount);
                if (GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfo(decoder, shaderCount, shaders.data()) ==
                    GFSDK_Aftermath_Result_Success)
                {
                    for (const auto& s : shaders)
                    {
                        // The dump's shaderHash is NOT the binary hash (the SDK
                        // header says so explicitly) — it has to be converted
                        // before it can be matched against what we registered.
                        std::string name = "<unmatched>";
                        GFSDK_Aftermath_ShaderBinaryHash binaryHash = {};
                        if (GFSDK_Aftermath_GetShaderHashForShaderInfo(decoder, &s, &binaryHash) ==
                            GFSDK_Aftermath_Result_Success)
                        {
                            const std::lock_guard lock(s_ShaderNameMutex);
                            if (const auto it = s_ShaderNames.find(binaryHash.hash); it != s_ShaderNames.end())
                            {
                                name = it->second;
                            }
                        }
                        OLO_CORE_ERROR("[Aftermath]   active shader: '{}' hash={:#x} binaryHash={:#x} type={} "
                                       "internal={}",
                                       name, s.shaderHash, binaryHash.hash, static_cast<int>(s.shaderType),
                                       s.isInternal != 0);
                    }
                }
            }

            GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
        }

        void GFSDK_AFTERMATH_CALL OnCrashDump(const void* dump, const u32 dumpSize, void* /*userData*/)
        {
            const std::lock_guard lock(s_CrashDumpMutex);
            s_DumpReceived.store(true, std::memory_order_release);
            OLO_CORE_ERROR("[Aftermath] GPU crash dump received ({} bytes)", dumpSize);

            // Write the dump first, decode second: if decoding throws or the
            // decoder rejects it, the file is still on disk for Nsight.
            try
            {
                const std::filesystem::path dir{ "CrashReports" };
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
                const auto path = dir / ("OloEngine-" + std::to_string(stamp) + ".nv-gpudmp");
                if (std::ofstream out{ path, std::ios::binary }; out)
                {
                    out.write(static_cast<const char*>(dump), static_cast<std::streamsize>(dumpSize));
                    OLO_CORE_ERROR("[Aftermath] crash dump written to {}", path.string());
                }
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("[Aftermath] could not write the crash dump file ({}) — decoding it anyway",
                               e.what());
            }

            try
            {
                DecodeAndLog(dump, dumpSize);
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("[Aftermath] decoding the crash dump threw: {}", e.what());
            }
        }

        void GFSDK_AFTERMATH_CALL OnShaderDebugInfo(const void* /*debugInfo*/, const u32 /*size*/, void* /*userData*/)
        {
            // Not persisted yet: mapping a shader hash back to source needs the
            // debug-info blobs keyed by identifier, which is worth doing only
            // once a fault actually names a shader we cannot otherwise place.
        }

        void GFSDK_AFTERMATH_CALL OnDescription(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addValue,
                                                void* /*userData*/)
        {
            addValue(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "OloEngine");
            addValue(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion, "dev");
        }

        void GFSDK_AFTERMATH_CALL OnResolveMarker(const void* /*markerData*/, const u32 /*markerDataSize*/,
                                                  void* /*userData*/,
                                                  PFN_GFSDK_Aftermath_ResolveMarker /*resolveMarker*/)
        {
            // No application markers are pushed: the pass names already reach a
            // fault report through VK_NV_device_diagnostic_checkpoints, so
            // there is nothing here to resolve.
        }
    } // namespace

    bool IsCompiledIn()
    {
        return true;
    }

    bool IsEnabled()
    {
        return Requested() && s_Initialized.load(std::memory_order_acquire);
    }

    void Initialize()
    {
        if (!Requested() || s_Initialized.load(std::memory_order_acquire))
        {
            return;
        }
        const auto result = GFSDK_Aftermath_EnableGpuCrashDumps(
            GFSDK_Aftermath_Version_API, GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
            GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks, OnCrashDump, OnShaderDebugInfo,
            OnDescription, OnResolveMarker, nullptr);
        if (result != GFSDK_Aftermath_Result_Success)
        {
            // Loud, not silent: a session that asked for crash dumps and will
            // not get them must say so before the device loss, not after.
            OLO_CORE_ERROR("[Aftermath] GFSDK_Aftermath_EnableGpuCrashDumps failed ({:#x}) — no GPU crash dump "
                           "will be produced",
                           static_cast<u32>(result));
            return;
        }
        s_Initialized.store(true, std::memory_order_release);
        OLO_CORE_INFO("[Aftermath] GPU crash dumps armed (Vulkan)");
    }

    u32 DeviceDiagnosticsFlags()
    {
        if (!IsEnabled())
        {
            return 0u;
        }
        // Resource tracking is what puts the page-fault resource descriptor —
        // including the residency and destroyed flags — into the dump. Without
        // it the dump names an address and nothing else, which is what the
        // existing VK_EXT_device_fault report already gives.
        return static_cast<u32>(VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV |
                                VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV |
                                VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV |
                                VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV);
    }

    const char* DeviceExtensionName()
    {
        return IsEnabled() ? VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME : nullptr;
    }

    void OnDeviceLost()
    {
        if (!IsEnabled())
        {
            return;
        }
        OLO_CORE_ERROR("[Aftermath] device lost — waiting for the GPU crash dump");

        // The dump is assembled on a driver thread. Bounded wait: the process
        // is already losing its device and a hang here would replace a
        // diagnosable crash with an undiagnosable one.
        constexpr auto kTimeout = std::chrono::seconds(10);
        const auto deadline = std::chrono::steady_clock::now() + kTimeout;
        auto status = GFSDK_Aftermath_CrashDump_Status_Unknown;
        bool statusQueryFailed = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (GFSDK_Aftermath_GetCrashDumpStatus(&status) != GFSDK_Aftermath_Result_Success)
            {
                // NOT a timeout, and `status` may not have been written — say
                // which of the two happened rather than blaming the clock.
                statusQueryFailed = true;
                break;
            }
            if (status == GFSDK_Aftermath_CrashDump_Status_Finished ||
                status == GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (statusQueryFailed)
        {
            OLO_CORE_ERROR("[Aftermath] GFSDK_Aftermath_GetCrashDumpStatus failed — cannot tell whether a dump is "
                           "still being collected");
        }
        else if (status == GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed)
        {
            OLO_CORE_ERROR("[Aftermath] the driver failed to collect a crash dump");
        }
        else if (!s_DumpReceived.load(std::memory_order_acquire))
        {
            OLO_CORE_ERROR("[Aftermath] no crash dump arrived within {} s (status {}) — the device loss may not "
                           "have been a GPU fault this process caused",
                           kTimeout.count(), static_cast<int>(status));
        }
    }

    void RegisterShaderBinary(const char* name, const void* spirv, const sizet sizeBytes)
    {
        if (!IsEnabled() || name == nullptr || spirv == nullptr || sizeBytes == 0u)
        {
            return;
        }
        GFSDK_Aftermath_SpirvCode code{};
        code.pData = spirv;
        code.size = static_cast<u32>(sizeBytes);
        GFSDK_Aftermath_ShaderBinaryHash hash = {};
        if (GFSDK_Aftermath_GetShaderHashSpirv(GFSDK_Aftermath_Version_API, &code, &hash) !=
            GFSDK_Aftermath_Result_Success)
        {
            return;
        }
        try
        {
            const std::lock_guard lock(s_ShaderNameMutex);
            s_ShaderNames.emplace(hash.hash, name);
        }
        catch (...)
        {
            // A diagnostic must never take a shader compile down with it.
        }
    }

    void Shutdown()
    {
        if (s_Initialized.exchange(false, std::memory_order_acq_rel))
        {
            GFSDK_Aftermath_DisableGpuCrashDumps();
            // Reset with the handler, not just alongside it: a second device
            // (backend switch, swapchain rebuild) would otherwise inherit a
            // latched "a dump already arrived" and stay silent about one that
            // never came.
            s_DumpReceived.store(false, std::memory_order_release);
        }
    }
#else
    bool IsCompiledIn()
    {
        return false;
    }

    bool IsEnabled()
    {
        return false;
    }

    void Initialize()
    {
        // Not a silent no-op: a session that set the lever on a build without
        // the SDK would otherwise wait for a dump that can never arrive.
        if (Levers::VulkanAftermathCrashDumps())
        {
            OLO_CORE_WARN("[Aftermath] GPU crash dumps requested, but this build has no Nsight Aftermath SDK. "
                          "Set AFTERMATH_SDK_ROOT and reconfigure (see OloEngine/CMakeLists.txt).");
        }
    }

    u32 DeviceDiagnosticsFlags()
    {
        return 0u;
    }

    const char* DeviceExtensionName()
    {
        return nullptr;
    }

    void OnDeviceLost()
    {
    }

    void RegisterShaderBinary(const char*, const void*, sizet)
    {
    }

    void Shutdown()
    {
    }
#endif // OLO_WITH_AFTERMATH
} // namespace OloEngine::VulkanAftermath
