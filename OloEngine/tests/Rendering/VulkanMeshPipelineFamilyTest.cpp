// OLO_TEST_LAYER: plumbing
//
// #1029 — the mesh / G-Buffer shader family must produce a real VkPipeline.
//
// The hole this closes: every other device-backed Vulkan suite builds its
// pipelines from FullscreenBlit.glsl and a handful of source-string test
// shaders, so a construct that is legal GLSL, valid SPIR-V and correct on
// OpenGL — but that the descriptor-heap binding model rejects — never reaches
// a pipeline in the suite. That is #994: one `.length()` in a fragment shader
// shipped through 7086 green tests (all 126 device-backed Vulkan tests
// included) and then killed the live editor.
//
// WHAT #994 ACTUALLY DOES, measured here rather than assumed: the NVIDIA
// driver ACCESS-VIOLATES inside vkCreateGraphicsPipelines. It does not return
// an error for VulkanPipelineBuilder to report — the stack is
// vvl::DispatchDevice::CreateGraphicsPipelines -> nvoglv64 -> 0xC0000005, with
// the validation layer's VUID-VkPipelineShaderStageCreateInfo-pNext-11378
// message logged just before. So a test CANNOT provoke this construct and then
// assert on the result: the process dies, and because the SEH unwind skips the
// pipeline builder's std::lock_guard, the fixture's TearDown then deadlocks on
// the builder mutex and the whole run hangs.
//
// Hence the two-part shape:
//
//   1. A SPIR-V PRE-FLIGHT (device-free, runs everywhere including CI):
//      OpArrayLength against a buffer block is refused before the module can
//      reach a driver. MeshPipelineSpirvContract proves the detector against a
//      probe shader that carries the construct and a twin that does not.
//   2. The DEVICE-BACKED GATE: every shipped mesh / G-Buffer shader goes
//      through the pre-flight and then through the draw path's own
//      VulkanPipelineBuilder::GetOrCreateGraphics call, and must yield a real
//      VkPipeline. The pre-flight runs first so a shader carrying the #994
//      construct is reported as a failure instead of crashing the suite.
//
// The family is DERIVED from the shader directory (anything with "GBuffer" in
// the name, plus the PBR_MultiLight and DepthPrepass variants that share the
// include closure), so a new *_GBuffer.glsl is covered the day it lands.
//
// The device half is gated: it SKIPs cleanly where the ADR 0010 contract is
// unmet (the AMD self-hosted runner among them — RADV has no
// VK_EXT_descriptor_heap, see #1029), never DISABLED_.

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#include "ShaderHarness.h"

