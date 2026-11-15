// OLO_TEST_LAYER: L8
// =============================================================================
// InstanceColourEvidenceTest.cpp — an instance's colour reaches the frame
// (issue #1691).
//
// THE BUG. InstancedMeshComponent instances carry a Color. The scene file
// stores it, the batcher carries it, it is uploaded per instance as
// InstanceData::Color, and no shader read it: every instanced mesh drew with
// its material's colour and the authored one was dropped without a message.
// Above the GPU-cull threshold it was dropped a second time, earlier:
// Renderer3D::SubmitGPUCulledInstanced built the records the cull compacts
// from the transforms alone, so the colour, the picking ID, the free float and
// the lightmap region of a 1024-instance batch never reached the instance
// buffer at all.
//
// WHAT THIS PINS. One scene, two instanced fields of touching cubes with the
// default material, so both submission routes run in one frame:
//   * left field, 64 instances   -> Renderer3D::DrawMeshInstanced (CPU cull);
//   * right field, 1156 instances -> Renderer3D::SubmitGPUCulledInstanced
//     (GPU compute cull, where the backend has one).
// Each field is split down the middle into two colours. A field that is one
// colour throughout would also pass if the shader read instance 0's colour for
// every fragment, and the GPU cull reorders the instances it keeps, so only a
// split shows that a fragment finds its own instance's colour.
//
// The control is the same scene with every colour left at the default: it has
// to be neutral, or the hues below are the light's or the material's.
//
// The colour's alpha is not applied (InstanceData.h says why), so a frame with
// every instance alpha at 0.25 must be the frame with it at 1, pixel for pixel.
//
// Evidence: OloEditor/assets/tests/visual/InstanceColour_<Backend>_<Path>.png
// and ..._Default.png. Runs in the --olo-rhi=vulkan process too.
//
// Classification: L8 (full pipeline through the real Scene render path,
// readback + PNG). SKIPs cleanly without a GPU context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"

