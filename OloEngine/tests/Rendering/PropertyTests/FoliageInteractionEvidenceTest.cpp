// OLO_TEST_LAYER: L8
// Full production raster pipeline for issue #1238: an actor walks into grass,
// the grass bends, the actor leaves, the grass stands back up.
//
// WHY THE RECOVERY ARM IS THE POINT. A bend is easy to see and easy to get
// right; what a CPU test cannot see is a bend that never fully goes away. So
// this test does not only assert "bent differs from not-bent" — it asserts that
// almost none of the pixels the bend moved are still moved once the influence
// is gone, against the same control frame. A permanent deformation passes every
// other assertion here and fails that one. See CountMovedPixels for why the
// measure is a per-pixel count and not whole-frame RMSE.
//
// Wind is OFF throughout, so the only thing that can move a plant is the
// influence field. That is what makes "the frames differ" attributable.
#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "VisualEvidenceGuards.h"
#include "RendererStateCheck.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageInteraction.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <glad/gl.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;
        constexpr const char* kFoliageAlbedo = "assets/textures/grass.png";

        // The patch the actor walks through, in world units. Everything is
        // sized around it: the camera sits just outside it and the influence
        // radius covers a good share of the plants on screen, so a bend is tens
        // of thousands of pixels rather than a few hundred.
        constexpr f32 kPatchX = 128.0f;
        constexpr f32 kPatchZ = 128.0f;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

        // How many pixels moved by more than `threshold`, rather than how much
        // the whole frame moved on average.
        //
        // WHOLE-FRAME RMSE IS THE WRONG INSTRUMENT HERE, and measuring it first
        // is what showed why: between two captures taken seconds apart the
        // frame drifts globally by a fraction of a level (auto-exposure and the
        // temporal history both ride the scene clock), and after three seconds
        // of recovery that drift is LARGER than the bend it is supposed to be
        // compared against — 2.02 against 1.75, which reads as "the bend never
        // recovered" when the bend had in fact gone completely. A per-pixel
        // count with a threshold is immune: the drift is below it everywhere,
        // and the bend is 23k pixels at up to 83 levels.
        [[nodiscard]] sizet CountMovedPixels(const std::vector<u8>& a, const std::vector<u8>& b, u32 threshold)
        {
            if (a.size() != b.size())
                return 0;
            sizet moved = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                const u32 delta = std::max({ static_cast<u32>(std::abs(static_cast<i32>(a[i]) - static_cast<i32>(b[i]))),
                                             static_cast<u32>(std::abs(static_cast<i32>(a[i + 1]) - static_cast<i32>(b[i + 1]))),
                                             static_cast<u32>(std::abs(static_cast<i32>(a[i + 2]) - static_cast<i32>(b[i + 2]))) });
                if (delta > threshold)
                    ++moved;
            }
            return moved;
        }
    } // namespace

    class FoliageInteractionEvidenceTest : public RendererAttachedTest
    {
      protected:
        RendererState::Snapshot m_SavedState;

        void TearDown() override
        {
            // The field is process-wide state, exactly like WindSystem's. A
            // residual left here is a bend in the NEXT test's frame, and the
            // cross-test-renderer-state rule exists because that has happened.
            FoliageInteractionField::Reset();
            Time::ClearMockTime();
            RendererAttachedTest::TearDown();
            RendererState::Restore(m_SavedState);
        }

        void BuildScene() override
        {
            ASSERT_TRUE(RendererState::Capture(m_SavedState));
            FoliageInteractionField::Reset();

            // Wind OFF, globally and per layer. The only thing that may move a
            // plant in this test is an influence.
            auto& wind = Renderer3D::GetWindSettings();
            wind = WindSettings{};
            wind.Enabled = false;
            wind.Speed = 0.0f;
            wind.GustStrength = 0.0f;

            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            m_TerrainEntity = scene.CreateEntityWithUUID(UUID(1238), "Terrain");
            {
                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 11;
                terrain.m_ProceduralResolution = 128;
                terrain.m_ProceduralOctaves = 4;
                terrain.m_ProceduralFrequency = 1.5f;
                terrain.m_WorldSizeX = 256.0f;
                terrain.m_WorldSizeZ = 256.0f;
                // Low relief so the plants, not the hillside, are what changes
                // between arms.
                terrain.m_HeightScale = 4.0f;
                terrain.m_TessellationEnabled = false;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (auto layer : TerrainGenerator::MakeDefaultLayers())
                {
                    layer.BaseColor = glm::vec3(0.4f); // neutral: the terrain cannot satisfy the green plant mask
                    terrain.m_Material->AddLayer(layer);
                }

                auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
                foliage.m_Enabled = true;

                FoliageLayer grass;
                grass.Name = "Meadow";
                grass.AlbedoPath = kFoliageAlbedo;
                grass.Density = 1.5f;
                grass.SplatmapChannel = -1;
                grass.MinSlopeAngle = 0.0f;
                grass.MaxSlopeAngle = 70.0f;
                grass.MinScale = 1.0f;
                grass.MaxScale = 1.0f;
                grass.MinHeight = 2.0f;
                grass.MaxHeight = 3.0f;
                grass.ViewDistance = 200.0f;
                grass.FadeStartDistance = 180.0f;
                grass.UseAuthoredMesh = false;
                grass.UseImpostor = false;
                grass.AlphaCutoff = 0.25f;
                grass.WindStrength = 0.0f;
                grass.WindStiffness = 0.35f; // the anchored bend profile; no wind amplitude
                grass.InteractionResponse = 1.0f;
                grass.BaseColor = glm::vec3(0.18f, 0.48f, 0.14f);
                foliage.m_Layers.push_back(grass);
                foliage.m_NeedsRebuild = true;
            }

            m_ActorEntity = scene.CreateEntityWithUUID(UUID(12380), "Runner");
            {
                auto& transform = m_ActorEntity.GetComponent<TransformComponent>();
                transform.Translation = glm::vec3(kPatchX, 0.0f, kPatchZ);
                auto& influence = m_ActorEntity.AddComponent<FoliageInteractionComponent>();
                influence.m_Enabled = false; // the control arm is the default state
                influence.m_Radius = 14.0f;
                influence.m_Height = 6.0f;
                influence.m_Strength = kFoliageInteractionMaxStrength;
                influence.m_Falloff = 1.0f;
                influence.m_RecoverySeconds = 0.35f;
                influence.m_TrailSpacing = 0.0f;
            }
        }

        void Capture(const glm::vec3& eye, f32 yaw, f32 pitch, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, pitch);
            RunEditorFrames(camera, 4);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);
        }

        // Advance the scene clock by `seconds` in 1/60 s steps, so the
        // interaction field integrates the same number of springs it would in a
        // real session — a single giant step would exercise the dt clamp
        // instead of the motion.
        void Advance(const glm::vec3& eye, f32 yaw, f32 pitch, f32 seconds, const glm::vec3* walkTo = nullptr)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, pitch);
            const auto steps = static_cast<u32>(std::max(1.0f, std::round(seconds * 60.0f)));
            const glm::vec3 start = m_ActorEntity.GetComponent<TransformComponent>().Translation;
            for (u32 i = 0; i < steps; ++i)
            {
                m_MockTime += 1.0f / 60.0f;
                Time::SetMockTime(m_MockTime);
                if (walkTo)
                {
                    const f32 t = static_cast<f32>(i + 1) / static_cast<f32>(steps);
                    m_ActorEntity.GetComponent<TransformComponent>().Translation = glm::mix(start, *walkTo, t);
                }
                RunEditorFrames(camera, 1);
            }
        }

        void SetInfluence(bool enabled)
        {
            m_ActorEntity.GetComponent<FoliageInteractionComponent>().m_Enabled = enabled;
        }

        std::vector<f32> Velocity()
        {
            std::vector<f32> values;
            const u32 texture = Renderer3D::ResolveFrameGraphTexture(ResourceNames::Velocity);
            EXPECT_NE(texture, 0u);
            if (texture != 0)
                ReadbackRgbaFloat(texture, kWidth, kHeight, values);
            return values;
        }

        static void WritePng(const std::string& name, const std::vector<u8>& px)
        {
            ASSERT_EQ(px.size(), static_cast<sizet>(kWidth) * kHeight * 4);
            std::vector<u8> flipped(px);
            VisualEvidence::FlipRgbaRowsInPlace(flipped, kWidth, kHeight);
            fs::path dir = fs::path("assets") / "tests" / "visual";
            if (!Options().GoldenVendor.empty())
                dir /= Options().GoldenVendor;
            if (!GoldenRebaseRequested())
            {
                ::stbi_set_flip_vertically_on_load(0);
                ::stbi_set_flip_vertically_on_load_thread(0);
                int width = 0, height = 0, channels = 0;
                auto* data = ::stbi_load((dir / name).string().c_str(), &width, &height, &channels, 4);
                ASSERT_TRUE(data) << "Missing golden " << name;
                std::vector<u8> baseline;
                if (width == static_cast<int>(kWidth) && height == static_cast<int>(kHeight))
                    baseline.assign(data, data + static_cast<sizet>(kWidth) * kHeight * 4);
                ::stbi_image_free(data);
                ASSERT_EQ(baseline.size(), flipped.size()) << name;
                const auto rmse = VisualEvidence::Rgba8Rmse(flipped, baseline) / 255.0;
                if (rmse < 0.002)
                    return;
                EXPECT_LE(rmse, 0.05) << name;
                EXPECT_GE(VisualEvidence::Rgba8Ssim(flipped, baseline, kWidth, kHeight), 0.985f) << name;
                return;
            }
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << ec.message();
            ASSERT_NE(::stbi_write_png((dir / name).string().c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                       flipped.data(), static_cast<int>(kWidth) * 4),
                      0);
        }

        Entity m_TerrainEntity;
        Entity m_ActorEntity;
        f32 m_MockTime = 4.0f;
    };

    TEST_F(FoliageInteractionEvidenceTest, EveryRasterPathBendsUnderAnActorAndRecoversAfterIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const glm::vec3 eye(kPatchX, 6.0f, kPatchZ + 34.0f);

        for (const auto path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            const std::string pathName = path == RenderingPath::Forward       ? "Forward"
                                         : path == RenderingPath::ForwardPlus ? "ForwardPlus"
                                                                              : "Deferred";
            SCOPED_TRACE(pathName);
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = path;
            Renderer3D::ApplyRendererSettings();

            for (const bool oblique : { false, true })
            {
                // The oblique pose must LOOK AT the patch. Two earlier versions
                // did not, and both arms came back byte-identical — a perfect
                // zero, which reads as "the feature does nothing" rather than
                // as "the camera is pointing somewhere else", and the frame is
                // full of grass either way so nothing else notices.
                //
                // EditorCamera::GetOrientation is quat(-pitch, -yaw, 0), so
                // forward is (sin(yaw), *, -cos(yaw)): POSITIVE yaw turns
                // toward +X. A camera up the +X/+Z diagonal therefore needs
                // -pi/4, not +pi/4, to face the centre.
                const glm::vec3 pose = oblique ? glm::vec3(kPatchX + 18.0f, 14.0f, kPatchZ + 18.0f) : eye;
                const f32 yaw = oblique ? -0.785f : 0.0f;
                const f32 pitch = oblique ? 0.45f : 0.05f;
                const std::string angle = oblique ? "Oblique" : "Ground";
                const std::string cell = "GL_" + pathName;

                // ── Control: no influence at all ──────────────────────────
                SetInfluence(false);
                FoliageInteractionField::Reset();
                m_ActorEntity.GetComponent<TransformComponent>().Translation = glm::vec3(kPatchX, 0.0f, kPatchZ);
                Advance(pose, yaw, pitch, 0.2f);
                std::vector<u8> off, offRepeat;
                Capture(pose, yaw, pitch, off);
                Capture(pose, yaw, pitch, offRepeat);
                // The noise floor of this fixture, measured rather than
                // assumed — two captures of the identical state.
                const f64 noise = VisualEvidence::Rgba8Rmse(off, offRepeat);

                // ── The actor walks in and stands there ───────────────────
                SetInfluence(true);
                const glm::vec3 walkTo(kPatchX, 0.0f, kPatchZ + 6.0f);
                m_ActorEntity.GetComponent<TransformComponent>().Translation = glm::vec3(kPatchX, 0.0f, kPatchZ - 6.0f);
                Advance(pose, yaw, pitch, 0.6f, &walkTo);
                std::vector<u8> bent;
                Capture(pose, yaw, pitch, bent);

                // ── The actor is gone; the grass stands back up ───────────
                // Five seconds, not three: at a 0.35 s time constant a
                // full-strength bend is still 3.6e-3 after three, which is
                // above kFoliageInteractionRetireEpsilon and therefore still a
                // live slot. Measured, not guessed — the first version used
                // three and the field had not let go.
                SetInfluence(false);
                Advance(pose, yaw, pitch, 5.0f);
                std::vector<u8> recovered;
                Capture(pose, yaw, pitch, recovered);

                const auto isPlant = [](u32 r, u32 g, u32 b)
                { return g > 12u && g > 1.4 * r && g > 1.4 * b; };
                VisualEvidence::ExpectFrameHasSubject(off, angle + " off", isPlant);
                VisualEvidence::ExpectFrameHasSubject(bent, angle + " bent", isPlant);

                VisualEvidence::ExpectCapturesAreDistinct({ bent, off }, { "bent", "off" }, noise);

                constexpr u32 kMovedThreshold = 8;
                const sizet bendPixels = CountMovedPixels(bent, off, kMovedThreshold);
                const sizet residualPixels = CountMovedPixels(recovered, off, kMovedThreshold);
                const sizet noisePixels = CountMovedPixels(offRepeat, off, kMovedThreshold);

                // The bend has to be a real, countable thing before its
                // recovery means anything.
                EXPECT_GT(bendPixels, 2000u)
                    << pathName << " " << angle << ": the actor barely moved the frame — "
                    << bendPixels << " px over " << kMovedThreshold << "/255 (fixture noise "
                    << noisePixels << " px, RMSE " << VisualEvidence::Rgba8Rmse(bent, off) << ")";

                // THE recovery assertion. Not "the residual is small" — small
                // compared to WHAT? — but "almost none of the pixels the bend
                // moved are still moved", against the same control frame, with
                // every count printed when it fails.
                EXPECT_LT(residualPixels, bendPixels / 20 + noisePixels + 64)
                    << pathName << " " << angle << ": the bend did not recover. bent " << bendPixels
                    << " px, residual " << residualPixels << " px, fixture noise " << noisePixels << " px";

                WritePng("FoliageInteraction_" + cell + "_" + angle + ".png", bent);
                WritePng("FoliageInteractionOff_" + cell + "_" + angle + ".png", off);
            }
        }
    }

    // Composition with the velocity output (the TAA / upscale axis): a bend
    // that does not write velocity ghosts, and a bend whose PREVIOUS state is
    // wrong smears — both look like a post-process bug rather than a foliage
    // one, so they are measured here at the source.
    TEST_F(FoliageInteractionEvidenceTest, BendingWritesVelocityAndStopsWritingItWhenTheActorLeaves)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        const glm::vec3 eye(kPatchX, 6.0f, kPatchZ + 34.0f);
        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose(eye, 0.0f, 0.05f);

        // Still, with no influence: the velocity attachment must be clean, so
        // anything measured below is the bend and not the fixture.
        SetInfluence(false);
        FoliageInteractionField::Reset();
        Advance(eye, 0.0f, 0.05f, 0.2f);
        const auto still = Velocity();
        ASSERT_FALSE(still.empty());

        SetInfluence(true);
        const glm::vec3 walkTo(kPatchX, 0.0f, kPatchZ + 8.0f);
        m_ActorEntity.GetComponent<TransformComponent>().Translation = glm::vec3(kPatchX, 0.0f, kPatchZ - 8.0f);
        Advance(eye, 0.0f, 0.05f, 0.5f, &walkTo);
        const auto moving = Velocity();
        ASSERT_EQ(moving.size(), still.size());

        sizet movingPixels = 0;
        for (sizet i = 0; i + 3 < moving.size(); i += 4)
        {
            ASSERT_TRUE(std::isfinite(moving[i]) && std::isfinite(moving[i + 1]));
            ASSERT_TRUE(std::isfinite(still[i]) && std::isfinite(still[i + 1]));
            EXPECT_LT(std::abs(still[i]), 1e-4f) << "the still frame already had velocity at " << i;
            EXPECT_LT(std::abs(still[i + 1]), 1e-4f);
            if (std::abs(moving[i]) + std::abs(moving[i + 1]) > 1e-5f)
                ++movingPixels;
        }
        EXPECT_GT(movingPixels, 200u) << "interaction bending never reached the velocity attachment";

        // And once the field has fully relaxed the velocity goes back to
        // nothing — a residual here is the ghost the upscale cell would show.
        SetInfluence(false);
        Advance(eye, 0.0f, 0.05f, 5.0f);
        ASSERT_EQ(FoliageInteractionField::GetActiveCount(), 0u)
            << "the field still holds influences five seconds after the actor left";
        const auto settled = Velocity();
        ASSERT_EQ(settled.size(), still.size());
        for (sizet i = 0; i + 3 < settled.size(); i += 4)
        {
            EXPECT_LT(std::abs(settled[i]), 1e-4f) << "residual velocity at " << i;
            EXPECT_LT(std::abs(settled[i + 1]), 1e-4f);
        }
    }

    // Composition with the SHADOW output. A plant that bends in colour and not
    // in shadow reads as a detached shadow, which is the failure the shared
    // foliageDeform producer exists to prevent — so it is worth a measurement
    // rather than an argument about which files call which function.
    TEST_F(FoliageInteractionEvidenceTest, TheShadowMapBendsWithTheColourPass)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        const glm::vec3 eye(kPatchX, 6.0f, kPatchZ + 34.0f);
        const auto shadowDepths = [&]
        {
            const u32 texture = Renderer3D::ResolveFrameGraphTexture(ResourceNames::ShadowMapCSMCascade0);
            std::vector<f32> values;
            if (texture == 0)
                return values;
            GLint width = 0, height = 0;
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &width);
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &height);
            values.resize(static_cast<sizet>(width) * static_cast<sizet>(height));
            glGetTextureSubImage(texture, 0, 0, 0, 0, width, height, 1, GL_DEPTH_COMPONENT, GL_FLOAT,
                                 static_cast<GLsizei>(values.size() * sizeof(f32)), values.data());
            return values;
        };

        SetInfluence(false);
        FoliageInteractionField::Reset();
        Advance(eye, 0.0f, 0.05f, 0.2f);
        const auto off = shadowDepths();
        if (off.empty())
            GTEST_SKIP() << "no CSM cascade 0 in this configuration";

        SetInfluence(true);
        Advance(eye, 0.0f, 0.05f, 0.6f);
        const auto bent = shadowDepths();
        ASSERT_EQ(bent.size(), off.size());

        sizet changed = 0;
        for (sizet i = 0; i < bent.size(); ++i)
        {
            ASSERT_TRUE(std::isfinite(bent[i]) && std::isfinite(off[i]));
            if (std::abs(bent[i] - off[i]) > 1e-5f)
                ++changed;
        }
        EXPECT_GT(changed, 100u) << "the shadow caster did not bend with the lit plant";
    }
} // namespace OloEngine::Tests
