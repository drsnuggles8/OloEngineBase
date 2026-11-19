// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GroomExpandGuidesParityTest -- issue #1589.
//
// A coat simulated at a budget below its guide count is drawn from EVERY guide
// slot's displacement: a left-out slot takes the blend of its stand-ins (#1509).
// That expansion used to run on the CPU, and the dense result -- ~120k points a
// fox at any budget -- crossed the bus every frame. Now only the simulated
// guides' points are sent and compute/GroomExpandGuides.comp writes the dense
// region; ExpandGroomGuideDisplacements stays the reference, and is still what
// every CPU reader of the coat samples.
//
// This file runs the REAL kernel on the REAL buffer layout -- GroomDeformBuffer
// packed from a sparse simulation with its stand-ins, the dispatch parameters it
// hands the pass, one dispatch -- and holds every record the kernel writes to
// the CPU expansion of the same inputs, this frame's and last frame's, at two
// simulation budgets. The tolerance and its reason are in
// GroomExpandGuidesFixture.h. ABrokenStandInWeightIsCaught is the control: one
// slot's weights changed in the uploaded bytes only, and the comparison must
// find that slot and no other.
//
// shaderpipe: a real compute shader dispatched on the GPU; SKIPs cleanly with no
// GL 4.6 context. The Vulkan twin is the VulkanPassSuite tenant
// GroomExpandGuidesKernelMatchesTheCpuExpansion.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "GroomExpandGuidesFixture.h"
#include "../Rendering/PropertyTests/RenderPropertyTest.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using namespace OloEngine::GroomExpandGuidesTest;

        // Lays the buffer out as GroomRenderPass does for a coat whose guides
        // the GPU expands, packs the sparse frame, sends the bytes (after
        // `tamper`, for the control), dispatches the kernel once and reads the
        // dense region back.
        [[nodiscard]] KernelRun RunKernel(const GroomAsset& groom, const Budget& budget,
                                          const std::function<void(std::vector<u8>&, const GroomDeformBufferLayout&)>&
                                              tamper = {})
        {
            PackedFrame packed = PackSparseFrame(groom, budget);
            KernelRun out;
            out.Frame = packed.Frame;
            out.Layout = packed.Layout;
            if (tamper)
            {
                tamper(packed.Bytes, packed.Layout);
            }

            Ref<StorageBuffer> gpu = StorageBuffer::Create(static_cast<u32>(packed.Bytes.size()),
                                                           ShaderBindingLayout::SSBO_GROOM_DEFORMATION,
                                                           StorageBufferUsage::DynamicDraw);
            EXPECT_TRUE(gpu);
            if (!gpu)
            {
                return out;
            }
            gpu->SetData(packed.Bytes.data(), static_cast<u32>(packed.Bytes.size()), 0u);

            UBOStructures::GroomExpandGuideParamsUBO params;
            params.Counts = packed.Dispatch.Counts;
            params.Bases = packed.Dispatch.Bases;
            params.Target = packed.Dispatch.Target;
            Ref<UniformBuffer> ubo = UniformBuffer::Create(UBOStructures::GroomExpandGuideParamsUBO::GetSize(),
                                                           ShaderBindingLayout::UBO_USER_0);
            ubo->SetData(&params, UBOStructures::GroomExpandGuideParamsUBO::GetSize());
            ubo->Bind();
            gpu->Bind();

            Ref<ComputeShader> kernel = ComputeShader::Create("assets/shaders/compute/GroomExpandGuides.comp");
            EXPECT_TRUE(kernel && kernel->IsValid()) << "GroomExpandGuides.comp did not compile";
            if (!kernel || !kernel->IsValid())
            {
                return out;
            }
            kernel->Bind();
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);
            RenderCommand::DispatchCompute(packed.Dispatch.Groups, 1u, 1u);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);

            out.Records.resize(out.Frame.DisplacementCount);
            gpu->GetData(out.Records.data(),
                         static_cast<u32>(out.Records.size() * sizeof(GroomDeformDisplacementRecord)),
                         static_cast<u32>(out.Layout.DisplacementBase * 16u));
            return out;
        }
    } // namespace

    // Two budgets, as a coat's LOD ladder halves its guides: half the slots
    // simulated, then a quarter. Every slot's points, this frame and last.
    TEST(GroomExpandGuidesParityTest, TheKernelExpandsTheStandInsTheCpuExpands)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const Ref<GroomAsset> groom = MakeVariedCoat(144u);
        ASSERT_TRUE(groom);

        for (const u32 keepEvery : { 2u, 4u })
        {
            SCOPED_TRACE("keep every " + std::to_string(keepEvery) + " guide slots");
            const Budget budget = MakeBudget(*groom, keepEvery, true);
            ASSERT_TRUE(budget.StandIns);
            const Reference cpu = ExpandOnCpu(budget);
            const KernelRun gpu = RunKernel(*groom, budget);
            ASSERT_EQ(gpu.Records.size(), static_cast<sizet>(cpu.Current.Num()));

            // The stand-ins must be doing work, or this compares copies: most
            // slots are left out, and only their simulated points crossed.
            const u32 slots = budget.Table->GetGuideCount();
            ASSERT_LE(budget.SlotOfGuide.size() * 2u, static_cast<sizet>(slots) + 1u);
            EXPECT_LT(gpu.Frame.SourceCount, gpu.Frame.DisplacementCount);

            const Comparison result = Compare(gpu, cpu, true);
            EXPECT_TRUE(result.WrongSlots.empty())
                << result.WrongSlots.size() << " of " << slots << " slots disagree, first "
                << (result.WrongSlots.empty() ? 0u : result.WrongSlots.front());
            EXPECT_EQ(result.Points, gpu.Frame.DisplacementCount);
            std::printf("[GroomExpandGuidesParity] GL keep 1/%u: %u slots, %u simulated points -> %u expanded, "
                        "max error %.3e m\n",
                        keepEvery, slots, gpu.Frame.SourceCount, gpu.Frame.DisplacementCount,
                        static_cast<f64>(result.MaxError));
        }
    }

    // A coat with no history emits exactly zero motion: the kernel writes this
    // frame's expansion in both lanes, bit for bit.
    TEST(GroomExpandGuidesParityTest, WithoutHistoryThePreviousFrameIsTheCurrentOne)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const Ref<GroomAsset> groom = MakeVariedCoat(96u);
        ASSERT_TRUE(groom);
        const Budget budget = MakeBudget(*groom, 3u, false);
        ASSERT_TRUE(budget.StandIns);
        const Reference cpu = ExpandOnCpu(budget);
        const KernelRun gpu = RunKernel(*groom, budget);
        ASSERT_EQ(gpu.Records.size(), static_cast<sizet>(cpu.Current.Num()));

        const Comparison result = Compare(gpu, cpu, false);
        EXPECT_TRUE(result.WrongSlots.empty()) << result.WrongSlots.size() << " slots disagree";
        u32 moving = 0;
        for (const GroomDeformDisplacementRecord& record : gpu.Records)
        {
            EXPECT_EQ(std::memcmp(&record.Current, &record.Previous, sizeof(glm::vec4)), 0);
            moving += glm::length(glm::vec3(record.Current)) > 1.0e-4f ? 1u : 0u;
        }
        EXPECT_GT(moving, 0u) << "every point at rest: the comparison above proved nothing";
    }

    // THE CONTROL. One left-out slot's stand-in weights are changed in the
    // uploaded bytes only -- all its weight on its first stand-in -- so the
    // kernel blends differently from the CPU for that slot and nothing else.
    // The comparison must name exactly that slot: an instrument that cannot see
    // a wrong weight cannot vouch for a right one.
    TEST(GroomExpandGuidesParityTest, ABrokenStandInWeightIsCaught)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const Ref<GroomAsset> groom = MakeVariedCoat(144u);
        ASSERT_TRUE(groom);
        const Budget budget = MakeBudget(*groom, 4u, true);
        ASSERT_TRUE(budget.StandIns);
        const u32 broken = FindSlotWithTwoStandIns(budget);
        ASSERT_NE(broken, GroomNoGuide) << "the fixture has no slot with two weighted stand-ins";

        const Reference cpu = ExpandOnCpu(budget);
        const KernelRun gpu = RunKernel(*groom, budget,
                                        [broken](std::vector<u8>& bytes, const GroomDeformBufferLayout& layout)
                                        { BreakStandInWeights(bytes, layout, broken); });
        ASSERT_EQ(gpu.Records.size(), static_cast<sizet>(cpu.Current.Num()));

        const Comparison result = Compare(gpu, cpu, true);
        ASSERT_EQ(result.WrongSlots.size(), 1u) << "the broken weight was not caught, or it leaked into other slots";
        EXPECT_EQ(result.WrongSlots.front(), broken);
    }
} // namespace OloEngine::Tests
