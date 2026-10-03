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
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCasterPose.h"
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

#include <array>
#include <cmath>
#include <cstdio>
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
    // #1533 review: the caster pose frames each root triangle on the CPU and
    // trusts that frame to within GroomCasterFrameTolerance of the one the
    // kernel draws with -- an assumption about two f32 evaluations of the same
    // skin, which this case measures. Triangles of every size and shape (0.3 mm
    // to 1 cm, equilateral to slivers a millionth as tall as they are long),
    // each on its own, blended between two of three bones under a groom mapping
    // that is not a similarity. For every triangle the pose trusts, the
    // kernel's rotation must sit within the tolerance of the CPU's.
    TEST(GroomRootFramesParityTest, TheKernelsFramesStayWithinTheCasterPosesTolerance)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const auto hash = [](u32 a, u32 b)
        {
            u32 h = a * 0x9E3779B9u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;
            h ^= h >> 16u;
            h *= 0x7FEB352Du;
            h ^= h >> 15u;
            return static_cast<f32>(h >> 8u) / static_cast<f32>(1u << 24u);
        };
        constexpr u32 kSide = 24u;
        constexpr u32 kTriangles = kSide * kSide;
        GridSurface surface;
        GroomBuilder builder;
        std::string reason;
        u16 group = 0;
        ASSERT_TRUE(builder.AddGroup("body_undercoat", group, reason)) << reason;
        surface.Influences.assign(static_cast<sizet>(kTriangles) * 3u * 32u, std::byte{ 0 });
        for (u32 t = 0; t < kTriangles; ++t)
        {
            const glm::vec3 centre{ -0.46f + 0.04f * static_cast<f32>(t % kSide), 0.3f * hash(t, 1u),
                                    -0.46f + 0.04f * static_cast<f32>(t / kSide) };
            const f32 size = 3.0e-4f * std::pow(33.0f, hash(t, 2u));                      // 0.3 mm .. 1 cm
            const f32 height = size * std::pow(10.0f, -6.0f * hash(t, 3u) * hash(t, 3u)); // equilateral-ish .. 1e-6
            const f32 a = 6.2831853f * hash(t, 4u);
            const glm::vec3 along{ std::cos(a), 0.3f * (hash(t, 5u) - 0.5f), std::sin(a) };
            const glm::vec3 e = glm::normalize(along);
            const glm::vec3 f = glm::normalize(glm::cross(e, glm::vec3(0.0f, 1.0f, 0.0f)));
            const u32 base = static_cast<u32>(surface.Positions.size());
            surface.Positions.push_back(centre);
            surface.Positions.push_back(centre + e * size);
            surface.Positions.push_back(centre + e * (0.5f * size) + f * height);
            surface.Indices.insert(surface.Indices.end(), { base, base + 1u, base + 2u });
            // One blend for all three corners, so the skin moves the triangle
            // by one affine map and a sliver stays a sliver.
            const u32 first = t % 3u;
            const f32 w = 0.2f + 0.6f * hash(t, 6u);
            for (u32 v = 0; v < 3u; ++v)
            {
                surface.SetInfluence(base + v, first, w, (first + 1u) % 3u, 1.0f - w);
            }
            const glm::vec3 root = (surface.Positions[base] + surface.Positions[base + 1u] + surface.Positions[base + 2u]) / 3.0f;
            const std::vector<glm::vec3> points{ root, root + glm::vec3(0.0f, 0.004f, 0.0f) };
            const std::vector<f32> widths(points.size(), 1.0e-4f);
            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.GroupId = group;
            ASSERT_TRUE(builder.AddCurve(input, reason)) << reason;
        }
        const Ref<GroomAsset> groom = builder.Build(reason);
        ASSERT_TRUE(groom) << reason;
        Ref<GroomBindingAsset> binding;
        GroomBindingBuildStats stats;
        ASSERT_TRUE(GroomBindingBuilder::Build(*groom, surface.View(3u), "TestBody", GroomBindingBuildSettings{}, binding,
                                               stats, reason))
            << reason;
        std::vector<glm::mat4> palette;
        for (u32 b = 0; b < 3u; ++b)
        {
            const glm::vec3 axis = glm::normalize(glm::vec3(hash(b, 7u) - 0.5f, hash(b, 8u) - 0.5f, hash(b, 9u) - 0.5f));
            palette.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(0.05f * static_cast<f32>(b), 0.02f, -0.03f)) *
                              glm::rotate(glm::mat4(1.0f), glm::radians(20.0f + 30.0f * static_cast<f32>(b)), axis));
        }
        GroomDeformationInputs inputs;
        inputs.Surface = surface.View(3u);
        inputs.Skinning = surface.Skinning(palette, palette, false);
        inputs.SurfaceToGroom = glm::translate(glm::mat4(1.0f), glm::vec3(0.1f, -0.2f, 0.3f)) *
                                glm::rotate(glm::mat4(1.0f), 0.5f, glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f))) *
                                glm::scale(glm::mat4(1.0f), glm::vec3(1.1f, 0.95f, 1.03f));

        TArray<GroomRootTransform> cpu;
        (void)EvaluateGroomRootTransforms(*groom, *binding, inputs, std::nullopt, cpu);
        const KernelRun gpu = RunKernel(*groom, *binding, inputs, false);
        ASSERT_EQ(gpu.Roots.size(), static_cast<sizet>(cpu.Num()));

        u32 trusted = 0;
        u32 distrusted = 0;
        f32 worstShare = 0.0f; // the largest measured error as a share of its tolerance
        f32 largestTrusted = 0.0f;
        for (u32 root = 0; root < static_cast<u32>(cpu.Num()); ++root)
        {
            const GroomRootBinding& record = binding->GetRoot(root);
            const glm::uvec3 corners = inputs.Surface.TriangleIndices(record.TriangleIndex);
            std::array<glm::vec3, 3> posed{};
            for (u32 c = 0; c < 3u; ++c)
            {
                bool weighted = false;
                const u32 v = corners[static_cast<i32>(c)];
                posed[c] = glm::vec3(inputs.SurfaceToGroom *
                                     glm::vec4(SkinGroomSurfaceVertex(inputs.Skinning, v, inputs.Surface.Position(v), palette, weighted), 1.0f));
            }
            const f32 tolerance = GroomCasterFrameTolerance(posed[0], posed[1], posed[2], kGroomCasterPositionTolerance);
            if (!(tolerance <= kGroomCasterMaxFrameError))
            {
                ++distrusted; // no credit, full reach: nothing to hold
                continue;
            }
            ++trusted;
            largestTrusted = std::max(largestTrusted, tolerance);
            const GroomRootTransform& expected = cpu[static_cast<i32>(root)];
            ASSERT_TRUE(expected.Valid) << "root " << root << ": the CPU held a triangle the pose trusts";
            // The relative rotation's vector part, in double: sin(angle / 2),
            // well conditioned near zero where acos of an f32 dot steps by
            // 7e-4 rad.
            const glm::vec4 g = gpu.Roots[root].Rotation;
            const glm::dquat relative = glm::conjugate(glm::dquat(expected.Rotation.w, expected.Rotation.x, expected.Rotation.y,
                                                                  expected.Rotation.z)) *
                                        glm::dquat(g.w, g.x, g.y, g.z);
            const f32 angle = static_cast<f32>(
                2.0 * std::asin(std::min(1.0, glm::length(glm::dvec3(relative.x, relative.y, relative.z)))));
            worstShare = std::max(worstShare, angle / tolerance);
            EXPECT_LE(angle, tolerance) << "root " << root << ": the kernel turned it " << angle
                                        << " rad from the CPU's frame, past the pose's tolerance " << tolerance;
        }
        std::printf("[groom-root-frames] caster tolerance: %u roots trusted (largest tolerance %.4f rad), %u not; the "
                    "kernel's worst disagreement is %.3f of its tolerance\n",
                    trusted, largestTrusted, distrusted, worstShare);
        EXPECT_GT(trusted, kTriangles / 4u) << "too few triangles were trusted for the case to measure anything";
        EXPECT_GT(distrusted, 0u) << "the control: no sliver was thin enough to be distrusted";
        EXPECT_GT(largestTrusted, 1.0e-3f) << "no trusted triangle came near the tolerance's edge";
    }
} // namespace OloEngine::Tests