#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kSize = 256;
        constexpr u32 kFramesPerCapture = 6;
        constexpr f32 kCaptureTime = 4.0f;

        // Field layout. The camera looks down -Z from z = kCameraDistance with a
        // 60 degree vertical field of view, so the frame is 2 * d * tan(30)
        // world units across and the two field centres land on the quarter
        // lines of the frame.
        constexpr f32 kCameraDistance = 8.0f;
        constexpr f32 kFieldSpan = 3.6f;
        constexpr i32 kCpuCulledGrid = 8;  // 64 instances: below the GPU-cull threshold (1024)
        constexpr i32 kGpuCulledGrid = 34; // 1156 instances: above it

        // Every tint keeps a channel at 1 and takes another to 0.05, which is
        // what the assertions below predict from: a kept channel does not move,
        // a removed one falls well below half of it.
        const glm::vec3 kRed(1.0f, 0.05f, 0.05f);
        const glm::vec3 kGreen(0.05f, 1.0f, 0.05f);
        const glm::vec3 kBlue(0.05f, 0.05f, 1.0f);
        const glm::vec3 kYellow(1.0f, 1.0f, 0.05f);

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

        // Rows arrive bottom-up; the PNG is written top-down.
        void WriteEvidence(const std::string& name, const std::vector<u8>& rgba)
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
            EXPECT_NE(::stbi_write_png(file.c_str(), static_cast<int>(kSize), static_cast<int>(kSize), 4, topDown.data(),
                                       static_cast<int>(rowBytes)),
                      0)
                << "could not write " << file;
        }

        // Mean 8-bit colour of a square centred on (cx, cy). The fields are
        // centred on the frame's horizontal midline, so no caller depends on
        // which way up the rows are.
        [[nodiscard]] glm::vec3 MeanColour(const std::vector<u8>& rgba, u32 cx, u32 cy, u32 halfExtent)
        {
            glm::dvec3 sum(0.0);
            u32 count = 0;
            for (u32 y = cy - halfExtent; y < cy + halfExtent; ++y)
            {
                for (u32 x = cx - halfExtent; x < cx + halfExtent; ++x)
                {
                    const sizet at = (static_cast<sizet>(y) * kSize + x) * 4u;
                    sum += glm::dvec3(rgba[at], rgba[at + 1], rgba[at + 2]);
                    ++count;
                }
            }
            return glm::vec3(sum / static_cast<f64>(count));
        }

        [[nodiscard]] u32 CountChangedPixels(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            u32 changed = 0;
            for (sizet px = 0; px + 3 < std::min(a.size(), b.size()); px += 4)
            {
                if (a[px] != b[px] || a[px + 1] != b[px + 1] || a[px + 2] != b[px + 2])
                    ++changed;
            }
            return changed;
        }

        [[nodiscard]] std::string Text(const glm::vec3& v)
        {
            return "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
        }

        // Pins the engine clock for a capture and puts back whatever was there.
        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
                : m_HadMock(Time::HasMockTime()), m_Previous(Time::GetMockTime())
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                if (m_HadMock)
                    Time::SetMockTime(m_Previous);
                else
                    Time::ClearMockTime();
            }
            ScopedMockTime(const ScopedMockTime&) = delete;
            ScopedMockTime& operator=(const ScopedMockTime&) = delete;

          private:
            bool m_HadMock;
            f32 m_Previous;
        };
    } // namespace

    class InstanceColourEvidence : public RendererAttachedTest, public ::testing::WithParamInterface<RenderingPath>
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        // A gridDim x gridDim wall of touching cubes facing the camera, centred
        // on (centreX, 0, 0). Touching, so a probe inside a half reads cube
        // faces and no background.
        Entity AddField(const char* name, i32 gridDim, f32 centreX)
        {
            Entity field = GetScene().CreateEntity(name);
            auto& imc = field.AddComponent<InstancedMeshComponent>();
            if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                imc.MeshSource = cube->GetMeshSource();
            imc.CastShadows = false;
            const f32 pitch = kFieldSpan / static_cast<f32>(gridDim);
            imc.Instances.Reserve(gridDim * gridDim);
            for (i32 gy = 0; gy < gridDim; ++gy)
            {
                for (i32 gx = 0; gx < gridDim; ++gx)
                {
                    const f32 x = centreX + (static_cast<f32>(gx) + 0.5f) * pitch - 0.5f * kFieldSpan;
                    const f32 y = (static_cast<f32>(gy) + 0.5f) * pitch - 0.5f * kFieldSpan;
                    InstanceData inst;
                    inst.Transform = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 0.0f)), glm::vec3(pitch));
                    inst.PrevTransform = inst.Transform;
                    imc.Instances.Add(inst);
                }
            }
            return field;
        }

        // The left half of a field (gx below the middle column) takes
        // `leftHalf`, the rest `rightHalf`. AddField fills row by row, so the
        // column of instance i is i % gridDim.
        static void Paint(Entity field, i32 gridDim, const glm::vec4& leftHalf, const glm::vec4& rightHalf)
        {
            auto& instances = field.GetComponent<InstancedMeshComponent>().Instances;
            for (i32 i = 0; i < instances.Num(); ++i)
                instances[i].Color = (i % gridDim) < gridDim / 2 ? leftHalf : rightHalf;
        }

        void PaintBoth(f32 alpha)
        {
            Paint(m_CpuCulled, kCpuCulledGrid, glm::vec4(kRed, alpha), glm::vec4(kGreen, alpha));
            Paint(m_GpuCulled, kGpuCulledGrid, glm::vec4(kBlue, alpha), glm::vec4(kYellow, alpha));
        }

        void BuildScene() override
        {
            EnableRendering(kSize, kSize);

            auto& rs = Renderer3D::GetRendererSettings();
            rs.Path = GetParam();
            rs.ShowGrid = false;
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
            Renderer3D::ApplyRendererSettings();

            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, kCameraDistance };
            auto& cam = camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetPerspective(glm::radians(60.0f), 0.05f, 100.0f);
            cam.Camera.SetViewportSize(kSize, kSize);

            // White light, straight at the wall of cubes: a hue in the frame is
            // then the surface's.
            Entity sun = GetScene().CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(0.15f, -0.2f, -1.0f));
            dl.m_Color = glm::vec3(1.0f);
            dl.m_Intensity = 1.0f;

            const f32 halfWidth = kCameraDistance * std::tan(glm::radians(30.0f));
            m_CpuCulled = AddField("CpuCulledField", kCpuCulledGrid, -0.5f * halfWidth);
            m_GpuCulled = AddField("GpuCulledField", kGpuCulledGrid, 0.5f * halfWidth);
        }

        // Through the scene's own camera, not the editor's: an editor frame
        // draws an icon over every light and camera, and one of them lands in a
        // probe.
        void Capture(std::vector<u8>& out)
        {
            RunFrames(kFramesPerCapture);
            u32 w = 0, h = 0;
            ASSERT_TRUE(ReadbackComposite(out, w, h)) << "no composited frame to read back";
            ASSERT_EQ(w, kSize);
            ASSERT_EQ(h, kSize);
        }

        // Probe centres, in pixels. A field centre sits on a quarter line and a
        // field is kFieldSpan / (2 * halfWidth) of the frame wide, so the middle
        // of each half is a quarter of that width either side of it.
        struct Probes
        {
            u32 CpuLeft, CpuRight, GpuLeft, GpuRight, Row, HalfExtent;
        };
        [[nodiscard]] static Probes MakeProbes()
        {
            const f32 halfWidth = kCameraDistance * std::tan(glm::radians(30.0f));
            const f32 fieldPixels = kFieldSpan / (2.0f * halfWidth) * static_cast<f32>(kSize);
            const auto quarter = static_cast<u32>(fieldPixels * 0.25f);
            return Probes{
                .CpuLeft = kSize / 4u - quarter,
                .CpuRight = kSize / 4u + quarter,
                .GpuLeft = 3u * kSize / 4u - quarter,
                .GpuRight = 3u * kSize / 4u + quarter,
                .Row = kSize / 2u,
                .HalfExtent = static_cast<u32>(fieldPixels * 0.125f),
            };
        }

        Entity m_CpuCulled;
        Entity m_GpuCulled;
    };

    TEST_P(InstanceColourEvidence, AnInstancesColourTintsItsOwnInstanceOnBothSubmissionRoutes)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        const std::string name = TestRenderBackend::EvidenceName("InstanceColour") + PathName(GetParam());
        const Probes probes = MakeProbes();

        std::vector<u8> plain;
        Capture(plain);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(name + "_Default", plain);

        PaintBoth(1.0f);
        std::vector<u8> tinted;
        Capture(tinted);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(name, tinted);

        // The right-hand field is evidence for the compute-cull route only if
        // it took it. The route is chosen from the batch size, the culling
        // switches and the culler existing, none of which this test sets.
        EXPECT_GE(Renderer3D::GetStats().GPUCulledInstanceBatches, 1u)
            << "no instanced batch went through SubmitGPUCulledInstanced in the last frame, so the 1156-instance "
            << "field was CPU-culled and nothing here covers the lane copy on the compute-cull route";

        struct Region
        {
            const char* Name;
            u32 Column;
            // The channels the tint keeps, and the one it removes.
            std::array<int, 2> Kept;
            int Removed;
        };
        // Red and green keep one channel each; the second "kept" entry repeats
        // it. Yellow keeps two.
        const std::array<Region, 4> regions{ {
            { "CPU-culled field, red half", probes.CpuLeft, { 0, 0 }, 2 },
            { "CPU-culled field, green half", probes.CpuRight, { 1, 1 }, 2 },
            { "GPU-culled field, blue half", probes.GpuLeft, { 2, 2 }, 0 },
            { "GPU-culled field, yellow half", probes.GpuRight, { 0, 1 }, 2 },
        } };

        for (const Region& region : regions)
        {
            const glm::vec3 before = MeanColour(plain, region.Column, probes.Row, probes.HalfExtent);
            const glm::vec3 after = MeanColour(tinted, region.Column, probes.Row, probes.HalfExtent);
            std::cout << "[instance-colour] " << name << " " << region.Name << ": default " << Text(before)
                      << " tinted " << Text(after) << "\n";

            // The control: lit, and neutral. Without this the hue below could be
            // the light's, the material's or the clear colour's.
            const f32 brightest = std::max({ before.r, before.g, before.b });
            const f32 darkest = std::min({ before.r, before.g, before.b });
            EXPECT_GT(darkest, 30.0f) << region.Name << ": the default-colour field is not lit, so it shows no hue";
            EXPECT_LT(brightest - darkest, 16.0f)
                << region.Name << ": the default-colour field is not neutral: " << Text(before);

            // The tint. A channel it keeps at 1 is the control's, since a
            // multiply by 1 changes nothing; and it stays at least twice the
            // channel the tint takes to 0.05, which an untinted region (equal
            // channels) and a region with another instance's tint both fail.
            for (const int kept : region.Kept)
            {
                EXPECT_NEAR(after[kept], before[kept], 1.5f)
                    << region.Name << ": channel " << kept << " has a tint of 1 and should not have moved";
                EXPECT_GT(after[kept], 2.0f * after[region.Removed])
                    << region.Name << ": channel " << kept << " should dominate channel " << region.Removed
                    << " under this instance's colour, but the region reads " << Text(after)
                    << ". The instance colour did not reach these fragments; see " << name << ".png";
            }
        }
    }

    TEST_P(InstanceColourEvidence, AnInstancesAlphaChangesNoPixel)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        const char* path = PathName(GetParam());

        PaintBoth(1.0f);
        std::vector<u8> opaque;
        Capture(opaque);
        ASSERT_FALSE(HasFatalFailure());

        // Determinism control: the same capture again. If this differs, the
        // comparison below cannot attribute a difference to the alpha.
        std::vector<u8> again;
        Capture(again);
        ASSERT_FALSE(HasFatalFailure());
        ASSERT_EQ(CountChangedPixels(opaque, again), 0u)
            << "on " << path << ", two captures with nothing changed differ, so this test cannot attribute a "
            << "difference to the instance alpha";

        PaintBoth(0.25f);
        std::vector<u8> faded;
        Capture(faded);
        ASSERT_FALSE(HasFatalFailure());

        EXPECT_EQ(CountChangedPixels(opaque, faded), 0u)
            << "on " << path << ", setting every instance colour's alpha to 0.25 changed the frame. The alpha of an "
            << "instance colour is not applied (InstanceData.h): applying it in the colour pass alone would leave the "
            << "depth prepass, the shadow programs and the ray-traced alpha test disagreeing with it.";
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, InstanceColourEvidence,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus,
                                               RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });
} // namespace OloEngine::Tests
