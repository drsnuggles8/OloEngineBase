// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GroomRootFramesParityTest -- issue #1533 E1.
//
// A bound coat's drawn roots are evaluated on the GPU (compute/GroomRootFrames
// .comp) when nothing on the CPU needs them, and EvaluateGroomRootTransforms
// stays the reference. This file runs the REAL kernel on the REAL buffer layout
// -- GroomDeformBuffer's static regions, its palette, one dispatch -- and holds
// every root record it writes to the frame the CPU evaluates from the same
// inputs, this frame's and last frame's.
//
// Within 1e-5 m and a rotation dot of 1 - 1e-6 rather than bit for bit: the two
// sides are two compilers' float arithmetic over the same operations, and the
// GPU's rsqrt, fused multiply-adds and division are not the host's. A quaternion
// is compared up to sign, which is the same rotation either way.
//
// Each case carries its own control: a root whose triangle collapses must come
// back on its bind frame, a coat with no history must emit no motion, and a
// kernel that ignored the previous palette would fail the bent case's motion.
//
// shaderpipe: a real compute shader dispatched on the GPU; SKIPs cleanly with no
// GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "GroomBindingFixture.h"
#include "../Rendering/PropertyTests/RenderPropertyTest.h"

#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomGpuDeformation.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using namespace OloEngine::GroomBindingTest;

        [[nodiscard]] std::vector<glm::mat4> HingePalette(f32 degrees)
        {
            const glm::vec3 hinge{ 0.5f, 0.0f, 0.0f };
            glm::mat4 bend = glm::translate(glm::mat4(1.0f), hinge);
            bend = glm::rotate(bend, glm::radians(degrees), glm::vec3(0.0f, 0.0f, 1.0f));
            bend = glm::translate(bend, -hinge);
            // A third bone that collapses everything it owns to the origin: the
            // degenerate-triangle case.
            return { glm::mat4(1.0f), bend, glm::mat4(0.0f) };
        }

        struct KernelRun
        {
            std::vector<GroomDeformRootRecord> Roots;
            std::vector<GroomDeformBindRecord> Bind;
        };

        // Lays the buffer out as GroomRenderPass does for a GPU-rooted coat,
        // dispatches the kernel once and reads the root region back.
        [[nodiscard]] KernelRun RunKernel(const GroomAsset& groom, const GroomBindingAsset& binding,
                                          const GroomDeformationInputs& inputs, bool usePreviousPose)
        {
            KernelRun out;
            const u32 roots = groom.GetCurveCount();
            std::vector<u32> rootCurves(roots);
            std::iota(rootCurves.begin(), rootCurves.end(), 0u);
            const u32 bones = static_cast<u32>(inputs.Skinning.Palette.size());

            GroomDeformBuffer buffer;
            const GroomDeformBufferLayout layout =
                GroomDeformBufferLayout::Make(roots, 0u, 0u, inputs.Surface.VertexCount, bones);
            EXPECT_TRUE(layout.RootsOnGpu());
            buffer.Reset(layout, rootCurves, nullptr);
            // Every root of the grid sits on a triangle inside the surface, so the
            // kernel evaluates all of them (a collapsed one is held by the kernel,
            // not skipped here).
            EXPECT_EQ(buffer.WriteSurfaceSkin(rootCurves, binding, inputs.Surface, inputs.Skinning, roots), roots);
            // The bind frames the held roots fall back to are written by the
            // first pack; on this layout it packs no roots.
            (void)buffer.PackFrame(rootCurves, binding, {}, nullptr, roots);
            buffer.WritePalette(inputs.Skinning.Palette,
                                usePreviousPose ? inputs.Skinning.PrevPalette : std::span<const glm::mat4>{});

            const std::span<const u8> bytes = buffer.GetBytes();
            Ref<StorageBuffer> gpu = StorageBuffer::Create(static_cast<u32>(bytes.size()),
                                                           ShaderBindingLayout::SSBO_GROOM_DEFORMATION,
                                                           StorageBufferUsage::DynamicDraw);
            EXPECT_TRUE(gpu);
            if (!gpu)
            {
                return out;
            }
            gpu->SetData(bytes.data(), static_cast<u32>(bytes.size()), 0u);

            UBOStructures::GroomRootFrameParamsUBO params;
            params.SurfaceToGroom = inputs.SurfaceToGroom;
            params.PrevSurfaceToGroom = inputs.SurfaceToGroom;
            params.Counts = glm::ivec4(static_cast<i32>(roots), static_cast<i32>(bones), usePreviousPose ? 1 : 0,
                                       static_cast<i32>(layout.VertexCount));
            params.Bases = glm::ivec4(static_cast<i32>(layout.RootBase), static_cast<i32>(layout.SkinBase),
                                      static_cast<i32>(layout.VertexBase), static_cast<i32>(layout.PaletteBase));
            params.Bind = glm::ivec4(static_cast<i32>(layout.BindBase), 0, 0, 0);
            Ref<UniformBuffer> ubo = UniformBuffer::Create(UBOStructures::GroomRootFrameParamsUBO::GetSize(),
                                                           ShaderBindingLayout::UBO_USER_0);
            ubo->SetData(&params, UBOStructures::GroomRootFrameParamsUBO::GetSize());
            ubo->Bind();
            gpu->Bind();

            Ref<ComputeShader> kernel = ComputeShader::Create("assets/shaders/compute/GroomRootFrames.comp");
            EXPECT_TRUE(kernel && kernel->IsValid()) << "GroomRootFrames.comp did not compile";
            if (!kernel || !kernel->IsValid())
            {
                return out;
            }
            kernel->Bind();
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);
            RenderCommand::DispatchCompute((roots + 63u) / 64u, 1u, 1u);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);

            out.Roots.resize(roots);
            gpu->GetData(out.Roots.data(), static_cast<u32>(roots * sizeof(GroomDeformRootRecord)),
                         static_cast<u32>(layout.RootBase * 16u));
            out.Bind.resize(roots);
            gpu->GetData(out.Bind.data(), static_cast<u32>(roots * sizeof(GroomDeformBindRecord)),
                         static_cast<u32>(layout.BindBase * 16u));
            return out;
        }

        [[nodiscard]] f32 QuatAgreement(const glm::vec4& gpu, const glm::quat& cpu)
        {
            return std::abs(gpu.x * cpu.x + gpu.y * cpu.y + gpu.z * cpu.z + gpu.w * cpu.w);
        }

        struct Scene
        {
            GridSurface Grid;
            Ref<GroomAsset> Groom;
            Ref<GroomBindingAsset> Binding;
            std::vector<glm::mat4> Palette;
            std::vector<glm::mat4> PrevPalette;
        };

        [[nodiscard]] Scene MakeScene()
        {
            Scene scene;
            scene.Grid = MakeGrid(8u);
            WeightAsHinge(scene.Grid);
            scene.Groom = MakeCoat(48u, 6u, 0.1f);
            std::string reason;
            GroomBindingBuildStats stats;
            EXPECT_TRUE(GroomBindingBuilder::Build(*scene.Groom, scene.Grid.View(3u), "TestBody",
                                                   GroomBindingBuildSettings{}, scene.Binding, stats, reason))
                << reason;
            scene.Palette = HingePalette(55.0f);
            scene.PrevPalette = HingePalette(40.0f);
            return scene;
        }
    } // namespace

    TEST(GroomRootFramesParityTest, TheKernelWritesTheFramesTheCpuEvaluates)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Scene scene = MakeScene();
        ASSERT_TRUE(scene.Groom && scene.Binding);

        GroomDeformationInputs inputs;
        inputs.Surface = scene.Grid.View(3u);
        inputs.Skinning = scene.Grid.Skinning(scene.Palette, scene.PrevPalette, true);
        inputs.HasHistory = true;
        // The groom a little off the body, so the mapping is not the identity.
        inputs.SurfaceToGroom = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.01f, 0.0f));

        TArray<GroomRootTransform> cpu;
        (void)EvaluateGroomRootTransforms(*scene.Groom, *scene.Binding, inputs, std::nullopt, cpu);
        const KernelRun gpu = RunKernel(*scene.Groom, *scene.Binding, inputs, true);
        ASSERT_EQ(gpu.Roots.size(), static_cast<sizet>(cpu.Num()));

        u32 moving = 0;
        for (u32 root = 0; root < static_cast<u32>(cpu.Num()); ++root)
        {
            SCOPED_TRACE("root " + std::to_string(root));
            const GroomRootTransform& expected = cpu[static_cast<i32>(root)];
            ASSERT_TRUE(expected.Valid) << "the reference held a root on a healthy triangle";
            const GroomDeformRootRecord& got = gpu.Roots[root];
            EXPECT_LT(glm::distance(glm::vec3(got.Origin), expected.Origin), 1.0e-5f);
            EXPECT_GT(QuatAgreement(got.Rotation, expected.Rotation), 1.0f - 1.0e-6f);
            EXPECT_LT(glm::distance(glm::vec3(got.PrevOrigin), expected.PrevOrigin), 1.0e-5f);
            EXPECT_GT(QuatAgreement(got.PrevRotation, expected.PrevRotation), 1.0f - 1.0e-6f);
            moving += glm::distance(expected.Origin, expected.PrevOrigin) > 1.0e-3f ? 1u : 0u;
        }
        // The control: the bend moved roots between the two frames, so a kernel
        // that ignored the previous palette could not have passed the above.
        EXPECT_GT(moving, 4u);
    }

    TEST(GroomRootFramesParityTest, ACollapsedTriangleHoldsItsRootsAndNoHistoryMeansNoMotion)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Scene scene = MakeScene();
        ASSERT_TRUE(scene.Groom && scene.Binding);

        // Every vertex a root's triangle touches on one side goes to the
        // collapsing bone: those triangles are degenerate this frame.
        u32 collapsed = 0;
        for (u32 v = 0; v < scene.Grid.VertexCount(); ++v)
        {
            if (scene.Grid.Positions[v].x < 0.26f)
            {
                scene.Grid.SetInfluence(v, 2u, 1.0f);
                ++collapsed;
            }
        }
        ASSERT_GT(collapsed, 0u);

        GroomDeformationInputs inputs;
        inputs.Surface = scene.Grid.View(3u);
        inputs.Skinning = scene.Grid.Skinning(scene.Palette, scene.PrevPalette, false);
        inputs.HasHistory = false;

        TArray<GroomRootTransform> cpu;
        (void)EvaluateGroomRootTransforms(*scene.Groom, *scene.Binding, inputs, std::nullopt, cpu);
        const KernelRun gpu = RunKernel(*scene.Groom, *scene.Binding, inputs, false);
        ASSERT_EQ(gpu.Roots.size(), static_cast<sizet>(cpu.Num()));

        u32 held = 0;
        for (u32 root = 0; root < static_cast<u32>(cpu.Num()); ++root)
        {
            SCOPED_TRACE("root " + std::to_string(root));
            const GroomRootTransform& expected = cpu[static_cast<i32>(root)];
            const GroomDeformRootRecord& got = gpu.Roots[root];
            if (!expected.Valid)
            {
                // Held: the kernel writes the bind frame for both frames.
                ++held;
                EXPECT_LT(glm::distance(glm::vec3(got.Origin), glm::vec3(gpu.Bind[root].Origin)), 1.0e-6f);
                EXPECT_LT(glm::distance(glm::vec3(got.PrevOrigin), glm::vec3(gpu.Bind[root].Origin)), 1.0e-6f);
                continue;
            }
            EXPECT_LT(glm::distance(glm::vec3(got.Origin), expected.Origin), 1.0e-5f);
            EXPECT_GT(QuatAgreement(got.Rotation, expected.Rotation), 1.0f - 1.0e-6f);
            // No history: the previous frame IS this one, exactly.
            EXPECT_EQ(glm::vec3(got.PrevOrigin), glm::vec3(got.Origin));
            EXPECT_EQ(got.PrevRotation, got.Rotation);
        }
        EXPECT_GT(held, 0u) << "the control: no root sat on a collapsed triangle, so the held path went untested";
    }
} // namespace OloEngine::Tests
