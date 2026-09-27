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

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
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
} // namespace OloEngine::Tests
