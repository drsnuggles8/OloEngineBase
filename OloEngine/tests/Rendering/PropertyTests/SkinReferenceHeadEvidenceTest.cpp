// OLO_TEST_LAYER: L8
// =============================================================================
// SkinReferenceHeadEvidenceTest.cpp — issue #1394: the REFERENCE HEAD moves up
// the skin transport ladder, from version 1 (diffusion only) to version 3
// (LayeredSpecular — diffusion, #1242 thin-region transmission and #1243
// layered specular).
//
// WHY A FILE OF ITS OWN, GIVEN SkinDigitalHumanEvidenceTest. That fixture is a
// PROCEDURAL PROBE: primitives, in-code profiles with deliberately exaggerated
// radii, a synthetic thickness map. It proves the mechanisms are connected and
// cannot say anything about the head a user actually opens. Every input here is
// the REAL one instead:
//
//   * the scanned mesh, assets/models/InfiniteScanHead/Head.fbx, placed exactly
//     as Scenes/DigitalHuman.olo places it;
//   * the profile READ FROM DISK, Assets/Materials/ReferenceHead.oloskin, through
//     the same SkinProfileSerializer the editor and the asset pack use — so an
//     edit to that file is an edit to this test's subject;
//   * the thickness map BAKED FROM THAT MESH by tools/skin-thickness/
//     bake_thickness.py, Assets/Textures/InfiniteScanHead_Thickness.png, at the
//     factor the scene authors.
//
// THE THREE ARMS, each built from the one file by changing only what the arm is
// about:
//   Version1  — the file with EvaluationModel set back to 1. That IS the profile
//               this issue started from: the file's other fields are unread
//               below version 2 (pinned by SkinProfileSerializerTest), so the
//               extra blocks cannot leak into the control.
//   Authored  — the file exactly as shipped.
//   Neutral   — the file at version 3 with every field versions 2 and 3 add set
//               to its NEUTRAL value. The acceptance criterion "a version bump
//               with no field authored changes nothing" is a claim about THIS
//               arm against Version1, and it is asserted over the whole v1 -> v3
//               jump rather than one rung at a time, because the jump is what
//               the reference head actually took.
//
// Evidence PNGs are named <Feature>[Off]_<Backend>_<Path>[_<Cell>].png so an
// unrun cell is a missing FILE (docs/process/task-loop.md 2a). GL only: the
// fixture needs a real GL 4.6 context and SKIPs without one, so the Vulkan rows
// of the verification grid come from the live editor and never from here.
//
// Classification: L8 (full Scene pipeline on all three raster paths, RGBA8
// readback + PNG; SKIPs cleanly without a GL 4.6 context).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SkinDiffusion.h"
#include "OloEngine/Renderer/SkinLayeredSpecular.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/SkinProfileTable.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 512;
        constexpr f32 kCaptureTime = 12.0f;

        // The asset handle ReferenceHead.oloskin is registered under in the
        // sandbox project. The same literal generate_reference_fixture_scenes.py
        // pins, for the same reason: nothing outside the editor reads the binary
        // registry.
        constexpr u64 kReferenceHeadProfileHandle = 15952688685437936278ULL;

        // THE THICKNESS THE SCENE AUTHORS. The map encodes thickness / 20 mm,
        // so 20 mm in metres is the factor that makes factor * map the measured
        // thickness. Asserted against DigitalHuman.olo below, so the scene and
        // this subject cannot drift apart.
        constexpr f32 kThicknessFactorMetres = 0.02f;
        constexpr const char* kThicknessMapProjectPath = "Assets/Textures/InfiniteScanHead_Thickness.png";

        // DigitalHuman.olo's placement of the scan: it is authored with its
        // origin at the base of the neck, so the scene scales it and drops it to
        // put the head on the origin.
        constexpr f32 kHeadScale = 0.38f;
        constexpr f32 kHeadDrop = -0.285f;

        // THE RIGHT EAR, in scene space, measured from the mesh: the bounding
        // box of every triangle whose UV lands in the ear's island of the atlas
        // (the same island the baked map is thin in). The window the backlight
        // claims are measured over is this box PROJECTED through the capture
        // camera, so it follows the camera rather than being a pixel constant.
        constexpr glm::vec3 kRightEarMin{ 0.049f, -0.098f, 0.006f };
        constexpr glm::vec3 kRightEarMax{ 0.074f, -0.049f, 0.041f };

        // A patch of FOREHEAD, the control region: skin the backlight reaches
        // only through the whole skull. It must stay dark in the transmission
        // view — a head that glowed everywhere under backlight would pass an
        // "is the ear lit" check and be exactly the uniformly emissive head
        // docs/guides/skin-transmission.md warns about.
        constexpr glm::vec3 kForeheadMin{ -0.025f, -0.005f, 0.080f };
        constexpr glm::vec3 kForeheadMax{ 0.025f, 0.020f, 0.100f };

        // THE CLOSED EYELIDS, the harder control. They are LOCALLY thin — a
        // fold of skin over the scan's eye pocket, 2.6-5.7 mm to the first
        // exit — and the backlight reaches their far side only through the
        // whole head. The first bake measured local thickness alone and lit
        // them like the ears; the bake's far-side test (tools/skin-thickness)
        // is what keeps them dark, and this window is what pins it.
        constexpr glm::vec3 kEyelidsMin{ -0.044f, -0.067f, 0.080f };
        constexpr glm::vec3 kEyelidsMax{ 0.044f, -0.042f, 0.110f };

        [[nodiscard]] fs::path EditorRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT };
        }

        [[nodiscard]] fs::path ProjectRoot()
        {
            return EditorRoot() / "SandboxProject";
        }

        [[nodiscard]] std::string ReadText(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        // The shipped profile, through the shipped reader. Not a literal: the
        // point of this file is that it measures the asset a user opens.
        [[nodiscard]] bool LoadShippedProfile(SkinProfileParameters& out, std::string& error)
        {
            const fs::path file = ProjectRoot() / "Assets" / "Materials" / "ReferenceHead.oloskin";
            const std::string yaml = ReadText(file);
            if (yaml.empty())
            {
                error = "could not read " + file.string();
                return false;
            }
            const SkinProfileSerializer serializer;
            auto profile = Ref<SkinProfile>::Create();
            if (!serializer.DeserializeFromYAML(yaml, profile))
            {
                error = "SkinProfileSerializer rejected " + file.string();
                return false;
            }
            out = profile->GetParameters();
            return true;
        }

        // The arm the neutral-identity criterion is about: version 3 with every
        // field versions 2 and 3 introduce at its neutral value. NOT the
        // defaults — Transmission.Strength defaults to 1 and
        // NormalVarianceStrength to 0.5, both deliberately non-neutral (see
        // docs/guides/skin-layered-specular.md, "LobeMix = 0 alone is not the
        // version-2 frame"). The neutral values are the ones that switch each
        // term off exactly.
        [[nodiscard]] SkinProfileParameters NeutralVersion3(SkinProfileParameters parameters)
        {
            parameters.EvaluationModel = SkinEvaluationModel::LayeredSpecular;
            parameters.Transmission.Strength = 0.0f;
            parameters.Specular.LobeMix = 0.0f;
            parameters.Specular.NormalVarianceStrength = 0.0f;
            parameters.Specular.DetailStrength = 0.0f;
            parameters.Specular.ExpressionDetailGain = 0.0f;
            return parameters;
        }

        struct Capture
        {
            std::vector<u8> Pixels;
            u32 Width = 0;
            u32 Height = 0;
        };

        struct Difference
        {
            u64 ChangedPixels = 0;
            u32 MaxDelta = 0;
        };

        [[nodiscard]] Difference Diff(const Capture& a, const Capture& b)
        {
            Difference d{};
            const sizet count = std::min(a.Pixels.size(), b.Pixels.size());
            for (sizet i = 0; i + 3 < count; i += 4)
            {
                u32 worst = 0;
                for (u32 c = 0; c < 3; ++c)
                {
                    worst = std::max(worst, static_cast<u32>(std::abs(static_cast<i32>(a.Pixels[i + c]) -
                                                                      static_cast<i32>(b.Pixels[i + c]))));
                }
                if (worst > 0)
                {
                    ++d.ChangedPixels;
                    d.MaxDelta = std::max(d.MaxDelta, worst);
                }
            }
            return d;
        }

        // A screen rectangle, image space (row 0 at the top).
        struct Window
        {
            u32 X0 = 0;
            u32 Y0 = 0;
            u32 X1 = 0;
            u32 Y1 = 0;

            [[nodiscard]] u64 Area() const
            {
                return static_cast<u64>(X1 - X0 + 1u) * static_cast<u64>(Y1 - Y0 + 1u);
            }
        };

        struct WindowStats
        {
            f64 MeanLuma = 0.0; // 0..1
            f64 MeanRed = 0.0;
            f64 MeanGreen = 0.0;
            // RED-DOMINANT pixels: red above a small floor AND half again the
            // green. Transmitted light through skin is red by construction
            // (exp(-t/d) with red's reach the longest), while the clear colour,
            // any overlay and ordinary lit skin are near-neutral, so this counts
            // the TRANSMISSION and nothing the frame happens to have behind it.
            // A luma floor counted the grey background as "lit" and read an
            // ear window at version 1 as 2 501 lit pixels.
            u64 RedPixels = 0;
            u64 Pixels = 0;
            u64 ChangedPixels = 0; // against a second capture, when one is given
            u32 MaxDelta = 0;
        };

        [[nodiscard]] WindowStats Measure(const Capture& capture, const Window& window,
                                          const Capture* against = nullptr)
        {
            WindowStats stats{};
            f64 luma = 0.0;
            f64 red = 0.0;
            f64 green = 0.0;
            for (u32 y = window.Y0; y <= window.Y1; ++y)
            {
                for (u32 x = window.X0; x <= window.X1; ++x)
                {
                    const sizet idx = (static_cast<sizet>(y) * capture.Width + x) * 4u;
                    const f64 r = capture.Pixels[idx + 0] / 255.0;
                    const f64 g = capture.Pixels[idx + 1] / 255.0;
                    const f64 b = capture.Pixels[idx + 2] / 255.0;
                    const f64 l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    luma += l;
                    red += r;
                    green += g;
                    if (r > 0.04 && r > 1.5 * g)
                        ++stats.RedPixels;
                    ++stats.Pixels;
                    if (against != nullptr)
                    {
                        u32 worst = 0;
                        for (u32 c = 0; c < 3; ++c)
                        {
                            worst = std::max(worst,
                                             static_cast<u32>(std::abs(static_cast<i32>(capture.Pixels[idx + c]) -
                                                                       static_cast<i32>(against->Pixels[idx + c]))));
                        }
                        if (worst > 0)
                            ++stats.ChangedPixels;
                        stats.MaxDelta = std::max(stats.MaxDelta, worst);
                    }
                }
            }
            if (stats.Pixels > 0)
            {
                const auto n = static_cast<f64>(stats.Pixels);
                stats.MeanLuma = luma / n;
                stats.MeanRed = red / n;
                stats.MeanGreen = green / n;
            }
            return stats;
        }

        void FlipRowsInPlace(std::vector<u8>& rgba, u32 width, u32 height)
        {
            const sizet rowBytes = static_cast<sizet>(width) * 4u;
            if (height < 2u || rgba.size() < rowBytes * height)
                return;
            std::vector<u8> scratch(rowBytes);
            for (u32 y = 0; y < height / 2u; ++y)
            {
                u8* top = rgba.data() + static_cast<sizet>(y) * rowBytes;
                u8* bottom = rgba.data() + static_cast<sizet>(height - 1u - y) * rowBytes;
                std::memcpy(scratch.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, scratch.data(), rowBytes);
            }
        }

        // Absolute, under the editor root: renderer initialisation moves the
        // process cwd into OloEditor/, so a relative path written after the
        // first frame lands in OloEditor/OloEditor/ and the test still passes.
        [[nodiscard]] fs::path VisualOutputPath(const std::string& name)
        {
            const fs::path dir = EditorRoot() / "assets" / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / (name + ".png");
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
            ScopedMockTime(const ScopedMockTime&) = delete;
            ScopedMockTime& operator=(const ScopedMockTime&) = delete;
        };

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
                    break;
            }
            return "Unknown";
        }

        constexpr std::array<RenderingPath, 3> kPaths = { RenderingPath::Forward, RenderingPath::ForwardPlus,
                                                          RenderingPath::Deferred };

        // The two camera poses, both on the FRONT of the head looking back toward
        // the light, because #1242's exit lobe peaks when the view looks INTO the
        // light through the ear. BACKLIT is DigitalHuman.olo's own three-quarter
        // camera; FRONTAL is nearer the light axis, where the lobe is strongest.
        //
        // A true side profile was tried and is the wrong photograph: it sees the
        // ear's lateral face, which this backlight lights DIRECTLY, so it
        // transmits toward that camera almost nothing (0.75% of the ear window
        // against 2.5% and 4.4% here).
        struct Pose
        {
            const char* Name;
            glm::vec3 Eye;
            glm::vec3 Target;
        };

        constexpr Pose kBacklitPose{ "Backlit", { 0.17f, 0.03f, 0.30f }, { 0.0f, -0.045f, 0.02f } };
        constexpr Pose kFrontalPose{ "Frontal", { 0.12f, -0.04f, 0.34f }, { 0.04f, -0.07f, 0.0f } };

        // Backlight: travelling from behind the head toward the lens, slightly
        // down, and from the ear's side so it reaches the BACK of the pinna past
        // the skull. The key is dim and in front, so the composite shows a face
        // rather than a silhouette and the layered specular has a lobe to move.
        const glm::vec3 kBacklightDirection = glm::normalize(glm::vec3(-0.25f, -0.20f, 1.0f));
        constexpr f32 kBacklightIntensity = 6.0f;
    } // namespace

    // =========================================================================
    // The shipped profile, as a file (no GPU)
    // =========================================================================

    // The claim the whole issue is: the file a user opens is at version 3, with
    // real values in the fields version 3 reads — not the neutral ones, which
    // would move the version and change nothing.
    TEST(SkinReferenceHeadProfile, TheShippedProfileIsAtLayeredSpecularWithAuthoredTerms)
    {
        SkinProfileParameters p{};
        std::string error;
        ASSERT_TRUE(LoadShippedProfile(p, error)) << error;

        EXPECT_EQ(p.EvaluationModel, SkinEvaluationModel::LayeredSpecular)
            << "ReferenceHead.oloskin is at transport version " << static_cast<i32>(p.EvaluationModel)
            << " (" << ToString(p.EvaluationModel)
            << "). Issue #1394 moves it to 3: every version above 1 is cumulative, and below 2 the reference head has "
               "no thin-region transmission and below 3 no layered specular.";

        // In range AS AUTHORED. Sanitize() returning false means a bound
        // corrected a value, i.e. the file says one thing and the renderer uses
        // another.
        SkinProfileParameters sanitized = p;
        EXPECT_TRUE(sanitized.Sanitize()) << "ReferenceHead.oloskin carries an out-of-range value that the loader "
                                             "silently corrected";

        // Each term the version adds is AUTHORED ON, i.e. differs from the value
        // that switches it off. A profile moved to 3 with these at neutral
        // would be the neutral-identity arm below: a version bump that renders
        // the version-1 frame and satisfies the issue's title but not its intent.
        EXPECT_GT(p.Transmission.Strength, 0.0f) << "#1242 transmission is authored off";
        EXPECT_GT(p.Specular.LobeMix, 0.0f) << "#1243's second lobe is authored off";
        EXPECT_GT(p.Specular.NormalVarianceStrength, 0.0f) << "#1243's variance filter is authored off";

        // AND THE DIFFUSION WAS NOT RETUNED ON THE WAY. The version move is about
        // the terms above; the scattering the head was signed off with at
        // version 1 is SkinDiffusionTest's pinned ReferenceHead(), and a change
        // here would move every frame for a reason this issue does not own.
        EXPECT_NEAR(p.ScatterColor.r, 0.85f, 1.0e-5f);
        EXPECT_NEAR(p.ScatterColor.g, 0.55f, 1.0e-5f);
        EXPECT_NEAR(p.ScatterColor.b, 0.45f, 1.0e-5f);
        EXPECT_NEAR(p.ScatterRadiusMM.r, 1.55f, 1.0e-5f);
        EXPECT_NEAR(p.ScatterRadiusMM.g, 0.80f, 1.0e-5f);
        EXPECT_NEAR(p.ScatterRadiusMM.b, 0.55f, 1.0e-5f);
        EXPECT_NEAR(p.ThicknessScale, 1000.0f, 1.0e-3f);

        // Nothing ABOVE version 3 is authored: the head is not a mouth and not
        // an eye, and those blocks are unread at 3 anyway. Asserted so a later
        // edit that moves the file to 4 or 5 for "completeness" has to change
        // this test and say why.
        EXPECT_TRUE(p.Oral == SkinOralParameters{}) << "the reference head authors an oral coat";
        EXPECT_TRUE(p.Ocular == SkinOcularParameters{}) << "the reference head authors an eye";
    }

    // The CPU half of the neutral-identity criterion: the neutral arm really is
    // neutral under the version predicates the shaders branch on, and it
    // differs from the authored file in the version-2/3 fields ONLY.
    TEST(SkinReferenceHeadProfile, TheNeutralArmDiffersFromTheFileOnlyInTheTermsItSwitchesOff)
    {
        SkinProfileParameters authored{};
        std::string error;
        ASSERT_TRUE(LoadShippedProfile(authored, error)) << error;

        const SkinProfileParameters neutral = NeutralVersion3(authored);
        SkinProfileParameters rebuilt = neutral;
        rebuilt.Transmission.Strength = authored.Transmission.Strength;
        rebuilt.Specular = authored.Specular;
        EXPECT_TRUE(rebuilt == authored) << "NeutralVersion3 changed a field it does not own, so the neutral-identity "
                                            "arm would be comparing two profiles that differ in more than the version";

        SkinProfileParameters sanitized = neutral;
        EXPECT_TRUE(sanitized.Sanitize()) << "the neutral arm is out of range";
        EXPECT_TRUE(SkinEvaluatesLayeredSpecular(neutral.EvaluationModel));
    }

    // =========================================================================
    // The live scene carries what this subject is built from (no GPU)
    // =========================================================================

    // The GPU fixture below assembles the head in code, so this pins it to the
    // scene a user opens: same profile handle, same thickness map, same factor,
    // and the same backlight — directional and UNSHADOWED. Both halves of that
    // were decided by a measurement:
    //   * a point backlight transmits on Forward and Deferred but not Forward+,
    //     whose clustered punctual lights skip the skin transmission term;
    //   * a shadowed one blacks the ear out in the live editor: at the editor
    //     camera's cascade resolution a ~3 mm ear is thicker than the shadow
    //     normal offset and occludes itself (docs/guides/skin-transmission.md,
    //     "Two limits, stated"). The eyelids, which a shadow would otherwise
    //     have kept dark, are kept dark by the bake's far-side test instead.
    TEST(SkinReferenceHeadScene, TheDigitalHumanSceneWiresTheBakedThicknessAndAnUnshadowedDirectionalBacklight)
    {
        const fs::path sceneFile = ProjectRoot() / "Assets" / "Scenes" / "DigitalHuman.olo";
        YAML::Node scene;
        ASSERT_NO_THROW(scene = YAML::LoadFile(sceneFile.string())) << sceneFile.string();

        YAML::Node cranium;
        YAML::Node backlight;
        for (const YAML::Node& entity : scene["Entities"])
        {
            const auto tag = entity["TagComponent"]["Tag"].as<std::string>("");
            if (tag == "Cranium")
                cranium = entity;
            else if (tag == "Backlight")
                backlight = entity;
        }
        ASSERT_TRUE(cranium) << "DigitalHuman.olo has no Cranium";
        ASSERT_TRUE(backlight) << "DigitalHuman.olo has no Backlight";

        const YAML::Node material = cranium["MaterialComponent"];
        ASSERT_TRUE(material) << "the cranium has no MaterialComponent";
        EXPECT_EQ(material["SkinProfile"].as<u64>(0), kReferenceHeadProfileHandle);
        EXPECT_EQ(material["ThicknessMapPath"].as<std::string>(""), kThicknessMapProjectPath)
            << "without the baked map the scanned head is one uniform thickness and the ear cannot be the thin region";
        EXPECT_NEAR(material["ThicknessFactor"].as<f32>(0.0f), kThicknessFactorMetres, 1.0e-6f)
            << "the map encodes thickness / 20 mm; any other factor makes factor * map a wrong thickness";
        EXPECT_TRUE(fs::exists(ProjectRoot() / kThicknessMapProjectPath))
            << "the scene names a thickness map that is not in the project";

        const YAML::Node light = backlight["DirectionalLightComponent"];
        ASSERT_TRUE(light) << "the backlight is not a directional light. Forward+ evaluates skin transmission for the "
                              "directional light only (its clustered punctual lights do not transmit), so a point "
                              "backlight leaves that path's backlight cell showing nothing.";
        EXPECT_FALSE(light["CastShadows"].as<bool>(true))
            << "a shadowed backlight makes the ~3 mm ear occlude itself at editor cascade resolution, and the "
               "transmission view goes black over it";
    }

    // The baked map is a measurement; this is a SANITY check that the file is
    // one — square, with a region thin enough to transmit and a thick one —
    // rather than a flat placeholder. It deliberately claims no more than that:
    // the atlas's uncovered texels are written thick too, so "mostly clamped"
    // says little about the head on its own. Whether the THIN region is where
    // it should be (the ears, not the skull or the eyelids) is the GPU
    // fixture's forehead and eyelid controls below, which read the map through
    // the real sampler.
    TEST(SkinReferenceHeadScene, TheBakedThicknessMapHasAThinRegionAndAThickOne)
    {
        const fs::path file = ProjectRoot() / kThicknessMapProjectPath;
        int w = 0;
        int h = 0;
        int channels = 0;
        stbi_uc* data = ::stbi_load(file.string().c_str(), &w, &h, &channels, 4);
        ASSERT_NE(data, nullptr) << "could not decode " << file.string();
        std::vector<u8> pixels(data, data + static_cast<sizet>(w) * static_cast<sizet>(h) * 4u);
        ::stbi_image_free(data);

        EXPECT_EQ(w, h);
        u8 thinnest = 255;
        u64 thin = 0;
        u64 clamped = 0;
        for (sizet i = 0; i < pixels.size(); i += 4)
        {
            thinnest = std::min(thinnest, pixels[i]);
            // Under 5 mm (value 64 of 255 at 20 mm full scale) is an ear rim or
            // a nostril; 255 is the skull, clamped at full scale.
            if (pixels[i] < 64)
                ++thin;
            if (pixels[i] == 255)
                ++clamped;
        }
        const auto total = static_cast<f64>(pixels.size() / 4u);
        EXPECT_LT(thinnest, 51) << "nothing on the head measures under 4 mm, so no region is thin enough to transmit";
        EXPECT_GT(static_cast<f64>(thin) / total, 0.002) << "the thin region is vanishingly small";
        EXPECT_GT(static_cast<f64>(clamped) / total, 0.5)
            << "most of the sheet should clamp to 20 mm (the skull and the uncovered atlas) — a map that is mostly "
               "thin is not this bake";
    }

    // =========================================================================
    // The subject
    // =========================================================================

    class SkinReferenceHeadBacklit : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            // Loading the model builds GPU buffers; headless, that is a crash
            // before the test body's skip can run.
            if (!RenderPropertyFixture::IsGpuAvailable())
                return;

            m_SavedPath = Renderer3D::GetRendererSettings().Path;
            EnableRendering(kSize, kSize);
            SetUpScratchProject();
            ASSERT_FALSE(HasFatalFailure()) << "the scratch project never came up";

            SkinProfileParameters shipped{};
            std::string error;
            ASSERT_TRUE(LoadShippedProfile(shipped, error)) << error;

            SkinProfileParameters version1 = shipped;
            version1.EvaluationModel = SkinEvaluationModel::ScreenSpaceDiffusion;

            m_Version1Profile = RegisterProfile("ReferenceHeadV1", version1);
            m_AuthoredProfile = RegisterProfile("ReferenceHeadV3", shipped);
            m_NeutralProfile = RegisterProfile("ReferenceHeadV3Neutral", NeutralVersion3(shipped));
            // An IDENTICAL copy of the version-1 arm under a second handle, so it
            // lands in a second profile slot. See the neutral-identity test.
            m_Version1TwinProfile = RegisterProfile("ReferenceHeadV1Twin", version1);

            m_Model = Ref<Model>::Create((EditorRoot() / "assets" / "models" / "InfiniteScanHead" / "Head.fbx").string());
            ASSERT_TRUE(m_Model && m_Model->GetMeshCount() > 0) << "the scanned head did not load";

            Ref<Texture2D> thickness =
                Texture2D::Create((ProjectRoot() / kThicknessMapProjectPath).string(), /*srgb=*/false);
            ASSERT_TRUE(thickness && thickness->IsLoaded()) << "the baked thickness map did not load";
            ASSERT_TRUE(thickness->GetRHIHandle().IsValid())
                << "the thickness map has no RHI handle, so u_UseThicknessMap will be 0 and the head is uniformly 20 mm";

            Entity head = GetScene().CreateEntity("Cranium");
            auto& transform = head.GetComponent<TransformComponent>();
            transform.Translation = { 0.0f, kHeadDrop, 0.0f };
            transform.Scale = glm::vec3(kHeadScale);
            head.AddComponent<ModelComponent>(m_Model, (EditorRoot() / "assets/models/InfiniteScanHead/Head.fbx").string());

            // DigitalHuman.olo's material, field for field.
            auto& material = head.AddComponent<MaterialComponent>().m_Material;
            material.SetBaseColorFactor({ 0.66f, 0.49f, 0.42f, 1.0f });
            material.SetMetallicFactor(0.0f);
            material.SetRoughnessFactor(0.42f);
            material.SetMaterialKind(MaterialKind::Skin);
            material.SetSkinProfileHandle(m_Version1Profile);
            material.SetThicknessFactor(kThicknessFactorMetres);
            material.SetThicknessMap(thickness);
            m_Head = head;

            m_Camera = GetScene().CreateEntity("Camera");
            {
                auto& cameraComp = m_Camera.AddComponent<CameraComponent>();
                cameraComp.Primary = true;
                // DigitalHuman.olo's lens: 45 degrees vertical, a 1 cm near plane
                // (the head is 20 cm across and the camera is 34 cm away).
                cameraComp.Camera.SetPerspective(glm::radians(45.0f), 0.01f, 100.0f);
                cameraComp.Camera.SetViewportSize(kSize, kSize);
            }
            PoseCamera(kBacklitPose);

            Entity sun = GetScene().CreateEntity("Backlight");
            auto& dir = sun.AddComponent<DirectionalLightComponent>();
            dir.m_Direction = kBacklightDirection;
            dir.m_Color = { 0.94f, 0.95f, 1.0f };
            dir.m_Intensity = kBacklightIntensity;
            // UNSHADOWED, as DigitalHuman.olo authors it — see the scene test
            // below for why a shadowed backlight blacks the ear out.
            dir.m_CastShadows = false;

            Entity key = GetScene().CreateEntity("Key");
            key.GetComponent<TransformComponent>().Translation = { 0.22f, 0.30f, 0.42f };
            auto& point = key.AddComponent<PointLightComponent>();
            point.m_Color = { 1.0f, 0.97f, 0.93f };
            point.m_Intensity = 0.6f;
            point.m_Range = 3.0f;
        }

        void TearDown() override
        {
            // Before anything that can fail: a renderer-wide switch left armed
            // here reaches every later fixture in the process.
            Renderer3D::GetSkinDiffusionSettings() = SkinDiffusionSettings{};
            Renderer3D::GetPostProcessSettings().MaterialDebug = MaterialDebugView::None;
            Renderer3D::GetRendererSettings().Path = m_SavedPath;
            Renderer3D::GetSkinProfileTable().Reset();

            m_Model.Reset();
            RendererAttachedTest::TearDown();
            if (m_AssetManager)
                m_AssetManager->Shutdown();
            m_AssetManager.Reset();
            Project::Unload();
            std::error_code ec;
            if (!m_ProjectDir.empty())
                fs::remove_all(m_ProjectDir, ec);
        }

        void SetUpScratchProject()
        {
            std::error_code ec;
            m_ProjectDir = OloEngine::Tests::TempDir("skin-reference-head-project");
            fs::create_directories(m_ProjectDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create the scratch project at " << m_ProjectDir.string();
            const fs::path projectFile = m_ProjectDir / "SkinReferenceHead.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: SkinReferenceHead\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << projectFile.string();
            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        [[nodiscard]] AssetHandle RegisterProfile(const char* name, const SkinProfileParameters& parameters)
        {
            auto profile = Ref<SkinProfile>::Create();
            profile->SetName(name);
            EXPECT_TRUE(profile->SetParameters(parameters)) << name << ": the profile needed correcting";
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
            EXPECT_NE(static_cast<u64>(handle), 0ULL) << name << ": AddMemoryOnlyAsset returned a zero handle";
            return handle;
        }

        // A PRIMARY RUNTIME CAMERA, driven by RunFrames, not an EditorCamera
        // through RunEditorFrames. The editor frame draws the editor's own grid
        // and the selected entity's transform gizmo into the composite, and the
        // first run of this file measured a forehead window that was mostly
        // gizmo. The runtime path draws the scene and nothing else.
        void PoseCamera(const Pose& pose)
        {
            auto& transform = m_Camera.GetComponent<TransformComponent>();
            transform.Translation = pose.Eye;
            transform.SetRotation(glm::quatLookAt(glm::normalize(pose.Target - pose.Eye), glm::vec3(0.0f, 1.0f, 0.0f)));
        }

        // Project a world-space box through the camera; the window is the
        // bounding rectangle of its eight corners, clamped to the frame. The
        // matrices are the camera COMPONENT's own, so the window follows
        // whatever convention the renderer used for the capture.
        [[nodiscard]] bool ProjectBox(const Pose& pose, const glm::vec3& lo, const glm::vec3& hi, Window& out)
        {
            PoseCamera(pose);
            auto& camera = m_Camera.GetComponent<CameraComponent>().Camera;
            camera.SetViewportSize(kSize, kSize);
            const glm::mat4 viewProjection =
                camera.GetProjection() * glm::inverse(m_Camera.GetComponent<TransformComponent>().GetTransform());
            f32 minX = 1.0e9f;
            f32 minY = 1.0e9f;
            f32 maxX = -1.0e9f;
            f32 maxY = -1.0e9f;
            for (u32 corner = 0; corner < 8u; ++corner)
            {
                const glm::vec3 p{ (corner & 1u) ? hi.x : lo.x, (corner & 2u) ? hi.y : lo.y,
                                   (corner & 4u) ? hi.z : lo.z };
                const glm::vec4 clip = viewProjection * glm::vec4(p, 1.0f);
                if (clip.w <= 0.0f)
                    return false;
                const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                const f32 x = (ndc.x * 0.5f + 0.5f) * static_cast<f32>(kSize - 1u);
                const f32 y = (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<f32>(kSize - 1u);
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
            const auto clampPixel = [](f32 v)
            { return static_cast<u32>(std::clamp(v, 0.0f, static_cast<f32>(kSize - 1u))); };
            out = Window{ clampPixel(minX), clampPixel(minY), clampPixel(maxX), clampPixel(maxY) };
            return out.X1 > out.X0 && out.Y1 > out.Y0;
        }

        [[nodiscard]] bool CaptureFrame(RenderingPath path, AssetHandle profile, const Pose& pose,
                                        MaterialDebugView view, const std::string& name, Capture& out)
        {
            if (view != MaterialDebugView::None && path != RenderingPath::Deferred)
            {
                ADD_FAILURE() << "the material debug views exist on the deferred path only; " << PathName(path)
                              << " would have captured a composite and called it an AOV";
                return false;
            }
            m_Head.GetComponent<MaterialComponent>().m_Material.SetSkinProfileHandle(profile);
            // DIFFUSION ON FOR EVERY CAPTURE, the debug views included. Until
            // #1394 the pass added the diffuse high-pass on top of a
            // Transmission view, so the version-1 capture of that view showed
            // every crease of the head; the "version 1 is black at the ear"
            // claim below is the regression test for that fix
            // (RenderPipeline.cpp, SkinDiffusionRunsThisFrame).
            Renderer3D::GetSkinDiffusionSettings().Enabled = true;
            Renderer3D::GetSkinDiffusionSettings().Quality = SkinDiffusionQuality::High;
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::GetPostProcessSettings().MaterialDebug = view;
            Renderer3D::ApplyRendererSettings();

            const ScopedMockTime clock(kCaptureTime);
            PoseCamera(pose);
            // Four frames: the first settles the graph rebuild a path or profile
            // change forces, the shadow map and the diffusion pass need one each
            // after that, and the last is the one measured.
            RunFrames(4);

            if (!ReadbackComposite(out.Pixels, out.Width, out.Height))
                return false;
            if (out.Width != kSize || out.Height != kSize)
                return false;
            FlipRowsInPlace(out.Pixels, out.Width, out.Height);

            if (!name.empty())
            {
                const fs::path file = VisualOutputPath(name);
                const int wrote = ::stbi_write_png(file.string().c_str(), static_cast<int>(out.Width),
                                                   static_cast<int>(out.Height), 4, out.Pixels.data(),
                                                   static_cast<int>(out.Width) * 4);
                EXPECT_NE(wrote, 0) << "failed to write " << file.string();
            }
            return true;
        }

        // Two captures of an unchanged configuration: every "unchanged" claim is
        // measured against this rather than against zero. Fatal on a failed
        // readback so a floor that was never measured cannot read as 0.
        void MeasureRepeatFloor(RenderingPath path, AssetHandle profile, const Pose& pose, Difference& out)
        {
            Capture first;
            Capture second;
            ASSERT_TRUE(CaptureFrame(path, profile, pose, MaterialDebugView::None, {}, first))
                << PathName(path) << ": repeat-floor readback failed";
            ASSERT_TRUE(CaptureFrame(path, profile, pose, MaterialDebugView::None, {}, second))
                << PathName(path) << ": repeat-floor readback failed";
            out = Diff(first, second);
        }

        [[nodiscard]] Window EarWindow(const Pose& pose)
        {
            Window window;
            EXPECT_TRUE(ProjectBox(pose, kRightEarMin, kRightEarMax, window))
                << pose.Name << ": the right ear is not in frame";
            return window;
        }

        [[nodiscard]] Window ForeheadWindow(const Pose& pose)
        {
            Window window;
            EXPECT_TRUE(ProjectBox(pose, kForeheadMin, kForeheadMax, window))
                << pose.Name << ": the forehead is not in frame";
            return window;
        }

        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_ProjectDir;
        Ref<Model> m_Model;
        Entity m_Head;
        Entity m_Camera;
        AssetHandle m_Version1Profile{};
        AssetHandle m_AuthoredProfile{};
        AssetHandle m_NeutralProfile{};
        AssetHandle m_Version1TwinProfile{};
        RenderingPath m_SavedPath = RenderingPath::Forward;
    };

    // CRITERION 1 — the reference head transmits AT THE EARS under backlight,
    // shown in the material-transmission view against the version-1 profile.
    //
    // THREE CLAIMS, because one would pass on the wrong picture:
    //   1. the ear is lit in the transmission view at version 3 — the term runs;
    //   2. it is black there at version 1 — the light comes from the VERSION,
    //      not from a view that shows something regardless;
    //   3. the ear is far brighter than the forehead — the glow is where the
    //      baked map is thin, not everywhere. A map sampled upside down, or a
    //      uniform thickness, fails this one and passes the other two.
    TEST_F(SkinReferenceHeadBacklit, TheEarTransmitsUnderBacklightAndVersionOneDoesNot)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        for (const Pose& pose : { kBacklitPose, kFrontalPose })
        {
            const std::string cell = std::string("_GL_Deferred_") + pose.Name;
            Capture authored;
            Capture version1;
            ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, m_AuthoredProfile, pose, MaterialDebugView::Transmission,
                                     "ReferenceHeadV3Transmission" + cell, authored));
            ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, m_Version1Profile, pose, MaterialDebugView::Transmission,
                                     "ReferenceHeadV3TransmissionOff" + cell, version1));

            const Window ear = EarWindow(pose);
            const Window forehead = ForeheadWindow(pose);
            Window eyelids;
            EXPECT_TRUE(ProjectBox(pose, kEyelidsMin, kEyelidsMax, eyelids)) << pose.Name << ": eyelids off-frame";
            ASSERT_FALSE(HasFailure()) << pose.Name << ": a measurement window is off-frame";

            const WindowStats earV3 = Measure(authored, ear);
            const WindowStats earV1 = Measure(version1, ear);
            const WindowStats browV3 = Measure(authored, forehead);

            const WindowStats browV1 = Measure(version1, forehead);
            const f64 earShare = static_cast<f64>(earV3.RedPixels) / static_cast<f64>(ear.Area());
            const f64 browShare = static_cast<f64>(browV3.RedPixels) / static_cast<f64>(forehead.Area());
            const WindowStats lidsV3 = Measure(authored, eyelids);
            const f64 lidShare = static_cast<f64>(lidsV3.RedPixels) / static_cast<f64>(eyelids.Area());
            std::printf("[ReferenceHead %s] eyelids (%ux%u) v3 red-dominant %llu\n", pose.Name,
                        eyelids.X1 - eyelids.X0 + 1u, eyelids.Y1 - eyelids.Y0 + 1u,
                        static_cast<unsigned long long>(lidsV3.RedPixels));
            std::printf("[ReferenceHead %s] transmission view, red-dominant pixels: ear (%ux%u) v3 %llu v1 %llu; "
                        "forehead (%ux%u) v3 %llu v1 %llu; ear mean r %.4f g %.4f\n",
                        pose.Name, ear.X1 - ear.X0 + 1u, ear.Y1 - ear.Y0 + 1u,
                        static_cast<unsigned long long>(earV3.RedPixels),
                        static_cast<unsigned long long>(earV1.RedPixels), forehead.X1 - forehead.X0 + 1u,
                        forehead.Y1 - forehead.Y0 + 1u, static_cast<unsigned long long>(browV3.RedPixels),
                        static_cast<unsigned long long>(browV1.RedPixels), earV3.MeanRed, earV3.MeanGreen);

            // One per cent of the window, and that is the physics rather than a
            // lenient bound: the scan's ear is thin only at the helix rim (~3 mm,
            // red transmittance ~13% through this profile) and 5-7 mm over the
            // rest of the pinna (1-3%). The glow is a rim, as it is on a real ear.
            EXPECT_GT(earV3.RedPixels, ear.Area() / 100u)
                << pose.Name << ": the transmission view is black over the ear at version 3. Check, in order: the "
                                "profile's version, its Transmission.Strength, the material's thickness factor and "
                                "map (docs/guides/skin-transmission.md).";
            EXPECT_EQ(earV1.RedPixels, 0u)
                << pose.Name << ": the ear shows transmitted light at version 1, so the light in the version-3 "
                                "capture is not coming from the version move.";
            EXPECT_GT(earShare, browShare * 4.0)
                << pose.Name << ": " << earShare * 100.0 << "% of the ear window transmits against "
                << browShare * 100.0
                << "% of the forehead's. The glow is not where the baked map is thin: a map sampled upside down or a "
                   "uniform thickness makes the whole head glow.";
            EXPECT_GT(earShare, lidShare * 4.0)
                << pose.Name << ": the closed eyelids transmit (" << lidShare * 100.0 << "% of their window) nearly as "
                << "much as the ear (" << earShare * 100.0
                << "%). They are thin only locally — behind them is the eye pocket and then the head — so the baked "
                   "map's far-side test has stopped reading them as thick.";
        }
    }

    // CRITERION 1's visible A/B and CRITERION 4's GL rows: the composite a user
    // sees, version 3 against version 1, on every raster path. Transmission on
    // Forward and Forward+ is evaluated in the forward shaders, not the deferred
    // lighting pass, so the deferred AOV above says nothing about them.
    TEST_F(SkinReferenceHeadBacklit, TheBacklitCompositeChangesAtTheEarOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        for (const RenderingPath path : kPaths)
        {
            const char* p = PathName(path);
            Difference floor{};
            ASSERT_NO_FATAL_FAILURE(MeasureRepeatFloor(path, m_Version1Profile, kBacklitPose, floor));

            Capture authored;
            Capture version1;
            ASSERT_TRUE(CaptureFrame(path, m_AuthoredProfile, kBacklitPose, MaterialDebugView::None,
                                     std::string("ReferenceHeadV3_GL_") + p + "_Backlit", authored));
            ASSERT_TRUE(CaptureFrame(path, m_Version1Profile, kBacklitPose, MaterialDebugView::None,
                                     std::string("ReferenceHeadV3Off_GL_") + p + "_Backlit", version1));

            const Window ear = EarWindow(kBacklitPose);
            const WindowStats earV3 = Measure(authored, ear, &version1);
            const WindowStats earV1 = Measure(version1, ear);
            std::printf("[ReferenceHead %s] ear: %llu px changed, max delta %u, mean red %.4f -> %.4f; "
                        "repeat floor %u\n",
                        p, static_cast<unsigned long long>(earV3.ChangedPixels), earV3.MaxDelta, earV1.MeanRed,
                        earV3.MeanRed, floor.MaxDelta);

            EXPECT_GT(earV3.MaxDelta, floor.MaxDelta + 8u)
                << p << ": the backlit ear looks the same at version 3 as at version 1 (max delta " << earV3.MaxDelta
                << " against a repeat floor of " << floor.MaxDelta << ")";
            EXPECT_GT(earV3.MeanRed, earV1.MeanRed)
                << p << ": the ear got no redder — transmission adds light through the thin region, so it cannot "
                        "make the backlit ear darker";
        }
    }

    // CRITERION 3 — the neutral-identity property, over the whole v1 -> v3
    // jump the reference head took: a version-3 profile with every term the two
    // versions add switched to neutral renders the version-1 frame. Asserted on
    // every raster path, under a rig with a front key AND a backlight, so both
    // the specular and the transmission term had something to change if either
    // version gate leaked.
    //
    // THE FLOOR IS A SECOND SLOT, NOT A SECOND FRAME, and that is a measured
    // finding rather than a loosening. The neutral arm is a different profile
    // ASSET, so it occupies a different skin-profile slot, and two slots holding
    // BYTE-IDENTICAL parameters already differ here: 197 px, max 3/255, on
    // Forward and Forward+, 6 px max 2 on Deferred — terminator pixels, found
    // while localising what first looked like a version-gate leak (v1 -> v2 at
    // strength 0 moved 84 px, v2 -> neutral v3 moved 66 px, and neither exceeds
    // what a mere slot change does). A same-handle repeat floor is 0 and cannot
    // see that, so it would fail this claim for a reason that is not the claim.
    // The twin measures exactly the part of the difference that is the SLOT.
    // That a slot index reaches the shading at all is a defect in its own
    // right, tracked as #1422; when it is fixed, the twin floor falls to 0 and
    // this claim becomes exact again with no edit here.
    TEST_F(SkinReferenceHeadBacklit, ANeutralVersionThreeRendersTheVersionOneFrame)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        for (const RenderingPath path : kPaths)
        {
            const char* p = PathName(path);
            Difference repeat{};
            ASSERT_NO_FATAL_FAILURE(MeasureRepeatFloor(path, m_Version1Profile, kBacklitPose, repeat));

            Capture version1;
            Capture twin;
            Capture neutral;
            ASSERT_TRUE(CaptureFrame(path, m_Version1Profile, kBacklitPose, MaterialDebugView::None,
                                     std::string("ReferenceHeadV3Off_GL_") + p + "_NeutralIdentity", version1));
            ASSERT_TRUE(CaptureFrame(path, m_Version1TwinProfile, kBacklitPose, MaterialDebugView::None, {}, twin));
            ASSERT_TRUE(CaptureFrame(path, m_NeutralProfile, kBacklitPose, MaterialDebugView::None,
                                     std::string("ReferenceHeadV3Neutral_GL_") + p + "_NeutralIdentity", neutral));

            const Difference slotFloor = Diff(version1, twin);
            const Difference d = Diff(version1, neutral);
            const u32 floor = std::max(repeat.MaxDelta, slotFloor.MaxDelta);
            std::printf("[ReferenceHead %s] neutral v3 vs v1: %llu px differ, max delta %u; identical twin in "
                        "another slot: %llu px, max %u; repeat floor %u\n",
                        p, static_cast<unsigned long long>(d.ChangedPixels), d.MaxDelta,
                        static_cast<unsigned long long>(slotFloor.ChangedPixels), slotFloor.MaxDelta,
                        repeat.MaxDelta);
            // The slot floor must itself be SMALL, or this test would pass on
            // anything. Three LSB is what was measured.
            EXPECT_LE(slotFloor.MaxDelta, 4u)
                << p << ": two slots holding identical profiles now differ by " << slotFloor.MaxDelta
                << "/255, so the floor below no longer bounds anything";
            EXPECT_LE(d.MaxDelta, floor)
                << p << ": A NEUTRAL VERSION-3 REFERENCE HEAD DID NOT RENDER THE VERSION-1 FRAME (" << d.ChangedPixels
                << " px differ, max delta " << d.MaxDelta << " against a floor of " << floor
                << "). Moving a profile up the ladder with the new terms at neutral must change nothing; a failure "
                   "means a version-2 or version-3 gate is evaluating a term the profile did not ask for.";

            // And the authored profile DID move this frame, far above that floor,
            // so the identity is not passing because nothing here can change.
            Capture authored;
            ASSERT_TRUE(CaptureFrame(path, m_AuthoredProfile, kBacklitPose, MaterialDebugView::None, {}, authored));
            EXPECT_GT(Diff(version1, authored).MaxDelta, floor * 4u)
                << p << ": the authored profile renders the version-1 frame too, so the identity above proves nothing";
        }
    }
} // namespace OloEngine::Tests
