// OLO_TEST_LAYER: L8
// =============================================================================
// ShadowMapWithoutCastersTest.cpp — a shadow map that draws no caster reads as
// lit.
//
// THE BUG. A light that requested shadows has its map sampled by every receiver
// that frame, whatever the shadow pass did. ShadowRenderPass returned without
// touching the map when every caster list was empty, and skipped any cascade no
// caster reached, so the map kept what it held before:
//   * the shadow of a caster that had just been deleted, or had walked away,
//     frozen on the ground;
//   * the previous scene's shadows after a scene change;
//   * in a process that had never drawn a caster, the texture's initial zeroes,
//     under which every receiver of the sun is fully shadowed.
// Every scene and every test happened to keep a casting ground plane in every
// cascade, which is what hid it. It surfaced in an evidence scene with a sun
// and no caster, rendering at a fifth of its brightness.
//
// WHAT THIS PINS. A non-casting ground under a slanted sun, seen from above,
// with one casting slab over it. The slab's shadow is measured first, so the
// probe is known to sit in it. Then the slab stops casting, two ways, and the
// probe must return to the lit ground's level each time:
//   * its instances are removed: no caster is submitted at all;
//   * it is moved far outside every cascade: a caster is submitted, and no
//     cascade contains it;
// and a scene that never had a caster is as bright with the sun's shadows on as
// with them off.
//
// Runs on Forward, Forward+ and Deferred, with the directional shadow on the
// cascades and on the virtual shadow map's pages, and in the --olo-rhi=vulkan
// process. The page route had the same hole one level up: the page work that
// retires a departed caster's shadow ran only if the pass ran.
// Classification: L8 (full pipeline through the real Scene render path). SKIPs
// cleanly without a GPU context.
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
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
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

        // The camera looks straight down from kCameraHeight with a 60 degree
        // field of view; +X is to the right of the frame. The sun travels along
        // (1, -1, 0), so a slab kSlabHeight above the ground drops its shadow
        // kSlabHeight further along +X, clear of the slab itself.
        constexpr f32 kCameraHeight = 10.0f;
        constexpr f32 kSlabHeight = 3.0f;

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

        // Mean 8-bit level (all three channels) of a square on the frame's
        // horizontal midline, centred on the ground point (worldX, 0, 0).
        [[nodiscard]] f32 GroundLevel(const std::vector<u8>& rgba, f32 worldX)
        {
            const f32 halfWidth = kCameraHeight * std::tan(glm::radians(30.0f));
            const auto cx = static_cast<u32>((0.5f + 0.5f * worldX / halfWidth) * static_cast<f32>(kSize));
            const u32 cy = kSize / 2u;
            constexpr u32 kHalfExtent = 8;
            f64 sum = 0.0;
            u32 count = 0;
            for (u32 y = cy - kHalfExtent; y < cy + kHalfExtent; ++y)
            {
                for (u32 x = cx - kHalfExtent; x < cx + kHalfExtent; ++x)
                {
                    const sizet at = (static_cast<sizet>(y) * kSize + x) * 4u;
                    sum += static_cast<f64>(rgba[at]) + rgba[at + 1] + rgba[at + 2];
                    count += 3;
                }
            }
            return static_cast<f32>(sum / static_cast<f64>(count));
        }

        // Where the slab's shadow falls, and a point of bare ground the same
        // distance the other side of the frame's centre.
        constexpr f32 kShadowX = kSlabHeight;
        constexpr f32 kLitX = -kSlabHeight;

        // One cell: a render path, and which technique holds the sun's shadow.
        struct Cell
        {
            RenderingPath Path;
            bool VirtualPages;
        };

        [[nodiscard]] std::string CellName(const Cell& cell)
        {
            return std::string(PathName(cell.Path)) + (cell.VirtualPages ? "_VSM" : "");
        }
    } // namespace

    class ShadowMapWithoutCasters : public RendererAttachedTest, public ::testing::WithParamInterface<Cell>
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void TearDown() override
        {
            if (m_ShadowSettingsSaved)
                Renderer3D::GetShadowMap().SetSettings(m_SavedShadowSettings);
            RendererAttachedTest::TearDown();
        }

        // False when this cell asks for the virtual shadow map and the backend
        // refused it; the case then skips, saying so.
        [[nodiscard]] bool TechniqueIsUp() const
        {
            return !GetParam().VirtualPages || Renderer3D::GetShadowMap().IsVirtualShadowMapActive();
        }

        Entity AddSlab(const char* name, const glm::vec3& centre, const glm::vec3& scale, bool castsShadows)
        {
            Entity slab = GetScene().CreateEntity(name);
            auto& imc = slab.AddComponent<InstancedMeshComponent>();
            if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                imc.MeshSource = cube->GetMeshSource();
            imc.CastShadows = castsShadows;
            InstanceData inst;
            inst.Transform = glm::scale(glm::translate(glm::mat4(1.0f), centre), scale);
            inst.PrevTransform = inst.Transform;
            imc.Instances.Add(inst);
            return slab;
        }

        void BuildScene() override
        {
            EnableRendering(kSize, kSize);

            auto& rs = Renderer3D::GetRendererSettings();
            rs.Path = GetParam().Path;
            rs.ShowGrid = false;

            m_SavedShadowSettings = Renderer3D::GetShadowMap().GetSettings();
            m_ShadowSettingsSaved = true;
            ShadowSettings shadow = m_SavedShadowSettings;
            shadow.VSM.Enabled = GetParam().VirtualPages;
            Renderer3D::GetShadowMap().SetSettings(shadow);
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
            Renderer3D::ApplyRendererSettings();

            Entity camera = GetScene().CreateEntity("Camera");
            auto& cameraTransform = camera.GetComponent<TransformComponent>();
            cameraTransform.Translation = { 0.0f, kCameraHeight, 0.0f };
            cameraTransform.SetRotationEuler({ glm::radians(-90.0f), 0.0f, 0.0f });
            auto& cam = camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetPerspective(glm::radians(60.0f), 0.05f, 100.0f);
            cam.Camera.SetViewportSize(kSize, kSize);

            m_Sun = GetScene().CreateEntity("Sun");
            auto& dl = m_Sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(1.0f, -1.0f, 0.0f));
            dl.m_Color = glm::vec3(1.0f);
            dl.m_Intensity = 1.0f;
            dl.m_CastShadows = true;

            // The ground receives and does not cast: with the slab gone the
            // scene has no caster of any kind.
            (void)AddSlab("Ground", { 0.0f, -0.1f, 0.0f }, { 24.0f, 0.2f, 24.0f }, false);
        }

        void AddCastingSlab()
        {
            m_Slab = AddSlab("Slab", { 0.0f, kSlabHeight, 0.0f }, { 2.0f, 0.2f, 2.0f }, true);
        }

        void Capture(std::vector<u8>& out)
        {
            RunFrames(kFramesPerCapture);
            u32 w = 0, h = 0;
            ASSERT_TRUE(ReadbackComposite(out, w, h)) << "no composited frame to read back";
            ASSERT_EQ(w, kSize);
            ASSERT_EQ(h, kSize);
        }

        // Captures the frame with the slab casting and checks the probe sits in
        // its shadow, so that "the probe is lit again" below means the shadow
        // left and not that it was never there. Returns the lit ground's level.
        [[nodiscard]] f32 CaptureTheSlabsShadow(const std::string& name)
        {
            std::vector<u8> frame;
            Capture(frame);
            if (HasFatalFailure())
                return 0.0f;
            WriteEvidence(name + "_Casting", frame);
            const f32 lit = GroundLevel(frame, kLitX);
            const f32 shadowed = GroundLevel(frame, kShadowX);
            std::cout << "[shadow-without-casters] " << name << " casting: lit ground " << lit << ", under the slab's shadow "
                      << shadowed << "\n";
            EXPECT_GT(lit, 40.0f) << "the ground is not lit by the sun, so a shadow cannot be told from it";
            EXPECT_LT(shadowed, 0.7f * lit) << "the slab casts no shadow where this test expects one";
            return lit;
        }

        void ExpectTheShadowHasLeft(const std::string& name, const char* how)
        {
            std::vector<u8> frame;
            Capture(frame);
            ASSERT_FALSE(HasFatalFailure());
            WriteEvidence(name, frame);
            const f32 lit = GroundLevel(frame, kLitX);
            const f32 formerShadow = GroundLevel(frame, kShadowX);
            std::cout << "[shadow-without-casters] " << name << ": lit ground " << lit << ", where the shadow was "
                      << formerShadow << "\n";
            EXPECT_NEAR(formerShadow, lit, 4.0f)
                << how << ", and its shadow is still on the ground. A shadow map that draws no caster has to be "
                << "cleared, or its receivers go on reading the last frame that drew one. See " << name << ".png";
        }

        Entity m_Sun;
        Entity m_Slab;
        ShadowSettings m_SavedShadowSettings{};
        bool m_ShadowSettingsSaved = false;
    };

    TEST_P(ShadowMapWithoutCasters, ADeletedCastersShadowLeavesWithIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!TechniqueIsUp())
            GTEST_SKIP() << "the virtual shadow map did not come up on this backend";
        ScopedMockTime mockTime(kCaptureTime);
        const std::string name = TestRenderBackend::EvidenceName("ShadowWithoutCasters") + CellName(GetParam()) + "_Deleted";

        AddCastingSlab();
        (void)CaptureTheSlabsShadow(name);
        ASSERT_FALSE(HasFatalFailure());

        // No instance, so nothing is drawn and nothing is submitted to cast.
        m_Slab.GetComponent<InstancedMeshComponent>().Instances.Reset();
        ExpectTheShadowHasLeft(name, "the only shadow caster was deleted");
    }

    TEST_P(ShadowMapWithoutCasters, ACasterThatLeavesEveryCascadeTakesItsShadowWithIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!TechniqueIsUp())
            GTEST_SKIP() << "the virtual shadow map did not come up on this backend";
        ScopedMockTime mockTime(kCaptureTime);
        const std::string name = TestRenderBackend::EvidenceName("ShadowWithoutCasters") + CellName(GetParam()) + "_MovedAway";

        AddCastingSlab();
        (void)CaptureTheSlabsShadow(name);
        ASSERT_FALSE(HasFatalFailure());

        // Still a caster, still submitted; five kilometres outside every cascade.
        auto& instance = m_Slab.GetComponent<InstancedMeshComponent>().Instances[0];
        instance.Transform = glm::translate(glm::mat4(1.0f), glm::vec3(5000.0f, 0.0f, 0.0f)) * instance.Transform;
        instance.PrevTransform = instance.Transform;
        ExpectTheShadowHasLeft(name, "the only shadow caster moved outside every cascade");
    }

    TEST_P(ShadowMapWithoutCasters, ASceneThatNeverHadACasterIsLitTheSameWithShadowsOnAsOff)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!TechniqueIsUp())
            GTEST_SKIP() << "the virtual shadow map did not come up on this backend";
        ScopedMockTime mockTime(kCaptureTime);
        const std::string name = TestRenderBackend::EvidenceName("ShadowWithoutCasters") + CellName(GetParam()) + "_Never";

        std::vector<u8> shadowsOn;
        Capture(shadowsOn);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(name, shadowsOn);

        m_Sun.GetComponent<DirectionalLightComponent>().m_CastShadows = false;
        std::vector<u8> shadowsOff;
        Capture(shadowsOff);
        ASSERT_FALSE(HasFatalFailure());

        for (const f32 x : { kLitX, 0.0f, kShadowX })
        {
            const f32 on = GroundLevel(shadowsOn, x);
            const f32 off = GroundLevel(shadowsOff, x);
            std::cout << "[shadow-without-casters] " << name << " ground at x=" << x << ": shadows on " << on
                      << ", shadows off " << off << "\n";
            EXPECT_GT(off, 40.0f) << "the ground is not lit by the sun with its shadows off";
            EXPECT_NEAR(on, off, 2.0f)
                << "with no caster in the scene the sun's shadow map has nothing in it, so the ground at x=" << x
                << " should be exactly as bright as with the sun's shadows off. See " << name << ".png";
        }
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, ShadowMapWithoutCasters,
                             ::testing::Values(Cell{ RenderingPath::Forward, false }, Cell{ RenderingPath::ForwardPlus, false },
                                               Cell{ RenderingPath::Deferred, false }, Cell{ RenderingPath::Forward, true },
                                               Cell{ RenderingPath::ForwardPlus, true },
                                               Cell{ RenderingPath::Deferred, true }),
                             [](const ::testing::TestParamInfo<Cell>& info)
                             { return CellName(info.param); });
} // namespace OloEngine::Tests