#include <spirv_cross/spirv_cross.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace SH = ShaderHarness;

        // --- The #994 pre-flight ---------------------------------------------

        // Every buffer block on this backend — UBO and SSBO alike — is reached
        // through VK_DESCRIPTOR_MAPPING_SOURCE_INDIRECT_ADDRESS_EXT (ADR 0011
        // §4: the root struct carries one GPU address per block). The spec
        // forbids OpArrayLength on any resource mapped that way
        // (VUID-VkPipelineShaderStageCreateInfo-pNext-11378), so ANY
        // OpArrayLength in a module this backend compiles is a defect — there
        // is no legal case to carve out, which is why this needs no knowledge
        // of which binding it hit.
        struct ArrayLengthUse
        {
            u32 PointerId = 0; ///< The SPIR-V id OpArrayLength was applied to.
            std::string Name;  ///< Resolved block/variable name, when reflection knows one.
        };

        // Raw word-stream scan rather than a SPIRV-Cross query: SPIRV-Cross
        // models OpArrayLength as an expression during *compilation*, and the
        // reflection-only Compiler this repo uses does not expose it. The
        // encoding is fixed and tiny — the instruction word is
        // (wordCount << 16) | opcode, and OpArrayLength (68) is
        // [type, result, structPointer, memberIndex].
        [[nodiscard]] std::vector<ArrayLengthUse> FindArrayLengthUses(const std::vector<u32>& spirv)
        {
            constexpr u32 kSpirvMagic = 0x07230203u;
            constexpr u32 kHeaderWords = 5u;
            constexpr u32 kOpArrayLength = 68u;

            std::vector<ArrayLengthUse> uses;
            if (spirv.size() <= kHeaderWords || spirv[0] != kSpirvMagic)
            {
                return uses;
            }

            for (sizet i = kHeaderWords; i < spirv.size();)
            {
                const u32 wordCount = spirv[i] >> 16u;
                const u32 opcode = spirv[i] & 0xFFFFu;
                // A zero word count would not advance — refuse to spin on a
                // malformed module rather than hang the suite.
                if (wordCount == 0 || i + wordCount > spirv.size())
                {
                    break;
                }
                if (opcode == kOpArrayLength && wordCount >= 5)
                {
                    uses.push_back({ spirv[i + 3], {} });
                }
                i += wordCount;
            }

            if (uses.empty())
            {
                return uses;
            }

            // Name the offender where reflection can: "Payload" beats "%14".
            const spirv_cross::Compiler refl(spirv);
            const spirv_cross::ShaderResources resources = refl.get_shader_resources();
            for (ArrayLengthUse& use : uses)
            {
                for (const auto& buffer : resources.storage_buffers)
                {
                    if (buffer.id == use.PointerId)
                    {
                        use.Name = buffer.name.empty() ? refl.get_name(buffer.id) : buffer.name;
                        break;
                    }
                }
                if (use.Name.empty())
                {
                    use.Name = refl.get_name(use.PointerId);
                }
            }
            return uses;
        }

        [[maybe_unused]] [[nodiscard]] std::string DescribeArrayLengthUses(const std::vector<ArrayLengthUse>& uses)
        {
            std::string description;
            for (const ArrayLengthUse& use : uses)
            {
                if (!description.empty())
                {
                    description += ", ";
                }
                description += use.Name.empty() ? ("%" + std::to_string(use.PointerId)) : use.Name;
            }
            return description;
        }

        // --- The family, derived from disk -----------------------------------

        // "GBuffer" anywhere in the stem covers the 17 *GBuffer* shaders (the
        // deferred material writers, the decal set, the debug view and the
        // flags resolve); the two prefixes add the forward PBR pair and the
        // depth-prepass variants, which share the same include closure and the
        // same reflected binding set. Name-based on purpose: the point is that
        // adding a shader to the family needs no edit here.
        [[maybe_unused]] [[nodiscard]] bool IsMeshFamilyShader(const std::string& stem)
        {
            return stem.contains("GBuffer") || stem.starts_with("PBR_MultiLight") ||
                   stem.starts_with("DepthPrepass");
        }

        // The probe pair differ in exactly one expression. Everything else —
        // the std430 block, the binding, the read — is what a G-Buffer fragment
        // shader does every frame.
        constexpr const char* kArrayLengthProbe = R"(
#version 460 core
layout(std430, binding = 30) readonly buffer Payload { vec4 Items[]; };
layout(location = 0) out vec4 o_Color;
void main()
{
    o_Color = Items[min(uint(gl_FragCoord.x), uint(Items.length()) - 1u)];
}
)";

        constexpr const char* kCleanTwin = R"(
#version 460 core
layout(std430, binding = 30) readonly buffer Payload { vec4 Items[]; };
layout(std140, binding = 31) uniform Counts { uint u_ItemCount; };
layout(location = 0) out vec4 o_Color;
void main()
{
    o_Color = Items[min(uint(gl_FragCoord.x), u_ItemCount - 1u)];
}
)";

        [[nodiscard]] std::vector<u32> CompileProbe(const char* source)
        {
            shaderc::Compiler compiler;
            const auto result = SH::CompileVulkanBackendStageToSpv(
                "probe.glsl", source, shaderc_glsl_fragment_shader, SH::ResolveShaderRoot(), compiler);
            if (result.GetCompilationStatus() != shaderc_compilation_status_success)
            {
                return {};
            }
            return { result.cbegin(), result.cend() };
        }
    } // namespace
} // namespace OloEngine::Tests

