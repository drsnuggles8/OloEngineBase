#pragma once

#include "OloEngine/Core/Base.h"

// =============================================================================
// VulkanAftermath.h — NVIDIA Nsight Aftermath GPU crash dumps (issue #1198).
//
// WHY, in one line: VK_EXT_device_fault says WHERE (an address),
// VK_NV_device_diagnostic_checkpoints says WHEN (which pass, per queue), and
// only Aftermath says WHAT — the resource that owned the faulting address, its
// format and extent, and whether its memory had already been freed.
//
// That last fact is the one a GPU use-after-free investigation turns on, and
// nothing in the validation layer can supply it: the layer's
// VK_EXT_device_address_binding_report gets as far as "a VkImage was bound
// here and unbound there" but cannot say which shader read held the address.
//
// OPTIONAL AND NEVER VENDORED. The SDK is licensed (the Steamworks rule), so
// it is resolved from $ENV{AFTERMATH_SDK_ROOT} at configure time. Without it
// every function here is a stub that reports the absence rather than going
// quiet — a session that asked for a crash dump and did not get one must be
// told why (no-silent-fallbacks.md).
// =============================================================================

namespace OloEngine::VulkanAftermath
{
    // True when the SDK was compiled in AND the lever asked for it. Everything
    // below is a no-op otherwise.
    [[nodiscard]] bool IsEnabled();

    // Compiled against the SDK at all? Distinguishes "not built with Aftermath"
    // from "built with it but switched off", which are different answers when a
    // fault report has no dump.
    [[nodiscard]] bool IsCompiledIn();

    // Arm the crash-dump handler. MUST be called BEFORE vkCreateDevice — the
    // driver reads Aftermath's state at device creation, so arming afterwards
    // produces a dump with no shader or resource tracking in it.
    void Initialize();

    // The device-diagnostics flags to chain into VkDeviceCreateInfo, or 0 when
    // disabled. VulkanDevice owns the VkDeviceDiagnosticsConfigCreateInfoNV
    // struct itself; this only decides what goes in it.
    [[nodiscard]] u32 DeviceDiagnosticsFlags();

    // Name of the device extension the flags above require, or nullptr.
    [[nodiscard]] const char* DeviceExtensionName();

    // Called from the device-lost path, after the VK_EXT_device_fault report.
    // Aftermath assembles its dump ASYNCHRONOUSLY on a driver thread, so this
    // polls GetCrashDumpStatus with a bounded timeout, then writes the
    // .nv-gpudmp and decodes the page-fault section into the log.
    //
    // Bounded rather than infinite on purpose: the process is already losing
    // its device, and a hang here replaces a diagnosable crash with a hung one.
    void OnDeviceLost();

    // Remember `name` for this SPIR-V module so a crash dump's active-shader
    // list can be reported by NAME instead of by hash. Aftermath identifies a
    // shader by a hash of its binary, which is meaningless on its own; hashing
    // every module we create is what turns "hash=0x3dc4a10b8749b9db" into
    // "ScenePass fragment". No-op when Aftermath is off, so the hashing cost is
    // only paid by a session that asked for crash dumps.
    void RegisterShaderBinary(const char* name, const void* spirv, sizet sizeBytes);

    // Release the crash-dump handler. Safe to call when never initialized.
    void Shutdown();
} // namespace OloEngine::VulkanAftermath
