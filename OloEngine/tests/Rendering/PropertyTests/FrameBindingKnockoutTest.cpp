// OLO_TEST_LAYER: L4
// =============================================================================
// FrameBindingKnockoutTest.cpp — a frame does not depend on the buffer
// bindings that were left in place before it.
//
// THE BUG CLASS. A UniformBuffer or StorageBuffer claims its binding point
// when it is CONSTRUCTED (notes-renderer.md, "last-created-wins"). A renderer
// buffer that is only ever written with SetData afterwards therefore depends on
// nothing else in the process touching that slot again. Deleting any buffer GL
// has bound at a slot reverts the slot to 0, and so does a test that clears it.
// Shaders then read zeroes, which for a feature block means "disabled": the
// frame renders, just without the feature, and only in a long run.
//
// It has been fixed one buffer at a time: fog (#446), motion blur, and on
// #1511 snow. GLStateGuardTest clears UBO slot 13 on its way out, and every
// snow test after it in a full run rendered bare ground: the snow blur then had
// no snow to change, SnowLayerTest's positive control read 0 pixels, and two
// CrossPath snow rows lost their term.
//
// This test states the contract for every feature the scene turns on, not one
// slot: capture a frame, capture it again as the determinism control, then
// empty EVERY uniform and storage binding point and capture it a third time.
// The third frame must be byte-identical to the first. A slot the frame reads
// but never binds stays empty for all of the third capture's frames, so the
// feature on it drops out of the picture and the comparison fails.
//
// Why pixels and not a binding census: a census flags every buffer bound only
// by its constructor, including features this frame does not run (terrain,
// water, the editor's jump flood), and cannot say which empty slot a shader
// actually reads. The picture can.
//
// Classification: L4 (GPU binding state across frames). SKIPs cleanly with no
// GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBlockContract.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kSize = 160;
        constexpr u32 kFramesPerCapture = 6;
        constexpr f32 kCaptureTime = 4.0f;

        struct BindingTarget
        {
            GLenum Target;       // GL_UNIFORM_BUFFER / GL_SHADER_STORAGE_BUFFER
            GLenum BindingQuery; // GL_*_BUFFER_BINDING
            GLenum MaxBindings;  // GL_MAX_*_BUFFER_BINDINGS
        };

        constexpr BindingTarget kUniform{ GL_UNIFORM_BUFFER, GL_UNIFORM_BUFFER_BINDING, GL_MAX_UNIFORM_BUFFER_BINDINGS };
        constexpr BindingTarget kStorage{ GL_SHADER_STORAGE_BUFFER, GL_SHADER_STORAGE_BUFFER_BINDING,
                                          GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS };

        [[nodiscard]] std::vector<GLint> Snapshot(const BindingTarget& t)
        {
            GLint count = 0;
            glGetIntegerv(t.MaxBindings, &count);
            std::vector<GLint> bound(static_cast<sizet>(std::max(count, 0)), 0);
            for (sizet i = 0; i < bound.size(); ++i)
                glGetIntegeri_v(t.BindingQuery, static_cast<GLuint>(i), &bound[i]);
            return bound;
        }

        // Empties every slot of one target and puts back what was there on
        // every exit path: the test deliberately wipes process-global GL
        // state, and a failed assertion must not hand the next test in the
        // process empty slots — which is this bug.
        class ScopedKnockout
        {
          public:
            explicit ScopedKnockout(const BindingTarget& target) : m_Target(target), m_Saved(Snapshot(target))
            {
                for (sizet i = 0; i < m_Saved.size(); ++i)
                    glBindBufferBase(m_Target.Target, static_cast<GLuint>(i), 0);
            }
            ~ScopedKnockout()
            {
                for (sizet i = 0; i < m_Saved.size(); ++i)
                    glBindBufferBase(m_Target.Target, static_cast<GLuint>(i), static_cast<GLuint>(m_Saved[i]));
            }
            ScopedKnockout(const ScopedKnockout&) = delete;
            ScopedKnockout& operator=(const ScopedKnockout&) = delete;

          private:
            const BindingTarget& m_Target;
            std::vector<GLint> m_Saved;
        };

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        };

        struct FrameDiff
        {
            u64 Changed = 0;
            u32 MaxAbsDiff = 0;
        };

        [[nodiscard]] FrameDiff Compare(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            FrameDiff d;
            for (sizet px = 0; px + 3 < std::min(a.size(), b.size()); px += 4)
            {
                u32 pixelMax = 0;
                for (sizet c = 0; c < 3; ++c)
                    pixelMax = std::max(pixelMax, static_cast<u32>(std::abs(static_cast<int>(a[px + c]) -
                                                                            static_cast<int>(b[px + c]))));
                d.MaxAbsDiff = std::max(d.MaxAbsDiff, pixelMax);
                if (pixelMax > 0)
                    ++d.Changed;
            }
            return d;
        }

        // Both frames of a failed comparison, rows as read back (bottom-up), so
        // the lost feature can be seen rather than inferred from a count.
        void WriteFailureEvidence(const std::string& name, const std::vector<u8>& rgba)
        {
            const std::filesystem::path dir = std::filesystem::path("assets") / "tests" / "visual";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::string file = (dir / (name + ".png")).string();
            const sizet rowBytes = static_cast<sizet>(kSize) * 4u;
            std::vector<u8> topDown(rgba.size());
            for (u32 y = 0; y < kSize; ++y)
                std::copy_n(rgba.data() + static_cast<sizet>(kSize - 1u - y) * rowBytes, rowBytes,
                            topDown.data() + static_cast<sizet>(y) * rowBytes);
            (void)::stbi_write_png(file.c_str(), static_cast<int>(kSize), static_cast<int>(kSize), 4, topDown.data(),
                                   static_cast<int>(rowBytes));
        }

        [[nodiscard]] const char* PathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
                default:
                    return "Other";
            }
        }
    } // namespace

    class FrameBindingKnockout : public RendererAttachedTest, public ::testing::WithParamInterface<RenderingPath>
    {
      protected:
        void TearDown() override
        {
            Renderer3D::GetSnowSettings() = m_SavedSnow;
            Renderer3D::GetFogSettings() = m_SavedFog;
            RendererAttachedTest::TearDown();
        }

        void AddBox(const char* name, const glm::vec3& position, const glm::vec3& scale)
        {
            Entity e = GetScene().CreateEntity(name);
            auto& tc = e.GetComponent<TransformComponent>();
            tc.Translation = position;
            tc.Scale = scale;
            auto& mc = e.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                mc.m_MeshSource = mesh->GetMeshSource();
            auto& mat = e.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.5f, 0.45f, 0.4f, 1.0f));
            mat.m_Material.SetRoughnessFactor(0.7f);
        }

        void BuildScene() override
        {
            m_SavedSnow = Renderer3D::GetSnowSettings();
            m_SavedFog = Renderer3D::GetFogSettings();
            EnableRendering(kSize, kSize);

            auto& rs = Renderer3D::GetRendererSettings();
            rs.Path = GetParam();
            rs.ShowGrid = false;
            // Every feature on here is a block this test protects: its data
            // reaches the shaders only through a buffer binding.
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
            pp.SSAOEnabled = true;
            pp.BloomEnabled = true;
            Renderer3D::ApplyRendererSettings();

            SnowSettings& snow = Renderer3D::GetSnowSettings();
            snow = SnowSettings{};
            snow.Enabled = true;
            snow.HeightStart = 1.0f;
            snow.HeightFull = 1.4f;
            snow.SSSBlurEnabled = true;

            FogSettings& fog = Renderer3D::GetFogSettings();
            fog.Enabled = true;
            fog.Mode = FogMode::ExponentialSquared;
            fog.Color = glm::vec3(0.2f, 0.4f, 0.9f);
            fog.Density = 0.03f;
            fog.HeightFalloff = 0.0f;
            fog.EnableVolumetric = false;

            Entity sun = GetScene().CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(0.35f, -0.55f, -0.75f));
            dl.m_Intensity = 4.0f;

            // Low ground (no snow) and a raised platform above the snow line,
            // with a box on each so SSAO has creases to darken.
            AddBox("Ground", { -5.0f, -0.5f, 0.0f }, { 10.0f, 1.0f, 40.0f });
            AddBox("Platform", { 5.0f, 1.0f, 0.0f }, { 10.0f, 2.0f, 40.0f });
            AddBox("LowBox", { -2.5f, 0.5f, 1.0f }, { 1.0f, 1.0f, 1.0f });
            AddBox("HighBox", { 2.5f, 2.5f, 1.0f }, { 1.0f, 1.0f, 1.0f });
        }

        void Capture(std::vector<u8>& out)
        {
            EditorCamera camera(60.0f, 1.0f, 0.05f, 300.0f);
            camera.SetViewportSize(static_cast<f32>(kSize), static_cast<f32>(kSize));
            camera.SetPose(glm::vec3(0.0f, 5.0f, 11.0f), 0.0f, 0.4f);
            RunEditorFrames(camera, kFramesPerCapture);
            u32 w = 0, h = 0;
            ASSERT_TRUE(ReadbackComposite(out, w, h)) << "no composited frame to read back";
            ASSERT_EQ(w, kSize);
            ASSERT_EQ(h, kSize);
        }

        SnowSettings m_SavedSnow{};
        FogSettings m_SavedFog{};
    };

    TEST_P(FrameBindingKnockout, AFrameDoesNotDependOnTheBufferBindingsLeftBeforeIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        const char* path = PathName(GetParam());

        std::vector<u8> reference;
        Capture(reference);
        ASSERT_FALSE(HasFatalFailure());

        // Determinism control: the same capture again, bindings untouched. If
        // this differs, the comparison below cannot tell a lost binding from
        // frame-to-frame noise, so it would be meaningless.
        std::vector<u8> again;
        Capture(again);
        ASSERT_FALSE(HasFatalFailure());
        const FrameDiff control = Compare(reference, again);
        ASSERT_EQ(control.Changed, 0u) << "on " << path << ", two captures with nothing changed differ in "
                                       << control.Changed << " pixels (max " << control.MaxAbsDiff
                                       << " levels), so this test cannot attribute a difference to a binding";

        std::vector<u8> knockedOut;
        {
            ScopedKnockout uniforms(kUniform);
            ScopedKnockout storage(kStorage);
            Capture(knockedOut);
        }
        ASSERT_FALSE(HasFatalFailure());

        const FrameDiff diff = Compare(reference, knockedOut);
        if (diff.Changed > 0)
        {
            WriteFailureEvidence(std::string("FrameBindingKnockout_") + path + "_Reference", reference);
            WriteFailureEvidence(std::string("FrameBindingKnockout_") + path + "_KnockedOut", knockedOut);
        }
        EXPECT_EQ(diff.Changed, 0u)
            << "on " << path << ", emptying every uniform and storage binding point before the frame changed "
            << diff.Changed << " pixels (max " << diff.MaxAbsDiff << " levels) across " << kFramesPerCapture
            << " frames. A buffer this frame reads relies on the glBindBufferBase its constructor issued, so "
               "anything that clears or re-occupies its slot (a deleted buffer, another test) leaves its shaders "
               "reading zeroes for the rest of the process. Bind it where the frame uploads it, as "
               "RenderPipeline::UploadExecutionState does for fog and motion blur. Compare FrameBindingKnockout_"
            << path << "_Reference.png / _KnockedOut.png.";
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, FrameBindingKnockout,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus,
                                               RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });

    // =========================================================================
    // The dirty predecessor (ADR 0025 section 5a, issue #1330).
    //
    // The knockout above EMPTIES the binding points before a frame. This is the
    // other half: after EVERY render-graph pass, a buffer of another layout is
    // left on the binding points that have no frame owner, the way a pass that
    // shares the point leaves its own block there. A pass that reads such a
    // point without binding its own buffer first then reads the predecessor's.
    // That is #1437: the vegetation dispatch read the skeletal deformer's block.
    //
    // Two things must both hold, and each catches what the other cannot:
    //  * the frame is byte-identical to the unpoisoned one (a pass that read
    //    the poison through an UNDECLARED path still changes the picture: the
    //    poison is 0xFF bytes);
    //  * the binding contract refused nothing (a pass whose reliance happens
    //    not to change this scene's pixels is still named, with its program
    //    and binding, by the violation it raises).
    //
    // Runs in both processes: the poison is bound through UniformBuffer, so it
    // is the same on OpenGL and on Vulkan.
    // =========================================================================
    namespace
    {
        // Large enough for any block, so the only thing wrong with the poison
        // is whose it is: the verdict is WrongLayout, never Undersized.
        constexpr u32 kPoisonBytes = 64u * 1024u;

        // The multi-layout binding points with no frame owner, from the table
        // that defines them (ShaderBindingLayout::IsPassScopedUBOBinding).
        [[nodiscard]] std::vector<u32> PassScopedBindings()
        {
            std::vector<u32> bindings;
            for (u32 binding = 0; binding < ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS; ++binding)
            {
                if (ShaderBindingLayout::IsPassScopedUBOBinding(binding))
                    bindings.push_back(binding);
            }
            return bindings;
        }

        constexpr std::string_view kPoisonHook = "FramePredecessorPoison";
    } // namespace

    class FramePredecessorPoison : public FrameBindingKnockout
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void SetUp() override
        {
            FrameBindingKnockout::SetUp();
            m_RefusedAtSetUp = BindingContract::ViolationCount();
        }

        void TearDown() override
        {
            if (RenderGraph* graph = ActiveGraph())
                graph->RemovePostPassHook(kPoisonHook);
            m_Poison.clear();
            FrameBindingKnockout::TearDown();
        }

        [[nodiscard]] static RenderGraph* ActiveGraph()
        {
            return const_cast<RenderGraph*>(RenderGraphDebugRuntime::GetActiveGraph().Raw());
        }

        // From now on, every pass ends with a buffer of another layout on each
        // of `bindings`.
        void PoisonAfterEveryPass(std::span<const u32> bindings)
        {
            const std::vector<u8> bytes(kPoisonBytes, u8{ 0xFF });
            for (const u32 binding : bindings)
            {
                Ref<UniformBuffer> poison = UniformBuffer::Create(kPoisonBytes, binding, "PredecessorPoison");
                poison->SetData(bytes.data(), kPoisonBytes);
                m_Poison.push_back(std::move(poison));
            }
            RenderGraph* graph = ActiveGraph();
            ASSERT_NE(graph, nullptr) << "no active render graph to hook";
            graph->AddPostPassHook(kPoisonHook, [this](std::string_view, RenderGraph&)
                                   {
                for (const Ref<UniformBuffer>& poison : m_Poison)
                    poison->Bind(); });
        }

        std::vector<Ref<UniformBuffer>> m_Poison;
        u64 m_RefusedAtSetUp = 0;
    };

    TEST_P(FramePredecessorPoison, APassDoesNotReadWhatItsPredecessorLeftOnASharedBinding)
    {
        ScopedMockTime mockTime(kCaptureTime);
        const char* path = PathName(GetParam());

        std::vector<u8> reference;
        Capture(reference);
        ASSERT_FALSE(HasFatalFailure());
        std::vector<u8> again;
        Capture(again);
        ASSERT_FALSE(HasFatalFailure());
        ASSERT_EQ(Compare(reference, again).Changed, 0u) << "on " << path << ", two untouched captures differ";

        const std::vector<u32> passScoped = PassScopedBindings();
        ASSERT_GE(passScoped.size(), 5u) << "the pass-scoped binding table is empty or nearly so";
        const u64 refusedBefore = BindingContract::ViolationCount();
        // The reference frames above ran under the contract too. An ordinary
        // frame is refused nothing: that alone is a regression test for every
        // producer the scene runs (it is how the particle renderer's 64-byte
        // camera buffer under a 384-byte block was found).
        ASSERT_EQ(refusedBefore, m_RefusedAtSetUp)
            << "on " << path << ", an ordinary frame had " << (refusedBefore - m_RefusedAtSetUp)
            << " uniform block read(s) refused before anything was poisoned; see the [BindingContract] error in the log";
        PoisonAfterEveryPass(passScoped);
        ASSERT_FALSE(HasFatalFailure());
        std::vector<u8> poisoned;
        Capture(poisoned);
        ASSERT_FALSE(HasFatalFailure());
        const u64 refused = BindingContract::ViolationCount() - refusedBefore;

        EXPECT_EQ(refused, 0u)
            << "on " << path << ", " << refused
            << " uniform block read(s) found a predecessor's buffer on a shared binding point. The [BindingContract] "
               "error in the log names the program, the binding and both blocks. That pass reads the point without "
               "binding its own buffer first: call Bind() before the draw or dispatch.";
        const FrameDiff diff = Compare(reference, poisoned);
        if (diff.Changed > 0)
        {
            WriteFailureEvidence(std::string("FramePredecessorPoison_") + path + "_Reference", reference);
            WriteFailureEvidence(std::string("FramePredecessorPoison_") + path + "_Poisoned", poisoned);
        }
        EXPECT_EQ(diff.Changed, 0u)
            << "on " << path << ", leaving another layout on the shared binding points after every pass changed "
            << diff.Changed << " pixels (max " << diff.MaxAbsDiff << " levels): a pass's output depends on what ran before it.";
    }

    // The control for the test above: poison a binding point that DOES have a
    // frame owner. The motion-blur matrices at UBO_USER_1 are published once per
    // frame for the G-Buffer, the deferred lighting and the fog pass to read, so
    // a predecessor that leaves another layout there is exactly the fault the
    // contract exists for. It must be refused (the shader is given the zero
    // block, not the 0xFF poison) and the frame must show it: the scene's fog
    // reconstructs position through those matrices on every path.
    TEST_P(FramePredecessorPoison, AnotherLayoutOnAFramePublishedBindingIsRefusedBeforeAnyShaderReadsIt)
    {
        ScopedMockTime mockTime(kCaptureTime);
        const char* path = PathName(GetParam());

        std::vector<u8> reference;
        Capture(reference);
        ASSERT_FALSE(HasFatalFailure());

        const u64 refusedBefore = BindingContract::ViolationCount();
        static constexpr std::array kFramePublished{ ShaderBindingLayout::UBO_USER_1 };
        ASSERT_FALSE(ShaderBindingLayout::IsPassScopedUBOBinding(kFramePublished[0]));
        PoisonAfterEveryPass(kFramePublished);
        ASSERT_FALSE(HasFatalFailure());
        std::vector<u8> poisoned;
        Capture(poisoned);
        ASSERT_FALSE(HasFatalFailure());

        EXPECT_GT(BindingContract::ViolationCount() - refusedBefore, 0u)
            << "on " << path << ", a buffer of another layout sat on a frame-published binding and no read of it was refused";
        EXPECT_GT(Compare(reference, poisoned).Changed, 0u)
            << "on " << path << ", the frame did not change although its motion-blur matrices were replaced: this control would "
                                "not notice a refused read either";
    }

    // What the check costs (issue #1330: "measure validation cost; do not infer
    // that all bindings need unconditional expensive reset"). The same frames
    // with the contract on and with OLO_BINDING_CONTRACT=0, interleaved so that
    // drift lands on both arms, timed on the CPU around the whole frame. A
    // probe, not a budget: it prints the two medians and asserts only that the
    // comparison is one (frames identical, nothing refused). Read the numbers
    // from a Release build on an idle machine.
    class UniformBlockContractPerfProbe : public FramePredecessorPoison
    {
    };

    TEST_P(UniformBlockContractPerfProbe, FrameCpuTimeWithAndWithoutTheCheck)
    {
        ScopedMockTime mockTime(kCaptureTime);
        const char* path = PathName(GetParam());
        constexpr u32 kRounds = 7;
        constexpr u32 kFramesPerRound = 40;

        EditorCamera camera(60.0f, 1.0f, 0.05f, 300.0f);
        camera.SetViewportSize(static_cast<f32>(kSize), static_cast<f32>(kSize));
        camera.SetPose(glm::vec3(0.0f, 5.0f, 11.0f), 0.0f, 0.4f);
        RunEditorFrames(camera, 8); // warm caches, pipelines and histories

        const auto timeRound = [&]() -> f64
        {
            const auto start = std::chrono::steady_clock::now();
            RunEditorFrames(camera, kFramesPerRound);
            return std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - start).count() / kFramesPerRound;
        };

        const Levers::Tristate saved = Levers::BindingContract();
        const u64 refusedBefore = BindingContract::ViolationCount();
        std::vector<f64> on;
        std::vector<f64> off;
        for (u32 round = 0; round < kRounds; ++round)
        {
            Levers::SetBindingContract(Levers::Tristate::On);
            on.push_back(timeRound());
            Levers::SetBindingContract(Levers::Tristate::Off);
            off.push_back(timeRound());
        }
        Levers::SetBindingContract(saved);
        ASSERT_FALSE(HasFatalFailure());
        EXPECT_EQ(BindingContract::ViolationCount(), refusedBefore) << "a refused read makes the two arms draw different frames";

        const auto median = [](std::vector<f64> values)
        {
            std::ranges::sort(values);
            return values[values.size() / 2];
        };
        const f64 onUs = median(on);
        const f64 offUs = median(off);
        std::cout << "[UniformBlockContractPerfProbe] " << TestRenderBackend::EvidenceBackendPrefix() << " " << path
                  << ": frame CPU median " << onUs << " us with the check, " << offUs << " us without ("
                  << (offUs > 0.0 ? (onUs - offUs) / offUs * 100.0 : 0.0) << " %), " << kRounds << " interleaved rounds of "
                  << kFramesPerRound << " frames\n";
        ::testing::Test::RecordProperty("contract_on_us", std::to_string(onUs));
        ::testing::Test::RecordProperty("contract_off_us", std::to_string(offUs));
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, UniformBlockContractPerfProbe,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus,
                                               RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });

    INSTANTIATE_TEST_SUITE_P(AllPaths, FramePredecessorPoison,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus,
                                               RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });
} // namespace OloEngine::Tests