// -----------------------------------------------------------------------------
// The detector, proved against a shader that carries the #994 construct and one
// that does not. Device-free and toolchain-only, so it runs everywhere the
// suite runs — including the CI arms where no Vulkan device exists. A test that
// has never been seen to fire is not evidence, and this is the one that can be
// seen to fire without a driver crash.
// -----------------------------------------------------------------------------
TEST(MeshPipelineSpirvContract, ArrayLengthProbeIsDetected)
{
    using namespace OloEngine::Tests;

    const std::vector<u32> spirv = CompileProbe(kArrayLengthProbe);
    ASSERT_FALSE(spirv.empty())
        << "The probe must COMPILE — the whole point of #994 is that this construct passes every "
           "compile-time and SPIR-V check and only fails when a driver sees it";

    const std::vector<ArrayLengthUse> uses = FindArrayLengthUses(spirv);
    ASSERT_FALSE(uses.empty())
        << "The pre-flight did not spot OpArrayLength in a shader that calls .length(). "
           "The gate in EveryShippedMeshShaderProducesAPipeline is therefore blind, and a shader "
           "carrying the #994 construct would reach the driver and crash the suite.";
    EXPECT_EQ(uses.front().Name, "Payload") << "The report must name the block a developer can find";
}

TEST(MeshPipelineSpirvContract, CleanShaderIsNotFlagged)
{
    using namespace OloEngine::Tests;

    const std::vector<u32> spirv = CompileProbe(kCleanTwin);
    ASSERT_FALSE(spirv.empty()) << "The clean twin must compile";

    // The twin reads the same buffer at the same binding and differs only in
    // taking its count from a UBO. Without this, a detector that flagged every
    // shader would pass the test above and fail the whole family.
    EXPECT_TRUE(FindArrayLengthUses(spirv).empty())
        << "The pre-flight flagged a shader that never calls .length() — it would reject the "
           "entire mesh family for a construct none of them use";
}

// -----------------------------------------------------------------------------
// The device-backed gate.
// -----------------------------------------------------------------------------

#if !OLO_WITH_VULKAN

TEST(VulkanMeshPipelineFamily, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "OLO_WITH_VULKAN is off — the Vulkan backend is not compiled into this build.";
}

#else

#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Shader.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanShader.h"

#include <volk.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string_view>

namespace
{
    using namespace OloEngine;
    using namespace OloEngine::Tests;
    namespace fs = std::filesystem;

