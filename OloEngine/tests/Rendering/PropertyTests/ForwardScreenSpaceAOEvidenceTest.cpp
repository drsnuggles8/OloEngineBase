// OLO_TEST_LAYER: integration
// =============================================================================
// ForwardScreenSpaceAOEvidenceTest.cpp — issue #1452 on the REAL pipeline:
// screen-space AO (SSAO / GTAO) is visibility for the AMBIENT term on EVERY
// rendering path, and Forward / Forward+ AO looks like Deferred AO.
//
// Before #1452 the forward paths built the AO buffer from the colour pass's own
// normals, after the lighting that needed it, and PostProcess_SSAOApply
// multiplied the COMPOSED colour: direct light, emission and transmission all
// darkened in every crease. The fix runs the depth prepass with normal-mapped
// normals, runs AO between the prepass and forward colour, and has every forward
// shader apply AO to its ambient term only.
//
// Two scenes, each rendered on every path cell x both AO techniques:
//
//   1. NO AMBIENT TERM. Black dielectric surfaces carrying emission, lit by a
//      strong sun. Black albedo zeroes the ambient ladder's fill (0.03 x albedo)
//      while F0 = 0.04 still reflects the sun, so the frame is emission plus
//      direct specular and nothing an ambient visibility may touch. Turning AO
//      on must leave it unchanged; the AO debug view of the same scene is the
//      positive control that the AO buffer holds real occlusion there.
//   2. AMBIENT ONLY. Grey dielectric boxes on a floor with no light and no
//      environment, so the ambient ladder's flat fill is the only light. AO must
//      darken the creases (positive control), and the per-pixel darkening on a
//      forward path must match the Deferred path's (parity).
//
// Path cells: Forward with ForwardPlusAutoSwitch (the default, prepass already
// on), Forward with the auto-switch and the prepass setting OFF (the one
// configuration that had no prepass — AO must force it on), Forward+ and
// Deferred. Evidence PNGs follow the task loop's naming:
// AO<Scene>[Off]_GL_<Path>_<Technique>.png under OloEditor/assets/tests/visual/.
// GL only: the headless fixtures need a GL 4.6 context and skip without one;
// the Vulkan cells are live-verified.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 480;
        constexpr f32 kCaptureTime = 4.0f;
        constexpr u32 kFramesPerCapture = 8; // lets the temporal resolves settle on a static pose

        enum class PathCell : u8
        {
            Forward,          // ForwardPlusAutoSwitch on: the default, prepass already ran
            ForwardNoPrepass, // auto-switch AND the prepass setting off: AO must force the prepass on
            ForwardPlus,
            Deferred,
        };

        struct AOCase
        {
            PathCell Path;
            AOTechnique Technique;
        };

        [[nodiscard]] const char* PathName(PathCell path)
        {
            switch (path)
            {
                case PathCell::Forward:
                    return "Forward";
                case PathCell::ForwardNoPrepass:
                    return "ForwardNoPrepass";
                case PathCell::ForwardPlus:
                    return "ForwardPlus";
                case PathCell::Deferred:
                    return "Deferred";
            }
            return "Unknown";
        }

        [[nodiscard]] const char* TechniqueName(AOTechnique technique)
        {
            return technique == AOTechnique::GTAO ? "GTAO" : "SSAO";
        }

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

        [[nodiscard]] f64 Luma(const u8* p)
        {
            return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        }

        struct FrameDiff
        {
            f64 MeanOff = 0.0;
            f64 MeanAbsDiff = 0.0;
            u32 MaxAbsDiff = 0;
            u64 Darkened = 0; // pixels whose luma dropped by more than 6 levels OFF -> ON
        };

        [[nodiscard]] FrameDiff Compare(const std::vector<u8>& off, const std::vector<u8>& on)
        {
            FrameDiff d;
            const std::size_t pixels = static_cast<std::size_t>(kWidth) * kHeight;
            for (std::size_t i = 0; i < pixels; ++i)
            {
                const u8* a = &off[i * 4u];
                const u8* b = &on[i * 4u];
                d.MeanOff += Luma(a);
                d.MeanAbsDiff += std::abs(Luma(a) - Luma(b));
                if (Luma(a) - Luma(b) > 6.0)
                    ++d.Darkened;
                for (int c = 0; c < 3; ++c)
                    d.MaxAbsDiff = std::max(d.MaxAbsDiff, static_cast<u32>(std::abs(static_cast<int>(a[c]) - static_cast<int>(b[c]))));
            }
            d.MeanOff /= static_cast<f64>(pixels);
            d.MeanAbsDiff /= static_cast<f64>(pixels);
            return d;
        }

        // How much AO darkened each pixel, OFF - ON, in luma levels.
        [[nodiscard]] std::vector<f32> Darkening(const std::vector<u8>& off, const std::vector<u8>& on)
        {
            const std::size_t pixels = static_cast<std::size_t>(kWidth) * kHeight;
            std::vector<f32> out(pixels);
            for (std::size_t i = 0; i < pixels; ++i)
                out[i] = static_cast<f32>(Luma(&off[i * 4u]) - Luma(&on[i * 4u]));
            return out;
        }
    } // namespace

    class ForwardScreenSpaceAOTest : public RendererAttachedTest, public ::testing::WithParamInterface<AOCase>
    {
      protected:
        Entity AddMesh(const char* name, MeshPrimitive primitive, const glm::vec3& position, const glm::vec3& scale,
                       const glm::vec3& albedo, const glm::vec3& emissive)
        {
            Scene& scene = GetScene();
            Entity e = scene.CreateEntity(name);
            auto& tc = e.GetComponent<TransformComponent>();
            tc.Translation = position;
            tc.Scale = scale;
            auto& mc = e.AddComponent<MeshComponent>();
            mc.m_Primitive = primitive;
            Ref<Mesh> mesh = (primitive == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane() : MeshPrimitives::CreateCube();
            if (mesh)
                mc.m_MeshSource = mesh->GetMeshSource();
            auto& mat = e.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            mat.m_Material.SetEmissiveFactor(glm::vec4(emissive, 1.0f));
            mat.m_Material.SetMetallicFactor(0.0f);
            mat.m_Material.SetRoughnessFactor(0.6f);
            return e;
        }

        void AddCreaseGeometry(const glm::vec3& albedo, const glm::vec3& floorEmission, const glm::vec3& boxEmissionA,
                               const glm::vec3& boxEmissionB)
        {
            AddMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 30.0f, 1.0f, 30.0f }, albedo, floorEmission);
            AddMesh("BoxA", MeshPrimitive::Cube, { -1.0f, 1.0f, 0.0f }, { 2.0f, 2.0f, 2.0f }, albedo, boxEmissionA);
            AddMesh("BoxB", MeshPrimitive::Cube, { 1.2f, 0.75f, -0.6f }, { 1.5f, 1.5f, 1.5f }, albedo, boxEmissionB);
            AddMesh("BoxC", MeshPrimitive::Cube, { 0.1f, 0.4f, 1.3f }, { 0.8f, 0.8f, 0.8f }, albedo, boxEmissionB);
        }

        void UsePath(PathCell path, f32 exposure)
        {
            auto& rs = Renderer3D::GetRendererSettings();
            // The editor grid is unlit overlay geometry that covers the floor;
            // it would only dilute every measurement below.
            rs.ShowGrid = false;
            rs.ForwardPlusAutoSwitch = true;
            rs.DepthPrepassEnabled = false;
            switch (path)
            {
                case PathCell::Forward:
                    rs.Path = RenderingPath::Forward;
                    break;
                case PathCell::ForwardNoPrepass:
                    rs.Path = RenderingPath::Forward;
                    rs.ForwardPlusAutoSwitch = false;
                    break;
                case PathCell::ForwardPlus:
                    rs.Path = RenderingPath::ForwardPlus;
                    break;
                case PathCell::Deferred:
                    rs.Path = RenderingPath::Deferred;
                    break;
            }
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.BloomEnabled = false;
            pp.AutoExposureEnabled = false;
            pp.Exposure = exposure;
            SetAO(false);
        }

        void SetAO(bool enabled)
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.ActiveAOTechnique = GetParam().Technique;
            pp.SSAOEnabled = enabled && GetParam().Technique == AOTechnique::SSAO;
            pp.GTAOEnabled = enabled && GetParam().Technique == AOTechnique::GTAO;
            pp.SSAORadius = 1.0f;
            pp.SSAOIntensity = 1.0f;
            pp.GTAORadius = 1.5f;
            pp.SSAODebugView = false;
            pp.GTAODebugView = false;
            Renderer3D::ApplyRendererSettings();
        }

        void SetAODebugView(bool enabled)
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.SSAODebugView = enabled && GetParam().Technique == AOTechnique::SSAO;
            pp.GTAODebugView = enabled && GetParam().Technique == AOTechnique::GTAO;
            Renderer3D::ApplyRendererSettings();
        }

        [[nodiscard]] std::string CellName(const char* feature, bool on, PathCell path) const
        {
            return std::string(feature) + (on ? "" : "Off") + "_GL_" + PathName(path) + "_" +
                   TechniqueName(GetParam().Technique);
        }

        // Render from the pose, read the composited frame back (rows top-down)
        // and write it as `<name>.png` evidence.
        void Capture(const std::string& name, const glm::vec3& position, f32 yaw, f32 pitch, std::vector<u8>& out)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);
            RunEditorFrames(camera, kFramesPerCapture);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            ASSERT_TRUE(fb) << "no composited framebuffer for '" << name << "'";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
            ASSERT_EQ(out.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = out.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bottom = out.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "cannot create " << dir.generic_string();
            const std::string path = (dir / (name + ".png")).string();
            ASSERT_NE(::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4, out.data(),
                                       static_cast<int>(rowBytes)),
                      0)
                << "stbi_write_png failed for " << path;
        }
    };

    std::string AOCaseName(const ::testing::TestParamInfo<AOCase>& info)
    {
        return std::string(PathName(info.param.Path)) + "_" + TechniqueName(info.param.Technique);
    }

    constexpr std::array<AOCase, 8> kAllCells = { {
        { PathCell::Forward, AOTechnique::SSAO },
        { PathCell::Forward, AOTechnique::GTAO },
        { PathCell::ForwardNoPrepass, AOTechnique::SSAO },
        { PathCell::ForwardNoPrepass, AOTechnique::GTAO },
        { PathCell::ForwardPlus, AOTechnique::SSAO },
        { PathCell::ForwardPlus, AOTechnique::GTAO },
        { PathCell::Deferred, AOTechnique::SSAO },
        { PathCell::Deferred, AOTechnique::GTAO },
    } };

    // -------------------------------------------------------------------------
    // 1. No ambient term: emission + direct specular, which AO must not touch.
    // -------------------------------------------------------------------------
    class AOLeavesNonAmbientTermsTest : public ForwardScreenSpaceAOTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            UsePath(GetParam().Path, 1.0f);
            Scene& scene = GetScene();
            Entity sun = scene.CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(0.3f, -0.8f, -0.5f));
            dl.m_Color = glm::vec3(1.0f);
            dl.m_Intensity = 12.0f;
            // Black DIELECTRIC: the ambient fill is 0.03 x albedo = 0, while
            // F0 = 0.04 reflects the sun, so the frame is emission + direct
            // specular and has no ambient term for AO to darken.
            AddCreaseGeometry(glm::vec3(0.0f), glm::vec3(0.35f), glm::vec3(0.2f, 0.4f, 0.7f),
                              glm::vec3(0.7f, 0.4f, 0.2f));
        }
    };

    TEST_P(AOLeavesNonAmbientTermsTest, AOLeavesEmissionAndDirectLightUntouched)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        const PathCell path = GetParam().Path;
        const glm::vec3 pose(0.0f, 3.5f, 6.5f);
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.45f;

        SetAO(false);
        std::vector<u8> off;
        Capture(CellName("AOEmissionDirect", false, path), pose, yaw, pitch, off);
        ASSERT_FALSE(HasFatalFailure());

        SetAO(true);
        // Positive control FIRST: the AO buffer of this very scene holds real
        // occlusion, so an unchanged frame below is not what an AO pass that
        // never ran would also produce.
        SetAODebugView(true);
        std::vector<u8> aoView;
        Capture(CellName("AOEmissionDirectDebug", true, path), pose, yaw, pitch, aoView);
        ASSERT_FALSE(HasFatalFailure());
        SetAODebugView(false);
        f64 darkest = 255.0;
        for (std::size_t i = 0; i < aoView.size(); i += 4u)
            darkest = std::min(darkest, Luma(&aoView[i]));
        EXPECT_LT(darkest, 150.0) << "the AO debug view found no occlusion in the crease scene, so the assertion "
                                     "below would be vacuous";

        std::vector<u8> on;
        Capture(CellName("AOEmissionDirect", true, path), pose, yaw, pitch, on);
        ASSERT_FALSE(HasFatalFailure());

        // THE ORDER THE FORWARD AO RESTS ON, in the production graph this frame
        // ran: the prepass writes depth and view normals, the AO pass reads them,
        // and only then does the colour pass sample the AO buffer. Registered
        // any other way the colour pass would read last frame's AO or none.
        if (path != PathCell::Deferred)
        {
            const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
            ASSERT_TRUE(graph) << "no active render graph after a rendered frame";
            const auto order = graph->GetExecutionOrder();
            const auto indexOf = [&order](std::string_view name) -> std::ptrdiff_t
            {
                const auto it = std::ranges::find_if(order, [name](const FString& n) { return n.ToView() == name; });
                return it == order.end() ? -1 : (it - order.begin());
            };
            const std::ptrdiff_t prepass = indexOf("ScenePrepassPass");
            const std::ptrdiff_t aoPass = indexOf(GetParam().Technique == AOTechnique::GTAO ? "GTAOPass" : "SSAOPass");
            const std::ptrdiff_t scene = indexOf("ScenePass");
            ASSERT_GE(prepass, 0) << "ScenePrepassPass is not in the forward execution order";
            ASSERT_GE(aoPass, 0) << "the AO pass is not in the forward execution order";
            ASSERT_GE(scene, 0) << "ScenePass is not in the forward execution order";
            EXPECT_LT(prepass, aoPass) << "the AO pass runs before the prepass that writes its depth and normals";
            EXPECT_LT(aoPass, scene) << "ScenePass's colour draws run before the AO pass that produces their AO";
        }

        const FrameDiff d = Compare(off, on);
        EXPECT_GT(d.MeanOff, 20.0) << "the emission + sun frame rendered (near-)black";
        EXPECT_LT(d.MeanAbsDiff, 0.5)
            << "TURNING AO ON CHANGED A FRAME WITH NO AMBIENT TERM on " << PathName(path) << " (mean |d luma| "
            << d.MeanAbsDiff << ", max " << d.MaxAbsDiff << ", " << d.Darkened << " pixels darkened by more than 6 "
            << "levels). AO is visibility for the ambient term; it was multiplied into emission or direct light.";
        EXPECT_LE(d.MaxAbsDiff, 4u) << "a crease pixel with no ambient term darkened by " << d.MaxAbsDiff << " levels";
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, AOLeavesNonAmbientTermsTest, ::testing::ValuesIn(kAllCells), AOCaseName);

    // -------------------------------------------------------------------------
    // 2. Ambient only: AO darkens the creases, the same way on every path.
    // -------------------------------------------------------------------------
    class AOAmbientParityTest : public ForwardScreenSpaceAOTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            // No light and no environment: the ambient ladder answers with its
            // flat fill (0.03 x albedo) everywhere. Exposure 8 lifts that into
            // the middle of the 8-bit range so AO's darkening is tens of levels.
            UsePath(PathCell::Deferred, 8.0f);
            AddCreaseGeometry(glm::vec3(0.8f), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f));
        }

        struct Frames
        {
            std::vector<u8> Off;
            std::vector<u8> On;
        };

        void CaptureCell(PathCell path, const glm::vec3& pose, f32 yaw, f32 pitch, const char* angle, Frames& out)
        {
            UsePath(path, 8.0f);
            SetAO(false);
            Capture(CellName("AOAmbient", false, path) + "_" + angle, pose, yaw, pitch, out.Off);
            ASSERT_FALSE(HasFatalFailure());
            SetAO(true);
            Capture(CellName("AOAmbient", true, path) + "_" + angle, pose, yaw, pitch, out.On);
        }
    };

    TEST_P(AOAmbientParityTest, AODarkensTheAmbientTermAsTheDeferredPathDoes)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        const PathCell path = GetParam().Path;
        if (path == PathCell::Deferred)
            GTEST_SKIP() << "Deferred is the reference every other cell is compared against";

        struct Pose
        {
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
            const char* Name;
        };
        const std::array<Pose, 2> poses = { {
            { { 0.0f, 3.5f, 6.5f }, 0.0f, 0.45f, "Front" },
            { { 4.5f, 2.2f, 4.0f }, -0.85f, 0.3f, "Side" },
        } };

        for (const Pose& pose : poses)
        {
            SCOPED_TRACE(pose.Name);
            Frames cell;
            CaptureCell(path, pose.Position, pose.Yaw, pose.Pitch, pose.Name, cell);
            ASSERT_FALSE(HasFatalFailure());
            Frames reference;
            CaptureCell(PathCell::Deferred, pose.Position, pose.Yaw, pose.Pitch, pose.Name, reference);
            ASSERT_FALSE(HasFatalFailure());

            // Positive control on both: AO darkened the creases.
            const FrameDiff cellDiff = Compare(cell.Off, cell.On);
            const FrameDiff refDiff = Compare(reference.Off, reference.On);
            EXPECT_GT(refDiff.MeanOff, 20.0) << "the ambient-only frame rendered (near-)black on Deferred";
            EXPECT_GT(refDiff.Darkened, 500u) << "AO darkened almost nothing on Deferred, so parity is vacuous";
            EXPECT_GT(cellDiff.Darkened, 500u) << "AO darkened almost nothing on " << PathName(path);

            // Parity: the same pixels darken by the same amount. The darkening
            // is compared rather than the frames, so any unrelated difference
            // between the two paths' lighting cancels out of the measurement.
            const std::vector<f32> cellDark = Darkening(cell.Off, cell.On);
            const std::vector<f32> refDark = Darkening(reference.Off, reference.On);
            f64 meanDelta = 0.0;
            f64 refMass = 0.0;
            u64 disagreeing = 0;
            for (std::size_t i = 0; i < cellDark.size(); ++i)
            {
                const f64 delta = std::abs(static_cast<f64>(cellDark[i]) - refDark[i]);
                meanDelta += delta;
                refMass += std::abs(static_cast<f64>(refDark[i]));
                if (delta > 8.0)
                    ++disagreeing;
            }
            meanDelta /= static_cast<f64>(cellDark.size());
            refMass /= static_cast<f64>(cellDark.size());
            EXPECT_LT(meanDelta, 0.15 * refMass + 0.25)
                << PathName(path) << " AO DARKENS DIFFERENTLY FROM DEFERRED: mean |cell - deferred| darkening "
                << meanDelta << " levels against a mean Deferred darkening of " << refMass << ". Compare the "
                << "AOAmbient*_" << PathName(path) << " and AOAmbient*_Deferred PNGs.";
            EXPECT_LT(disagreeing, cellDark.size() / 100u)
                << disagreeing << " pixels darken by more than 8 levels differently from Deferred";
        }
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, AOAmbientParityTest, ::testing::ValuesIn(kAllCells), AOCaseName);
} // namespace OloEngine::Tests