    // Enumerated from `assets/shaders` (the fixture chdir's into OloEditor/),
    // top level only — include/ holds no standalone shaders and tests/ holds
    // fixtures the renderer never creates a pipeline for.
    [[nodiscard]] std::vector<fs::path> EnumerateMeshFamily()
    {
        std::vector<fs::path> out;
        std::error_code ec;
        const fs::path root = fs::path("assets") / "shaders";
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec) || ec)
            {
                ec.clear();
                continue;
            }
            const fs::path& p = it->path();
            if (p.extension() != ".glsl")
            {
                continue;
            }
            if (IsMeshFamilyShader(p.stem().string()))
            {
                out.push_back(p);
            }
        }
        std::ranges::sort(out);
        return out;
    }

    // --- Attachment plan, derived from the fragment stage --------------------

    // The colour attachments a pipeline for this shader must declare. Derived
    // from the fragment stage's reflected outputs rather than guessed: a
    // G-Buffer writer declares four, a depth prepass declares none, and handing
    // vkCreateGraphicsPipelines a count that disagrees with the shader is
    // itself a validation error — which would make this suite fail for a reason
    // that has nothing to do with the shader.
    struct ColorPlan
    {
        u32 Count = 0;
        std::array<VkFormat, 8> Formats{};
        std::string Error; ///< Non-empty when the plan could not be derived.
    };

    [[nodiscard]] VkFormat FormatForOutput(spirv_cross::SPIRType::BaseType base)
    {
        switch (base)
        {
            case spirv_cross::SPIRType::UInt:
            case spirv_cross::SPIRType::UByte:
            case spirv_cross::SPIRType::UShort:
                return VK_FORMAT_R32G32B32A32_UINT;
            case spirv_cross::SPIRType::Int:
            case spirv_cross::SPIRType::SByte:
            case spirv_cross::SPIRType::Short:
                return VK_FORMAT_R32G32B32A32_SINT;
            default:
                // Float/Half alike: a wide float attachment accepts any
                // floating-point output width without a numeric-type mismatch.
                return VK_FORMAT_R16G16B16A16_SFLOAT;
        }
    }

    [[nodiscard]] ColorPlan DeriveColorPlan(const std::vector<u32>& fragmentSpirv)
    {
        ColorPlan plan;
        if (fragmentSpirv.empty())
        {
            plan.Error = "the shader has no fragment stage";
            return plan;
        }

        const spirv_cross::Compiler refl(fragmentSpirv);
        const spirv_cross::ShaderResources resources = refl.get_shader_resources();
        for (const auto& out : resources.stage_outputs)
        {
            if (!refl.has_decoration(out.id, spv::DecorationLocation))
            {
                continue; // Built-ins (gl_FragDepth) occupy no attachment.
            }
            const u32 location = refl.get_decoration(out.id, spv::DecorationLocation);
            const spirv_cross::SPIRType& type = refl.get_type(out.type_id);
            // An array-typed output occupies one location per element.
            u32 span = 1;
            for (const u32 dim : type.array)
            {
                span *= (dim == 0 ? 1u : dim);
            }
            const VkFormat format = FormatForOutput(type.basetype);
            for (u32 i = 0; i < span; ++i)
            {
                const u32 slot = location + i;
                if (slot >= plan.Formats.size())
                {
                    plan.Error = "fragment output location " + std::to_string(slot) +
                                 " exceeds the 8 colour attachments a thin PSO can declare";
                    return plan;
                }
                plan.Formats[slot] = format;
                plan.Count = std::max(plan.Count, slot + 1);
            }
        }
        // A gap (location 0 and 2 declared, 1 not) still needs a format for the
        // undeclared slot — a G-Buffer pass binds every attachment regardless.
        for (u32 i = 0; i < plan.Count; ++i)
        {
            if (plan.Formats[i] == VK_FORMAT_UNDEFINED)
            {
                plan.Formats[i] = VK_FORMAT_R16G16B16A16_SFLOAT;
            }
        }
        return plan;
    }

    // --- Fixture --------------------------------------------------------------

    // Walk up from CWD to the folder containing OloEditor/ and chdir into it,
    // so "assets/shaders/..." and the shader includer resolve — the
    // VulkanShaderPipelineTest fixture's rule, replicated.
    bool ChangeToOloEditorDir()
    {
        std::error_code ec;
        fs::path candidate = fs::current_path(ec);
        if (ec)
        {
            return false;
        }
        for (int i = 0; i < 6; ++i)
        {
            const fs::path editorDir = candidate / "OloEditor";
            if (fs::exists(editorDir / "assets" / "shaders", ec) && !ec)
            {
                fs::current_path(editorDir, ec);
                return !ec;
            }
            ec.clear();
            if (!candidate.has_parent_path() || candidate.parent_path() == candidate)
            {
                break;
            }
            candidate = candidate.parent_path();
        }
        const bool alreadyThere = fs::exists(fs::path("assets") / "shaders", ec) && !ec;
        return alreadyThere;
    }

    struct ScopedVulkanApiSelection
    {
        ScopedVulkanApiSelection()
        {
            RendererAPI::SetAPI(RendererAPI::API::Vulkan);
        }
        ~ScopedVulkanApiSelection()
        {
            RendererAPI::SetAPI(RendererAPI::API::OpenGL);
        }
    };

    class VulkanMeshPipelineFamily : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (volkInitialize() != VK_SUCCESS)
            {
                GTEST_SKIP() << "No Vulkan loader on this machine.";
            }

            // Bare probe instance first — constructing VulkanDevice on a
            // driverless machine SEH-faults under ASan (the shared ladder).
            VkApplicationInfo appInfo{};
            appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.apiVersion = VulkanCapabilities::kMinApiVersion;
            VkInstanceCreateInfo instanceInfo{};
            instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            instanceInfo.pApplicationInfo = &appInfo;
            VkInstance probe = VK_NULL_HANDLE;
            if (vkCreateInstance(&instanceInfo, nullptr, &probe) != VK_SUCCESS)
            {
                GTEST_SKIP() << "vkCreateInstance failed (no Vulkan 1.4 runtime).";
            }
            volkLoadInstance(probe);

            u32 deviceCount = 0;
            if (vkEnumeratePhysicalDevices(probe, &deviceCount, nullptr) != VK_SUCCESS)
            {
                vkDestroyInstance(probe, nullptr);
                GTEST_SKIP() << "vkEnumeratePhysicalDevices (count) failed on this machine.";
            }
            std::vector<VkPhysicalDevice> devices(deviceCount);
            if (deviceCount > 0)
            {
                const VkResult listResult = vkEnumeratePhysicalDevices(probe, &deviceCount, devices.data());
                if (listResult == VK_SUCCESS || listResult == VK_INCOMPLETE)
                {
                    devices.resize(deviceCount);
                }
                else
                {
                    vkDestroyInstance(probe, nullptr);
                    GTEST_SKIP() << "vkEnumeratePhysicalDevices (list) failed on this machine.";
                }
            }
            const bool anySatisfying = std::ranges::any_of(devices, [](VkPhysicalDevice d)
                                                           { return VulkanCapabilities::Evaluate(d).Satisfied; });
            vkDestroyInstance(probe, nullptr);
            if (!anySatisfying)
            {
                GTEST_SKIP() << "No device satisfies the ADR 0010 capability contract "
                                "(VK_EXT_descriptor_heap et al.) — the gate would refuse --rhi=vulkan here, "
                                "so no pipeline for the mesh family can be created on this machine.";
            }
            if (volkInitialize() != VK_SUCCESS)
            {
                GTEST_SKIP() << "volk re-init failed.";
            }

            if (!ChangeToOloEditorDir())
            {
                GTEST_SKIP() << "Could not locate OloEditor/assets/shaders from CWD.";
            }

            m_Device = std::make_unique<VulkanDevice>();
            try
            {
                m_Device->Init([](VkInstance)
                               { return VkSurfaceKHR(VK_NULL_HANDLE); });
            }
            catch (const std::exception& e)
            {
                m_Device.reset();
                GTEST_SKIP() << "VulkanDevice::Init failed: " << e.what();
            }
            VulkanDevice::ResetValidationErrorCount();
            VulkanPipelineBuilder::Get().ClearLastCreationFailure();
        }

        void TearDown() override
        {
            if (!m_Device)
            {
                return;
            }
            vkDeviceWaitIdle(m_Device->GetDevice());
            // Process-wide singletons hold objects belonging to THIS device.
            VulkanPipelineBuilder::Get().ReleaseAll();
            VulkanPipelineBuilder::Get().ClearLastCreationFailure();
            VulkanResourceHeap::Get().Release();
            VulkanFrameArena::Get().ReleaseBuffers();
            VulkanDeferredReclaim::Get().FlushAll();
            VulkanPipelineCache::Get().SaveAndDestroy();
            m_Device->Shutdown();
            m_Device.reset();
        }

        std::unique_ptr<VulkanDevice> m_Device;
    };

    // The recorded state a G-Buffer / depth draw arrives with: depth tested and
    // written, no blend, back-face culled. Blend is a baked PSO axis only where
    // EDS3 is absent, so this is one pipeline per shader either way.
    [[nodiscard]] VulkanRecordedPipelineState MeshDrawState()
    {
        VulkanRecordedPipelineState state{};
        state.DepthTest = true;
        state.DepthWrite = true;
        state.Culling = true;
        state.CullFace = RHI::CullMode::Back;
        return state;
    }
} // namespace

// -----------------------------------------------------------------------------
// The gate. Every shipped mesh / G-Buffer shader must yield a real VkPipeline
// through the draw path's own builder call.
//
// Each shader is pre-flighted first. A module carrying OpArrayLength is
// reported and NOT handed to the driver — on the dev box's NVIDIA driver that
// call is an access violation inside nvoglv64, which would kill the process and
// then hang the run in TearDown (the SEH unwind skips the builder's lock_guard,
// so ReleaseAll deadlocks). Reporting beats crashing, and the failure text says
// the same thing the crash would have.
// -----------------------------------------------------------------------------
TEST_F(VulkanMeshPipelineFamily, EveryShippedMeshShaderProducesAPipeline)
{
    ScopedVulkanApiSelection vulkanSelected;

    const std::vector<fs::path> family = EnumerateMeshFamily();

    // A glob that matched nothing would report success while testing nothing.
    // These four anchors are the load-bearing members of the family: the
    // deferred material writer and its skinned twin, the forward PBR pass, and
    // the depth prepass. If the derivation stops finding them, the enumeration
    // broke — not the shaders.
    const auto has = [&family](std::string_view stem)
    {
        return std::ranges::any_of(family, [stem](const fs::path& p)
                                   { return p.stem().string() == stem; });
    };
    ASSERT_TRUE(has("PBR_GBuffer")) << "The family derivation found no PBR_GBuffer.glsl — enumeration is broken";
    ASSERT_TRUE(has("PBR_GBuffer_Skinned"));
    ASSERT_TRUE(has("PBR_MultiLight"));
    ASSERT_TRUE(has("DepthPrepass"));
    ASSERT_GE(family.size(), 20u) << "The mesh/G-Buffer family shrank unexpectedly — " << family.size()
                                  << " shaders found where #1029 counted 23";

    const bool meshShadersEnabled = m_Device->IsMeshShaderEnabled();
    const auto started = std::chrono::steady_clock::now();

    u32 built = 0;
    u32 skippedForMeshStage = 0;
    for (const fs::path& path : family)
    {
        const std::string name = path.stem().string();
        SCOPED_TRACE(name);

        // A task/mesh-stage shader is only ever loaded behind the
        // SupportsMeshShaders() gate (#813); on a device without
        // VK_EXT_mesh_shader the engine never asks for it either, so neither
        // does this suite. Counted and reported, never silent.
        const std::string source = SH::ReadWholeFile(path);
        if (source.contains("#type mesh") && !meshShadersEnabled)
        {
            ++skippedForMeshStage;
            continue;
        }

        // Report every family member, so one broken shader does not hide the
        // rest: a failure here is ADD_FAILURE + continue, never an ASSERT that
        // abandons the remaining shaders.
        Ref<Shader> shader = Shader::Create(path.generic_string());
        if (!shader || shader->GetCompilationStatus() != ShaderCompilationStatus::Ready)
        {
            ADD_FAILURE() << name << " does not compile through shaderc(vulkan_1_4) — no pipeline can exist for it";
            continue;
        }
        auto* vkShader = static_cast<VulkanShader*>(shader.get());

        // The pre-flight, on the production SPIR-V, before any driver sees it.
        bool preflightFailed = false;
        for (const auto& [stage, stageSpirv] : vkShader->GetSPIRV())
        {
            const std::vector<ArrayLengthUse> uses = FindArrayLengthUses(stageSpirv);
            if (uses.empty())
            {
                continue;
            }
            preflightFailed = true;
            ADD_FAILURE()
                << name << " calls .length() on a buffer block (" << DescribeArrayLengthUses(uses)
                << ") in stage 0x" << std::hex << static_cast<u32>(stage) << std::dec
                << ".\nEvery buffer block on this backend is mapped through "
                   "VK_DESCRIPTOR_MAPPING_SOURCE_INDIRECT_ADDRESS_EXT, and the spec forbids OpArrayLength "
                   "on such a resource (VUID-VkPipelineShaderStageCreateInfo-pNext-11378). This is #994: "
                   "legal GLSL, valid SPIR-V, correct on OpenGL, and an access violation inside the driver "
                   "when vkCreateGraphicsPipelines sees it. Pass the count in explicitly — see "
                   "docs/agent-rules/glsl-shaders.md §6b.";
        }
        if (preflightFailed)
        {
            // Deliberately NOT handed to the driver: that call crashes.
            continue;
        }

        // The layout the DRAW PATH uses — the shader's own cached one, not a
        // rebuild, so the mapping array this pipeline bakes is byte-for-byte
        // the one a real draw would bake.
        const VulkanRootDataLayout& layout = vkShader->GetRootDataLayout();

        const auto& spirv = vkShader->GetSPIRV();
        const auto fragmentIt = spirv.find(VK_SHADER_STAGE_FRAGMENT_BIT);
        if (fragmentIt == spirv.end())
        {
            ADD_FAILURE() << name << " has no fragment-stage SPIR-V";
            continue;
        }
        const ColorPlan plan = DeriveColorPlan(fragmentIt->second);
        if (!plan.Error.empty())
        {
            ADD_FAILURE() << name << ": " << plan.Error;
            continue;
        }

        VulkanRenderTargetDesc targets;
        targets.ColorCount = plan.Count;
        targets.ColorFormats = plan.Formats;
        targets.DepthFormat = VK_FORMAT_D32_SFLOAT;
        targets.Samples = 1;

        const VulkanRecordedPipelineState state = MeshDrawState();
        // Cleared per shader, not once per test: GetOrCreateGraphics has
        // early returns that hand back VK_NULL_HANDLE without recording
        // anything (no device, no shader modules). Without this, a genuine
        // failure on an earlier shader would be re-reported as this one's
        // reason, and the "recorded no VkResult" branch below could never be
        // reached again.
        VulkanPipelineBuilder::Get().ClearLastCreationFailure();
        const VkPipeline pipeline = VulkanPipelineBuilder::Get().GetOrCreateGraphics(*vkShader, layout, state, targets);

        if (pipeline == VK_NULL_HANDLE)
        {
            const auto failure = VulkanPipelineBuilder::Get().GetLastCreationFailure();
            ADD_FAILURE() << "vkCreateGraphicsPipelines produced no pipeline for '" << name << "' (" << plan.Count
                          << " colour attachments, " << layout.Fields.size() << " root-data fields)"
                          << (failure.Valid
                                  ? " — builder recorded VkResult " +
                                        std::to_string(static_cast<int>(failure.Result)) + " for '" +
                                        failure.ShaderName + "'"
                                  : " — the builder recorded no VkResult, so creation was refused before "
                                    "vkCreateGraphicsPipelines (no device, or an unbuildable stage set)")
                          << ".";
            continue;
        }
        ++built;
    }

    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    OLO_CORE_INFO("[VulkanMeshPipelineFamily] {} of {} family shaders built a pipeline ({} skipped for a mesh "
                  "stage this device cannot run) in {} ms",
                  built, family.size(), skippedForMeshStage, elapsedMs);

    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u)
        << "The bar: ZERO validation errors while creating the family's pipelines";
}

#endif // OLO_WITH_VULKAN
