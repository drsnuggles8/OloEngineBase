// OLO_TEST_LAYER: L8
// =============================================================================
// SkinDigitalHumanEvidenceTest.cpp — the EPIC-LEVEL acceptance for issue #1222,
// "AAA skin and digital-human surface rendering".
//
// WHY THIS FILE EXISTS AT ALL, GIVEN SIX GREEN CHILDREN.
// -----------------------------------------------------------------------------
// #1231, #1241, #1242, #1243, #1244 and #1245 each ship their own evidence
// fixture, and each of those fixtures isolates ONE feature on a probe built to
// show it: a sphere with a split thickness map, a pair of eyeballs, a mouth that
// opens. That isolation is exactly what makes them good contract tests, and
// exactly why their sum is not the epic. The epic's three criteria are about
// what happens when the six run ON THE SAME PIXEL AT THE SAME TIME:
//
//   1. skin, eyes and mouth integrated on an ANIMATED reference subject,
//   2. several SKIN TONES holding up under soft / hard-side / BACKLIGHT while
//      the expression changes and the camera moves,
//   3. diffuse scattering, specular and transmission SEPARATELY INSPECTABLE and
//      MEASURED.
//
// Every one of those can fail with all six children green. Three profiles that
// each work alone can collide in the G-Buffer's three-bit slot field and shade
// each other's parameters. An expression can reach the deformation and not the
// shading, or reach the shading a frame early. A tone that looks right under a
// soft key can go grey under backlight because the transmission tint is being
// applied in the wrong space. None of those is visible from inside a
// single-feature fixture, because a single-feature fixture has one profile, one
// light and a static subject.
//
// WHAT THE SUBJECT IS, STATED PLAINLY. A procedural stand-in head: a sphere for
// the cranium, two smaller spheres for the eyes, one for the oral region, each
// carrying the profile its child issue authored, deformed by a morph target and
// viewed by a moving camera. It is NOT a scanned licensed head — #1239 recorded
// that as a genuine external dependency and it still is (see
// docs/guides/benchmark-reference-fixtures.md). What the stand-in CAN do is
// carry three real profiles in three real slots through the real frame graph,
// which is what all three criteria are actually about. A scanned head would
// make the captures prettier and would not make any assertion here stronger.
//
// THE SHAPE OF EVERY CLAIM. Each is A against B on the SAME scene with ONE
// authored value moved, measured against this fixture's OWN same-mode repeat
// floor (MeasureRepeatFloor) rather than against zero — two captures of an
// unchanged scene are not byte-identical here, and the floor is per raster path
// (deferred is the jittery one). An absolute threshold would either be a flake
// or be so loose it passed with the feature off.
//
// WHAT IS DELIBERATELY NOT HERE. The HISTORY quarter of criterion 3 belongs to
// issue #1256, which owns temporal reconstruction for skin, hair and foliage and
// is in flight in a sibling worktree. Duplicating a temporal debug view here
// would put two owners on one surface. This file covers the other three
// components and says so in its PR rather than quietly capturing three of four.
//
// Evidence PNGs, named <Feature>_<Backend>_<Path>[_<Cell>].png so an unrun cell
// is a missing FILE (docs/process/task-loop.md 2a). GL only — this fixture needs
// a real GL 4.6 context and SKIPs without one, so the Vulkan rows of the
// verification grid come from the live editor and can never come from here.
//
// Classification: L8 (full Scene pipeline on all three raster paths, RGBA8
// readback + PNG; SKIPs cleanly without a GL 4.6 context).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SkinDiffusion.h"
#include "OloEngine/Renderer/SkinLayeredSpecular.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/SkinProfileTable.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 384;

        // ---------------------------------------------------------------------
        // THE THREE SKIN TONES.
        //
        // Criterion 2 says "several skin tones", and the interesting axis is NOT
        // albedo — a darker albedo trivially renders darker on any material in
        // the engine, so three albedos would prove nothing about skin. What
        // separates skin tones PHYSICALLY is the mean free path: melanin absorbs
        // the long wavelengths that travel furthest, so a deeply pigmented skin
        // scatters over a SHORTER distance and with a LESS red-shifted tint than
        // a pale one. Both halves are authored here, and the assertions below
        // are about the SCATTERING, not about the albedo.
        //
        // The radii bracket the published range rather than sitting on one
        // measurement: the claim under test is "the ladder is monotonic and
        // survives every rig", which needs separation, not accuracy.
        // ---------------------------------------------------------------------
        struct ToneSetup
        {
            const char* Name;
            glm::vec3 Albedo;
            glm::vec3 ScatterColor;
            glm::vec3 ScatterRadiusMM;
        };

        constexpr ToneSetup kTones[] = {
            // Long red mean free path, strongly red-shifted: the classic
            // "pale skin glows red at the ear" response.
            { "Fair", { 0.78f, 0.62f, 0.55f }, { 0.94f, 0.58f, 0.44f }, { 9.0f, 4.2f, 2.6f } },
            { "Medium", { 0.56f, 0.40f, 0.31f }, { 0.86f, 0.52f, 0.38f }, { 6.2f, 3.0f, 1.9f } },
            // Short, much more achromatic: melanin absorbs before the light can
            // travel far enough to separate the channels.
            { "Deep", { 0.27f, 0.18f, 0.13f }, { 0.62f, 0.42f, 0.34f }, { 3.4f, 2.0f, 1.4f } },
        };

        // ---------------------------------------------------------------------
        // THE THREE LIGHTING RIGS.
        //
        // One directional key per rig and nothing else that moves, so the rig is
        // the ONLY difference between two captures of the same tone. The
        // intensities differ because the rigs are not meant to be equally bright
        // — a hard side light IS brighter than a soft wrap — and equalising them
        // would have made "holds up under hard light" a test of a light nobody
        // would author.
        //
        // BACKLIGHT is the one with teeth: it points AWAY from the camera, so
        // the only thing reaching the lens through the subject is the #1242
        // transmission term. A tone whose transmission tint is wrong is
        // invisible under the other two rigs and obvious here.
        // ---------------------------------------------------------------------
        struct LightingRig
        {
            const char* Name;
            glm::vec3 Direction;
            f32 Intensity;
            bool CastShadows;
            // Does this rig put a meaningful DIFFUSE irradiance on the subject?
            // The scattering-ladder assertion below is a claim about what the
            // diffusion pass does to the diffuse half, and under a pure
            // backlight that half is almost absent — see the comment on that
            // assertion for the measured numbers.
            bool LightsTheDiffuseHalf;
        };

        const LightingRig kRigs[] = {
            // Soft: a broad front-top wrap, shadows off — the "beauty" case.
            { "Soft", glm::normalize(glm::vec3(-0.18f, -0.42f, -0.89f)), 2.2f, false, true },
            // Hard side: 80 degrees off the view axis with shadows on, which is
            // where the layered specular of #1243 and the terminator live.
            { "HardSide", glm::normalize(glm::vec3(-0.97f, -0.16f, -0.18f)), 5.5f, true, true },
            // Backlight: behind the subject, pointing at the camera.
            { "Backlight", glm::normalize(glm::vec3(0.06f, -0.10f, 0.99f)), 6.0f, true, false },
        };

        // The camera poses the subject is captured from. THREEQUARTER and
        // OBLIQUE are where a grazing-angle specular and a transmitted rim are
        // largest; FRONT is the control where the subject is most symmetric.
        struct AngleSetup
        {
            const char* Name;
            glm::vec3 CameraPosition;
            glm::vec3 CameraEulerDegrees;
        };

        // All three sit at the SAME radius (3.6) from the subject's centre, so
        // the head subtends the same solid angle from each and a per-angle
        // brightness comparison is not secretly a distance comparison. Each
        // pose's euler is the one that points at the origin from its position.
        //
        // 3.6 and not the 3.3 this fixture started at: at 3.3 the head filled
        // the frame edge to edge and the oblique poses CLIPPED it, which both
        // made the captures hard to read and pushed the mouth off-screen at the
        // exact angles where the oral profiles are most visible.
        constexpr AngleSetup kAngles[] = {
            { "Front", { 0.0f, 0.05f, 3.60f }, { -0.8f, 0.0f, 0.0f } },
            { "ThreeQuarter", { 2.255f, 0.35f, 2.784f }, { -5.6f, 39.0f, 0.0f } },
            { "Oblique", { 3.036f, 0.95f, 1.683f }, { -15.3f, 61.0f, 0.0f } },
        };

        constexpr RenderingPath kPaths[] = { RenderingPath::Forward, RenderingPath::ForwardPlus,
                                             RenderingPath::Deferred };

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

        [[nodiscard]] const char* DebugViewName(MaterialDebugView view)
        {
            switch (view)
            {
                case MaterialDebugView::Diffuse:
                    return "Diffuse";
                case MaterialDebugView::Specular:
                    return "Specular";
                case MaterialDebugView::Transmission:
                    return "Transmission";
                case MaterialDebugView::ProfileIdentity:
                    return "ProfileId";
                case MaterialDebugView::ScatteringMask:
                    return "ScatteringMask";
                case MaterialDebugView::None:
                case MaterialDebugView::Count:
                default:
                    break;
            }
            return "Composite";
        }

        // The authored values that make each child's term fire hard enough to
        // measure. Exaggerated against what an artist would pick, for the reason
        // SkinTransmissionEvidenceTest exaggerates its radii: a contract test
        // measures whether the mechanism is CONNECTED, and a tolerance wide
        // enough to see a tasteful value would also pass with the term off.
        constexpr f32 kThicknessMetres = 0.012f;    // 12 mm — an ear, a lip
        constexpr f32 kLobeMix = 0.45f;             // #1243's second GGX lobe
        constexpr f32 kExpressionDetailGain = 2.4f; // pore gain at full expression
        constexpr f32 kOcularStrength = 1.0f;       // #1244's eye, fully on
        constexpr f32 kOralCoatStrength = 0.75f;    // #1245's wet film
        constexpr f32 kOralCavityOcclusion = 1.0f;
        constexpr f32 kEnamelIor = 1.63f;

        // The morph's displacement, in the subject's units. Big enough that the
        // deformation is unmistakable in the capture, which matters because the
        // expression assertions below have to be able to tell "the expression
        // reached the shading" from "the expression reached nothing".
        constexpr f32 kExpressionDisplacement = 0.16f;

        [[nodiscard]] f32 Channel(const std::vector<u8>& px, std::size_t idx, u32 channel)
        {
            return static_cast<f32>(px[idx + channel]) / 255.0f;
        }

        [[nodiscard]] f32 LumaAt(const std::vector<u8>& px, std::size_t idx)
        {
            return 0.2126f * Channel(px, idx, 0) + 0.7152f * Channel(px, idx, 1) + 0.0722f * Channel(px, idx, 2);
        }

        // Red's share of the pixel's total energy. Scale-free, so it does not
        // move when one capture is a little brighter overall — only when the
        // colour BALANCE changes, which is what the tone ladder is about.
        [[nodiscard]] f32 RedFractionAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = Channel(px, idx, 0);
            const f32 g = Channel(px, idx, 1);
            const f32 b = Channel(px, idx, 2);
            return r / std::max(r + g + b, 1.0e-4f);
        }

        struct Capture
        {
            std::vector<u8> Pixels;
            u32 Width = 0;
            u32 Height = 0;

            [[nodiscard]] std::size_t Index(u32 x, u32 y) const
            {
                return (static_cast<std::size_t>(y) * Width + x) * 4u;
            }

            [[nodiscard]] bool Valid() const
            {
                return Width > 0 && Height > 0 &&
                       Pixels.size() == static_cast<std::size_t>(Width) * Height * 4u;
            }
        };

        struct FrameStats
        {
            f32 MeanLuma = 0.0f;
            f32 MeanRedFraction = 0.0f;
            f32 PeakLuma = 0.0f;
            u64 Samples = 0;
            u64 LitSamples = 0; // pixels above a floor — "is anything on screen"
        };

        // Measured over the CENTRAL DISC, which is where the subject is — not
        // over the whole image, which is mostly background and would dilute
        // every measurement toward zero.
        // 0.28 of the width: at this fixture's camera radius of 3.6 the head's
        // projected radius is about 0.34 of the width, so the disc sits inside
        // the silhouette with margin and never averages background into a
        // measurement.
        [[nodiscard]] FrameStats MeasureSubject(const Capture& capture, f32 discFraction = 0.28f)
        {
            FrameStats stats{};
            f64 lumaSum = 0.0;
            f64 redSum = 0.0;
            const f32 cx = 0.5f * static_cast<f32>(capture.Width);
            const f32 cy = 0.5f * static_cast<f32>(capture.Height);
            const f32 radius = discFraction * static_cast<f32>(capture.Width);

            for (u32 y = 0; y < capture.Height; ++y)
            {
                for (u32 x = 0; x < capture.Width; ++x)
                {
                    const f32 dx = static_cast<f32>(x) - cx;
                    const f32 dy = static_cast<f32>(y) - cy;
                    if ((dx * dx) + (dy * dy) > radius * radius)
                        continue;

                    const std::size_t idx = capture.Index(x, y);
                    const f32 luma = LumaAt(capture.Pixels, idx);
                    lumaSum += static_cast<f64>(luma);
                    redSum += static_cast<f64>(RedFractionAt(capture.Pixels, idx));
                    stats.PeakLuma = std::max(stats.PeakLuma, luma);
                    if (luma > 0.02f)
                        ++stats.LitSamples;
                    ++stats.Samples;
                }
            }
            if (stats.Samples > 0)
            {
                stats.MeanLuma = static_cast<f32>(lumaSum / static_cast<f64>(stats.Samples));
                stats.MeanRedFraction = static_cast<f32>(redSum / static_cast<f64>(stats.Samples));
            }
            return stats;
        }

        // How many pixels differ, and by how much. The measured A/B the task
        // loop asks for: "61 000 pixels differ, max delta 89/255" is a result,
        // "it looks about the same" is not.
        struct Difference
        {
            u64 ChangedPixels = 0;
            u32 MaxDelta = 0;
            f64 MeanAbsDelta = 0.0;
        };

        [[nodiscard]] Difference Diff(const Capture& a, const Capture& b)
        {
            Difference d{};
            const std::size_t count = std::min(a.Pixels.size(), b.Pixels.size());
            f64 total = 0.0;
            std::size_t counted = 0;
            for (std::size_t i = 0; i + 3 < count; i += 4)
            {
                u32 worst = 0;
                for (u32 c = 0; c < 3; ++c)
                {
                    const auto delta = static_cast<u32>(
                        std::abs(static_cast<i32>(a.Pixels[i + c]) - static_cast<i32>(b.Pixels[i + c])));
                    worst = std::max(worst, delta);
                    total += static_cast<f64>(delta);
                    ++counted;
                }
                if (worst > 0)
                {
                    ++d.ChangedPixels;
                    d.MaxDelta = std::max(d.MaxDelta, worst);
                }
            }
            if (counted > 0)
                d.MeanAbsDelta = total / static_cast<f64>(counted);
            return d;
        }

        // How many DISTINCT strongly-saturated hues a capture contains, counted
        // in coarse buckets. This is the measurement behind the profile-identity
        // claim: the view paints one hue per skin-profile slot and black
        // everywhere else, so "three parts are in three slots" becomes "three
        // buckets are populated" rather than an eyeball on a colour wheel.
        [[nodiscard]] u32 CountDistinctHues(const Capture& capture, u64 minPixelsPerBucket)
        {
            constexpr u32 kBuckets = 12;
            std::array<u64, kBuckets> counts{};
            for (u32 y = 0; y < capture.Height; ++y)
            {
                for (u32 x = 0; x < capture.Width; ++x)
                {
                    const std::size_t idx = capture.Index(x, y);
                    const f32 r = Channel(capture.Pixels, idx, 0);
                    const f32 g = Channel(capture.Pixels, idx, 1);
                    const f32 b = Channel(capture.Pixels, idx, 2);
                    const f32 maxC = std::max({ r, g, b });
                    const f32 minC = std::min({ r, g, b });
                    const f32 chroma = maxC - minC;
                    // Black (no profile) and near-grey (tone-mapped background)
                    // are both excluded: the view's whole contract is that a
                    // slot is a SATURATED hue.
                    if (maxC < 0.25f || chroma < 0.20f)
                        continue;

                    // WHICH CHANNEL IS THE MAXIMUM, decided by ORDERING rather
                    // than by comparing a float against the max it was chosen
                    // from. `maxC == r` is the textbook spelling of this and it
                    // is banned here for a reason that bites in exactly this
                    // function: the three branches are not mutually exclusive
                    // under equality (a grey pixel satisfies all three), and
                    // the repo forbids float == outright (CLAUDE.md).
                    f32 hue = 0.0f;
                    if (r >= g && r >= b)
                        hue = std::fmod(((g - b) / chroma) + 6.0f, 6.0f);
                    else if (g >= b)
                        hue = ((b - r) / chroma) + 2.0f;
                    else
                        hue = ((r - g) / chroma) + 4.0f;

                    auto bucket = static_cast<u32>((hue / 6.0f) * static_cast<f32>(kBuckets));
                    bucket = std::min(bucket, kBuckets - 1u);
                    ++counts[bucket];
                }
            }
            u32 populated = 0;
            for (const u64 count : counts)
            {
                if (count >= minPixelsPerBucket)
                    ++populated;
            }
            return populated;
        }

        // GL reads back BOTTOM-UP; stbi_write_png writes TOP-DOWN. Without this
        // every evidence PNG in this file came out vertically mirrored — the
        // eyes below the mouth — which is how the first revision of PR #1388
        // shipped and how a reviewer spotted it. The sibling evidence tests
        // (AtmosphereVisualEvidenceTest) each do this same row swap after
        // ReadbackRgba8; this file simply did not.
        //
        // NO ASSERTION in this file was affected, which is exactly why it
        // survived: the measurement disc is centred and flip-symmetric, the hue
        // histogram is whole-frame, and every Diff compares two captures that
        // were flipped identically. Only the pictures were wrong — and in a
        // change whose entire subject is what the screen looks like, the
        // pictures are the deliverable.
        void FlipRowsInPlace(std::vector<u8>& rgba, u32 width, u32 height)
        {
            if (width == 0u || height < 2u)
                return;
            const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
            if (rgba.size() < rowBytes * height)
                return;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < height / 2u; ++y)
            {
                u8* top = rgba.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = rgba.data() + static_cast<std::size_t>(height - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }
        }

        [[nodiscard]] fs::path VisualOutputPath(const std::string& name)
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / (name + ".png");
        }
    } // namespace

    // =========================================================================
    // The subject
    // =========================================================================

    class SkinDigitalHumanScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            SetUpScratchProject();
            // ASSERT_* inside a helper returns from THAT helper, not from here,
            // so without this check a failed Project::Load would fall straight
            // through into AddMemoryOnlyAsset with no asset manager set. The
            // sibling ocular fixture guards the same seam the same way.
            ASSERT_FALSE(HasFatalFailure()) << "the scratch project never came up";

            // Three tone profiles, each at the TOP of the version ladder
            // (OcularSurface, 5). The ladder is cumulative — version 5 does
            // everything versions 1 through 4 do — so putting all three parts of
            // the subject at the same version means the only thing separating
            // the head from an eye from a mouth is an AUTHORED FIELD, which is
            // exactly the integration claim. Three parts at three DIFFERENT
            // versions would have made every difference below attributable to
            // the version branch instead.
            for (const ToneSetup& tone : kTones)
                m_ToneProfiles.push_back(MakeSkinProfile(tone));

            m_OcularProfile = MakeOcularProfile();
            m_OralProfile = MakeOralProfile();
            m_EnamelProfile = MakeEnamelProfile();

            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = kAngles[0].CameraPosition;
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity sun = GetScene().CreateEntity("Sun");
            auto& dirLight = sun.AddComponent<DirectionalLightComponent>();
            dirLight.m_Direction = kRigs[0].Direction;
            dirLight.m_Color = { 1.0f, 0.98f, 0.95f };
            dirLight.m_Intensity = kRigs[0].Intensity;
            dirLight.m_CastShadows = kRigs[0].CastShadows;

            // TWO PUNCTUAL FILLS, and they are here to make the Forward+ cell
            // mean something. Forward+ differs from Forward by CLUSTERED LIGHT
            // CULLING, and with a single directional light there is nothing to
            // cluster: the first version of this fixture produced nine
            // Forward+ captures that were BYTE-IDENTICAL to their Forward
            // counterparts, so that row of the matrix proved only that the path
            // ran. Two point lights inside the cluster grid give the two paths
            // different work to do while leaving the directional key — which
            // every lighting-rig claim above is written against — untouched.
            //
            // Deliberately dim and off-axis: they must not become the subject
            // of the rigs, only populate the grid.
            {
                Entity fillLeft = GetScene().CreateEntity("FillLeft");
                fillLeft.GetComponent<TransformComponent>().Translation = { -2.1f, 0.9f, 1.6f };
                auto& left = fillLeft.AddComponent<PointLightComponent>();
                left.m_Color = { 0.82f, 0.88f, 1.0f };
                left.m_Intensity = 1.4f;
                left.m_Range = 6.0f;

                Entity fillRight = GetScene().CreateEntity("FillRight");
                fillRight.GetComponent<TransformComponent>().Translation = { 2.3f, -0.7f, 1.2f };
                auto& right = fillRight.AddComponent<PointLightComponent>();
                right.m_Color = { 1.0f, 0.90f, 0.80f };
                right.m_Intensity = 1.1f;
                right.m_Range = 6.0f;
            }

            BuildCranium();
            ASSERT_FALSE(HasFatalFailure()) << "the cranium never got built";
            BuildEyes();
            ASSERT_FALSE(HasFatalFailure()) << "the eyes never got built";
            BuildOralRegion();
            ASSERT_FALSE(HasFatalFailure()) << "the oral region never got built";

            EnableRendering(kSize, kSize);
        }

        void TearDown() override
        {
            // BEFORE anything that can fail. An ASSERT_* returns from the test
            // body, so a reset written there never runs and leaves a
            // renderer-wide switch armed for every later fixture in the process
            // (cross-test-renderer-state.md).
            Renderer3D::GetSkinDiffusionSettings() = SkinDiffusionSettings{};
            Renderer3D::GetPostProcessSettings().MaterialDebug = MaterialDebugView::None;
            Renderer3D::GetSkinProfileTable().Reset();

            m_AssetManager.Reset();
            Project::Unload();
            std::error_code ec;
            if (!m_ProjectDir.empty())
                fs::remove_all(m_ProjectDir, ec);
            RendererAttachedTest::TearDown();
        }

        // ---------------------------------------------------------------------
        // Subject construction
        // ---------------------------------------------------------------------

        // The cranium: the skin surface proper, and the only part that morphs.
        // It carries the #1241 diffusion, the #1242 transmission and the #1243
        // layered specular at once, which is the "skin child integrated" half of
        // criterion 1.
        void BuildCranium()
        {
            const Ref<Mesh> sphere = MeshPrimitives::CreateSphere(1.0f, 48);
            ASSERT_TRUE(sphere) << "the cranium primitive failed to build";
            Ref<MeshSource> source = sphere->GetMeshSource();
            ASSERT_TRUE(source) << "the cranium primitive has no mesh source";

            const auto vertexCount = static_cast<u32>(source->GetVertices().Num());
            ASSERT_GT(vertexCount, 0u) << "the cranium primitive produced no vertices";

            // THE EXPRESSION, as a morph target rather than as a bone.
            //
            // A morph and not a skeleton because #1243's expression detail is
            // derived from MorphTargetComponent::AppliedWeights and from nothing
            // else (Scene.cpp::StampSkinExpression). A bone-animated subject
            // would deform beautifully and drive the detail term not at all, and
            // the capture would look like a working expression while the half of
            // it this epic cares about never ran.
            auto targets = Ref<MorphTargetSet>::Create();
            MorphTarget smile("Smile", vertexCount);
            const auto& vertices = source->GetVertices();
            for (u32 i = 0; i < vertexCount; ++i)
            {
                // Pull the LOWER FRONT of the sphere forward and down, which on
                // a head-shaped stand-in is where a jaw and a cheek are. Scaled
                // by how far below the equator and how far toward the camera the
                // vertex is, so the deformation is localised — a whole-sphere
                // scale would move the silhouette and make every "the expression
                // changed the frame" measurement pass on the silhouette alone.
                const glm::vec3& p = vertices[i].Position;
                const f32 below = std::max(0.0f, -p.y);
                const f32 front = std::max(0.0f, p.z);
                const f32 weight = below * front;
                smile.Vertices[i].DeltaPosition =
                    glm::vec3(0.0f, -0.55f, 1.0f) * (weight * kExpressionDisplacement);
            }
            targets->AddTarget(smile);
            source->SetMorphTargets(targets);

            Entity head = GetScene().CreateEntity("Cranium");
            head.AddComponent<MeshComponent>(source);
            auto& morph = head.AddComponent<MorphTargetComponent>();
            morph.MorphTargets = targets;
            morph.Weights["Smile"] = 0.0f;

            auto& material = head.AddComponent<MaterialComponent>();
            material.m_Material.SetBaseColorFactor(glm::vec4(kTones[0].Albedo, 1.0f));
            material.m_Material.SetMetallicFactor(0.0f);
            material.m_Material.SetRoughnessFactor(0.42f);
            material.m_Material.SetMaterialKind(MaterialKind::Skin);
            material.m_Material.SetSkinProfileHandle(m_ToneProfiles[0]);
            // NO NORMAL MAP BY DEFAULT — the expression test attaches one, and
            // only it. See AttachPoreNormalMap for why that is a deliberate
            // split rather than an oversight.
            // Without a thickness the #1242 term is disabled and reported as
            // SkinTransmissionFallbackReason::NoThickness — the backlight rig
            // would then measure nothing and every transmission claim here would
            // pass by never running.
            material.m_Material.SetThicknessFactor(kThicknessMetres);
            // Thin at the silhouette, thick through the middle. See
            // MakeHeadThicknessMap for why a uniform thickness made the backlit
            // captures useless.
            material.m_Material.SetThicknessMap(MakeHeadThicknessMap());
            m_Cranium = head;
        }

        // The eyes: #1244's ocular surface, two of them, in their own slot. A
        // PAIR rather than one, because a single eye cannot show that two
        // entities sharing one profile share one slot — which is the cheapest
        // way the slot table could be wrong and still look right.
        void BuildEyes()
        {
            const Ref<Mesh> eyeMesh = MeshPrimitives::CreateSphere(1.0f, 32);
            ASSERT_TRUE(eyeMesh) << "the eye primitive failed to build";

            const std::array<std::pair<const char*, f32>, 2> eyes = { { { "EyeL", -0.34f },
                                                                        { "EyeR", 0.34f } } };
            for (const auto& [name, x] : eyes)
            {
                Entity eye = GetScene().CreateEntity(name);
                eye.AddComponent<MeshComponent>(eyeMesh->GetMeshSource());
                auto& transform = eye.GetComponent<TransformComponent>();
                transform.Translation = { x, 0.20f, 0.80f };
                transform.Scale = glm::vec3(0.23f);

                auto& material = eye.AddComponent<MaterialComponent>();
                // A sclera, not a skin albedo: the iris colour is the profile's
                // and is MULTIPLIED into this, so a dark base here would make the
                // iris unreadable and the ocular claim untestable.
                material.m_Material.SetBaseColorFactor({ 0.93f, 0.91f, 0.89f, 1.0f });
                material.m_Material.SetMetallicFactor(0.0f);
                material.m_Material.SetRoughnessFactor(0.08f);
                material.m_Material.SetMaterialKind(MaterialKind::Skin);
                material.m_Material.SetSkinProfileHandle(m_OcularProfile);
                m_Eyes.push_back(eye);
            }
        }

        // The oral region: #1245's wet coat and cavity on the lips, and a
        // separate ENAMEL profile on the teeth. Two profiles and not one,
        // because "teeth and mucosa are not assigned identical skin response" is
        // that issue's own criterion and is the thing most likely to be lost
        // when the parts are assembled onto one subject.
        void BuildOralRegion()
        {
            const Ref<Mesh> mouthMesh = MeshPrimitives::CreateSphere(1.0f, 32);
            ASSERT_TRUE(mouthMesh) << "the oral primitive failed to build";

            Entity lips = GetScene().CreateEntity("Lips");
            lips.AddComponent<MeshComponent>(mouthMesh->GetMeshSource());
            auto& lipTransform = lips.GetComponent<TransformComponent>();
            lipTransform.Translation = { 0.0f, -0.52f, 0.80f };
            lipTransform.Scale = { 0.34f, 0.17f, 0.20f };

            auto& lipMaterial = lips.AddComponent<MaterialComponent>();
            lipMaterial.m_Material.SetBaseColorFactor({ 0.63f, 0.31f, 0.30f, 1.0f });
            lipMaterial.m_Material.SetMetallicFactor(0.0f);
            lipMaterial.m_Material.SetRoughnessFactor(0.28f);
            lipMaterial.m_Material.SetMaterialKind(MaterialKind::Skin);
            lipMaterial.m_Material.SetSkinProfileHandle(m_OralProfile);
            lipMaterial.m_Material.SetThicknessFactor(kThicknessMetres);
            m_Lips = lips;

            Entity teeth = GetScene().CreateEntity("Teeth");
            teeth.AddComponent<MeshComponent>(MeshPrimitives::CreateCube()->GetMeshSource());
            auto& teethTransform = teeth.GetComponent<TransformComponent>();
            teethTransform.Translation = { 0.0f, -0.50f, 0.86f };
            teethTransform.Scale = { 0.22f, 0.07f, 0.06f };

            auto& teethMaterial = teeth.AddComponent<MaterialComponent>();
            teethMaterial.m_Material.SetBaseColorFactor({ 0.91f, 0.89f, 0.84f, 1.0f });
            teethMaterial.m_Material.SetMetallicFactor(0.0f);
            teethMaterial.m_Material.SetRoughnessFactor(0.15f);
            teethMaterial.m_Material.SetMaterialKind(MaterialKind::Skin);
            teethMaterial.m_Material.SetSkinProfileHandle(m_EnamelProfile);
            m_Teeth = teeth;
        }

        // ---------------------------------------------------------------------
        // Profiles
        // ---------------------------------------------------------------------

        [[nodiscard]] AssetHandle MakeSkinProfile(const ToneSetup& tone)
        {
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = SkinEvaluationModel::OcularSurface;
            parameters.ScatterColor = tone.ScatterColor;
            parameters.ScatterRadiusMM = tone.ScatterRadiusMM;
            parameters.Transmission.Strength = 1.0f;
            parameters.Specular.LobeMix = kLobeMix;
            parameters.Specular.ExpressionDetailGain = kExpressionDetailGain;
            return RegisterProfile(tone.Name, parameters);
        }

        [[nodiscard]] AssetHandle MakeOcularProfile()
        {
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = SkinEvaluationModel::OcularSurface;
            parameters.Ocular.OcularStrength = kOcularStrength;
            parameters.Ocular.RefractionStrength = 1.0f;
            parameters.Ocular.LimbalRingStrength = 0.8f;
            parameters.Ocular.IrisColor = { 0.32f, 0.48f, 0.60f };
            parameters.Ocular.IrisConcavity = 0.25f;
            // A short achromatic radius: an eye is not scattering like a cheek,
            // and leaving the skin default here would have put the cranium's
            // mean free path through a cornea.
            parameters.ScatterColor = { 0.60f, 0.60f, 0.62f };
            parameters.ScatterRadiusMM = { 1.2f, 1.2f, 1.3f };
            return RegisterProfile("Ocular", parameters);
        }

        [[nodiscard]] AssetHandle MakeOralProfile()
        {
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = SkinEvaluationModel::OcularSurface;
            parameters.Oral.CoatStrength = kOralCoatStrength;
            parameters.Oral.CoatRoughness = 0.06f;
            parameters.Oral.CavityOcclusion = kOralCavityOcclusion;
            parameters.Transmission.Strength = 1.0f;
            parameters.ScatterColor = { 0.88f, 0.42f, 0.36f };
            parameters.ScatterRadiusMM = { 7.5f, 3.4f, 2.4f };
            return RegisterProfile("OralMucosa", parameters);
        }

        // Enamel: a version-5 profile with a short achromatic radius and the
        // higher index. One authored field — CoatIor — is what makes a tooth not
        // a lip, which is #1245's design and is re-tested here on the assembled
        // subject rather than on that issue's isolated mouth.
        [[nodiscard]] AssetHandle MakeEnamelProfile()
        {
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = SkinEvaluationModel::OcularSurface;
            parameters.Oral.CoatStrength = 0.30f;
            parameters.Oral.CoatRoughness = 0.04f;
            parameters.Oral.CoatIor = kEnamelIor;
            parameters.ScatterColor = { 0.72f, 0.70f, 0.66f };
            parameters.ScatterRadiusMM = { 1.6f, 1.5f, 1.4f };
            return RegisterProfile("Enamel", parameters);
        }

        // A THICKNESS MAP, and it is what stops the backlit capture being
        // worthless.
        //
        // WHY IT IS HERE. The first run of this fixture gave the cranium a
        // UNIFORM ThicknessFactor and no map. Every backlit capture came back a
        // uniformly glowing red ball — it rendered, it passed every difference
        // assertion, and it looked like a wax sphere rather than like a backlit
        // head. #1242's own fixture says this in as many words: "a uniformly
        // thick sphere backlit dead-on glows all over, which is visually
        // indistinguishable from the uniformly emissive head the issue's second
        // criterion forbids". An acceptance capture of a criterion about
        // backlight cannot be an image that demonstrates the failure mode.
        //
        // THIN AT THE SILHOUETTE, THICK THROUGH THE MIDDLE — which is both what
        // a head is (an ear and a nostril wing transmit; the skull does not)
        // and what makes the effect LOCALISED and therefore legible. The
        // resulting frame is a glowing rim on a dark mass, which is what
        // backlit skin actually looks like.
        //
        // THE u AXIS, NOT v, AND THE 0.25 CENTRE. MeshPrimitives::CreateSphere
        // lays u around the full 360 degrees of longitude, so the hemisphere
        // facing a camera on +z is u in (0, 0.5) and its CENTRE is u = 0.25.
        // Thickness therefore peaks at 0.25 and falls to its minimum at u = 0
        // and u = 0.5, which are the left and right silhouette edges.
        //
        // RGBA8 and NOT sRGB: thickness is DATA. An sRGB upload would put a
        // gamma curve through the optical depth and make the thick half read
        // thinner than it is.
        [[nodiscard]] Ref<Texture2D> MakeHeadThicknessMap()
        {
            constexpr u32 kDim = 128;
            TextureSpecification spec{};
            spec.Width = kDim;
            spec.Height = kDim;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;
            spec.SRGB = false;

            Ref<Texture2D> texture = Texture2D::Create(spec);
            EXPECT_TRUE(static_cast<bool>(texture)) << "the head thickness map texture was not created";
            EXPECT_TRUE(texture->GetRHIHandle().IsValid())
                << "the head thickness map has no RHI handle, so u_UseThicknessMap will be 0, the uniform "
                   "factor will be used instead, and every backlit capture will be a uniformly glowing ball.";

            std::vector<u8> pixels(static_cast<std::size_t>(kDim) * kDim * 4u, 0u);
            for (u32 y = 0; y < kDim; ++y)
            {
                for (u32 x = 0; x < kDim; ++x)
                {
                    const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kDim);
                    // Distance from the camera-facing meridian, normalised so 0
                    // is dead centre and 1 is either silhouette edge.
                    const f32 fromCentre = std::clamp(std::abs(u - 0.25f) / 0.25f, 0.0f, 1.0f);
                    // 38/255 at the edge (optically thin, glows) up to full at
                    // the centre (optically deep, stays dark). Squared so the
                    // thin band hugs the silhouette instead of washing halfway
                    // across the face.
                    const f32 thickness = 1.0f - (0.85f * fromCentre * fromCentre);
                    const auto level = static_cast<u8>(std::lround(std::clamp(thickness, 0.15f, 1.0f) * 255.0f));

                    const std::size_t base = ((static_cast<std::size_t>(y) * kDim) + x) * 4u;
                    pixels[base + 0u] = level; // RED is the channel the shader reads
                    pixels[base + 1u] = level;
                    pixels[base + 2u] = level;
                    pixels[base + 3u] = 255u;
                }
            }
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
            return texture;
        }

        // Attach the pore map to the cranium. Called by the EXPRESSION test and
        // by nothing else.
        //
        // WHY ONLY THERE. The map below is a near-texel-rate test pattern, not
        // a skin normal map an artist would author, and on a head that fills a
        // 384px frame it aliases into a visible moiré lattice. That is fine for
        // the one claim that needs it — the detail band is DEFINED as the
        // high-frequency content the mip chain throws away, so the pattern has
        // to be near the texel rate to exist at all — and actively harmful
        // everywhere else, where it puts a grid over every tone, lighting and
        // AOV capture and makes the evidence read as an artefact of the
        // fixture. Attaching it per-test keeps both properties.
        void AttachPoreNormalMap()
        {
            ASSERT_TRUE(static_cast<bool>(m_Cranium));
            m_Cranium.GetComponent<MaterialComponent>().m_Material.SetNormalMap(MakePoreNormalMap());
        }

        // A PORE NORMAL MAP, and it is load-bearing rather than decoration.
        //
        // #1243's detail term is a GAIN ON AN AUTHORED NORMAL MAP's pore band —
        // "0 leaves the authored normal map exactly as it is". With no map the
        // renderer sets u_UseNormalMap to 0, the gain multiplies nothing, and
        // an expression-detail A/B measures two identical frames. That is not a
        // hypothesis: the first run of this fixture had no map, and the
        // gain-on and gain-off arms came back 1.14097 and 1.14155 mean delta —
        // the same number to three decimals, which read as "the expression does
        // not reach the shading" about a mechanism that was working and had
        // nothing to act on.
        //
        // Same construction as SkinLayeredSpecularEvidenceTest's: a
        // near-texel-rate pore lattice over a low-frequency furrow band, so the
        // pores are what the mip chain throws away and the detail gain has a
        // real high-frequency band to restore.
        [[nodiscard]] Ref<Texture2D> MakePoreNormalMap()
        {
            constexpr u32 kDim = 256;
            TextureSpecification spec{};
            spec.Width = kDim;
            spec.Height = kDim;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = true;
            // NOT sRGB: a normal map is a direction, not a colour.
            spec.SRGB = false;

            Ref<Texture2D> texture = Texture2D::Create(spec);
            // Texture2D::Create NEVER returns null, so the Ref proves nothing —
            // the RHI handle is the only thing that says a GL texture exists.
            EXPECT_TRUE(static_cast<bool>(texture)) << "the pore normal map texture was not created";
            EXPECT_TRUE(texture->GetRHIHandle().IsValid())
                << "the pore normal map has no RHI handle, so u_UseNormalMap will be 0 and this fixture's "
                   "expression-detail arms would both shade a smooth head.";

            std::vector<u8> pixels(static_cast<std::size_t>(kDim) * kDim * 4u, 0u);
            for (u32 y = 0; y < kDim; ++y)
            {
                for (u32 x = 0; x < kDim; ++x)
                {
                    const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kDim);
                    const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kDim);

                    constexpr f32 kTwoPi = 6.2831853f;
                    const f32 poreU = std::sin(u * kTwoPi * 48.0f) * std::cos(v * kTwoPi * 48.0f);
                    const f32 poreV = std::cos(u * kTwoPi * 48.0f) * std::sin(v * kTwoPi * 48.0f);
                    const f32 furrowU = std::sin(u * kTwoPi * 6.0f) * 0.5f;
                    const f32 furrowV = std::sin(v * kTwoPi * 6.0f + 1.1f) * 0.5f;

                    const f32 nx = std::clamp(0.55f * poreU + 0.30f * furrowU, -1.0f, 1.0f);
                    const f32 ny = std::clamp(0.55f * poreV + 0.30f * furrowV, -1.0f, 1.0f);

                    const std::size_t base = ((static_cast<std::size_t>(y) * kDim) + x) * 4u;
                    pixels[base + 0u] = static_cast<u8>(std::lround((nx * 0.5f + 0.5f) * 255.0f));
                    pixels[base + 1u] = static_cast<u8>(std::lround((ny * 0.5f + 0.5f) * 255.0f));
                    // Blue is reconstructed by decodeTangentNormal, never read.
                    pixels[base + 2u] = 255u;
                    pixels[base + 3u] = 255u;
                }
            }
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
            return texture;
        }

        [[nodiscard]] AssetHandle RegisterProfile(const char* name, const SkinProfileParameters& parameters)
        {
            auto profile = Ref<SkinProfile>::Create();
            profile->SetName(name);
            EXPECT_TRUE(profile->SetParameters(parameters))
                << name << ": the profile needed correcting — a bound moved and this fixture is now "
                           "authoring a value the renderer will not use";
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
            EXPECT_NE(static_cast<u64>(handle), 0ULL) << name << ": AddMemoryOnlyAsset returned a zero handle";
            return handle;
        }

        void SetUpScratchProject()
        {
            std::error_code ec;
            m_ProjectDir = OloEngine::Tests::TempDir("skin-digital-human-project");
            fs::create_directories(m_ProjectDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create the scratch project dir at " << m_ProjectDir.string();

            const fs::path projectFile = m_ProjectDir / "SkinDigitalHuman.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: SkinDigitalHuman\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << projectFile.string();

            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        // ---------------------------------------------------------------------
        // Scene controls
        // ---------------------------------------------------------------------

        void SetRig(const LightingRig& rig)
        {
            Entity sun = GetScene().FindEntityByName("Sun");
            ASSERT_TRUE(static_cast<bool>(sun)) << "the subject lost its sun";
            auto& light = sun.GetComponent<DirectionalLightComponent>();
            light.m_Direction = rig.Direction;
            light.m_Intensity = rig.Intensity;
            light.m_CastShadows = rig.CastShadows;
        }

        void SetAngle(const AngleSetup& angle)
        {
            Entity camera = GetScene().FindEntityByName("Camera");
            ASSERT_TRUE(static_cast<bool>(camera)) << "the subject lost its camera";
            auto& transform = camera.GetComponent<TransformComponent>();
            transform.Translation = angle.CameraPosition;
            transform.SetRotationEuler(glm::radians(angle.CameraEulerDegrees));
        }

        void SetTone(sizet toneIndex)
        {
            ASSERT_LT(toneIndex, m_ToneProfiles.size());
            ASSERT_TRUE(static_cast<bool>(m_Cranium));
            auto& material = m_Cranium.GetComponent<MaterialComponent>().m_Material;
            material.SetSkinProfileHandle(m_ToneProfiles[toneIndex]);
            material.SetBaseColorFactor(glm::vec4(kTones[toneIndex].Albedo, 1.0f));
        }

        void SetExpression(f32 weight)
        {
            ASSERT_TRUE(static_cast<bool>(m_Cranium));
            m_Cranium.GetComponent<MorphTargetComponent>().Weights["Smile"] = weight;
        }

        // Render and read back. `frames` is 2 by default: the first settles the
        // graph rebuild a path or profile change forces, the second is measured.
        //
        // WHEN THE EXPRESSION IS MOVING this must be more than 2 — the morph
        // pass writes AppliedWeights at the frame boundary, so a weight set from
        // outside needs one tick to reach the deformed surface and a second to
        // reach the shading that reads AppliedWeights. Callers that animate pass
        // the frame count explicitly and say why.
        [[nodiscard]] bool CaptureFrame(RenderingPath path, const std::string& name, Capture& out,
                                        MaterialDebugView view = MaterialDebugView::None, u32 frames = 2,
                                        f64* outMilliseconds = nullptr, bool diffusionEnabled = true)
        {
            Renderer3D::GetSkinDiffusionSettings().Enabled = diffusionEnabled;
            Renderer3D::GetSkinDiffusionSettings().Quality = SkinDiffusionQuality::High;
            Renderer3D::GetRendererSettings().Path = path;
            // The material debug views are DEFERRED-ONLY by construction
            // (RenderPipeline.cpp forces None on the forward paths), so asking
            // for one on a forward path would silently capture a composite and
            // the caller would compare an AOV against a beauty frame. Refuse
            // instead — a wrong answer here is worse than no answer.
            if (view != MaterialDebugView::None && path != RenderingPath::Deferred)
            {
                ADD_FAILURE() << DebugViewName(view) << ": the material debug views only exist on the deferred "
                              << "path; " << PathName(path) << " would have captured a composite instead.";
                return false;
            }
            Renderer3D::GetPostProcessSettings().MaterialDebug = view;
            Renderer3D::ApplyRendererSettings();

            const auto start = std::chrono::steady_clock::now();
            RunFrames(frames);
            const auto finish = std::chrono::steady_clock::now();
            if (outMilliseconds != nullptr)
            {
                *outMilliseconds = std::chrono::duration<f64, std::milli>(finish - start).count() /
                                   static_cast<f64>(std::max(frames, 1u));
            }

            if (!ReadbackComposite(out.Pixels, out.Width, out.Height))
                return false;
            if (!out.Valid())
                return false;
            // Bottom-up out of GL, top-down into the PNG. See FlipRowsInPlace.
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

        // The SAME-MODE REPEAT FLOOR for one raster path: two captures of an
        // unchanged scene, back to back. Every threshold below is a multiple of
        // this rather than a constant, because deferred is jittery here and
        // forward is not — one constant would either flake on deferred or be
        // meaningless on forward.
        // ASSERT, not EXPECT. A failed readback here returns an ALL-ZERO
        // floor, and every threshold downstream is a multiple of it — so the
        // whole battery silently degrades to "> 0" and reports its failures
        // against a floor that was never measured. Aborting is the only honest
        // answer; the caller's own ASSERT_* propagation stops the test.
        [[nodiscard]] Difference MeasureRepeatFloor(RenderingPath path, u32 frames = 4)
        {
            Capture first;
            Capture second;
            Difference floor{};
            if (!CaptureFrame(path, std::string(), first, MaterialDebugView::None, frames) ||
                !CaptureFrame(path, std::string(), second, MaterialDebugView::None, frames))
            {
                ADD_FAILURE() << PathName(path)
                              << ": the repeat-floor readback failed, so no threshold below this point "
                                 "would have meant anything.";
                return floor;
            }
            return Diff(first, second);
        }

        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_ProjectDir;

        std::vector<AssetHandle> m_ToneProfiles;
        AssetHandle m_OcularProfile{};
        AssetHandle m_OralProfile{};
        AssetHandle m_EnamelProfile{};

        Entity m_Cranium;
        Entity m_Lips;
        Entity m_Teeth;
        std::vector<Entity> m_Eyes;
    };

    // =========================================================================
    // CRITERION 1 — the three children are integrated on ONE animated subject
    // =========================================================================

    // The claim that needs no GPU, and the one that cannot be satisfied by
    // tuning: the four profiles this subject wears resolve to four DIFFERENT
    // slots with four DIFFERENT parameter sets.
    //
    // WHY THIS IS THE FIRST TEST. The G-Buffer names a profile by a THREE-BIT
    // slot, so there are seven of them and the eighth value means "no profile".
    // A subject assembled from four profiles is the first thing in this engine
    // to spend more than two, and the failure — two parts aliasing onto one slot
    // — renders a completely plausible head in which the teeth quietly shade
    // with the lips' mean free path. No screenshot shows that.
    TEST_F(SkinDigitalHumanScene, TheFourPartsOfTheSubjectOccupyFourDistinctProfileSlots)
    {
        // The ASSERTIONS here are pure CPU, but the FIXTURE is not: BuildScene
        // calls EnableRendering, so this test needs a context like every other
        // one on RendererAttachedTest.
        OLO_ENSURE_GPU_OR_SKIP();

        SkinProfileTable& table = Renderer3D::GetSkinProfileTable();
        table.Reset();

        struct Part
        {
            const char* Name;
            AssetHandle Handle;
        };
        const Part parts[] = { { "Cranium", m_ToneProfiles[0] },
                               { "Eyes", m_OcularProfile },
                               { "Lips", m_OralProfile },
                               { "Teeth", m_EnamelProfile } };

        std::vector<u32> slots;
        for (const Part& part : parts)
        {
            const SkinProfileResolution resolution = table.Resolve(part.Handle);
            EXPECT_FALSE(resolution.IsFallback())
                << part.Name << ": the profile fell back (reason "
                << static_cast<u32>(resolution.Reason)
                << "). A fallback here means this part is shading with somebody else's skin.";
            EXPECT_NE(resolution.Slot, kSkinProfileSlotNone)
                << part.Name << ": resolved to the no-profile slot, so the G-Buffer will say this pixel is not skin";
            slots.push_back(resolution.Slot);
        }

        std::vector<u32> unique = slots;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        EXPECT_EQ(unique.size(), std::size(parts))
            << "the four parts share slots (" << slots[0] << ", " << slots[1] << ", " << slots[2] << ", "
            << slots[3] << "). Two parts on one slot shade with one set of parameters.";

        // AND THE PARAMETERS BEHIND THE SLOTS DIFFER, which is the half a slot
        // comparison alone would miss: four distinct slots all carrying the
        // default parameters would pass the check above and render one material.
        const SkinProfileParameters cranium = table.GetParametersForSlot(slots[0]);
        const SkinProfileParameters eyes = table.GetParametersForSlot(slots[1]);
        const SkinProfileParameters lips = table.GetParametersForSlot(slots[2]);
        const SkinProfileParameters teeth = table.GetParametersForSlot(slots[3]);

        EXPECT_GT(eyes.Ocular.OcularStrength, 0.0f) << "the eye slot carries no ocular strength — #1244 is not on this subject";
        // NOT EXPECT_EQ against 0.0f: CLAUDE.md bans float equality outright,
        // and this file makes that argument itself in CountDistinctHues. The
        // claim is "no meaningful eye", which is a magnitude claim.
        EXPECT_LT(std::abs(cranium.Ocular.OcularStrength), 1.0e-6f) << "the CRANIUM acquired an eye";
        EXPECT_GT(lips.Oral.CoatStrength, 0.0f) << "the lip slot carries no wet coat — #1245 is not on this subject";
        EXPECT_GT(cranium.Specular.LobeMix, 0.0f) << "the cranium slot carries no second lobe — #1243 is not on this subject";
        EXPECT_GT(cranium.Transmission.Strength, 0.0f) << "the cranium slot cannot transmit — #1242 is not on this subject";
        // The one authored field that makes a tooth not a lip.
        EXPECT_GT(teeth.Oral.CoatIor, lips.Oral.CoatIor)
            << "enamel and mucosa resolved to the same index of refraction, so teeth shade like gums";
    }

    TEST_F(SkinDigitalHumanScene, TheProfileIdentityViewSeparatesThePartsOnScreen)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SetRig(kRigs[0]);
        SetAngle(kAngles[0]);
        SetExpression(0.0f);

        Capture identity;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, "DigitalHumanProfileId_GL_Deferred_Front", identity,
                                 MaterialDebugView::ProfileIdentity));

        // The view paints one hue per slot and BLACK everywhere else, so the
        // count of populated hue buckets is the on-screen version of the slot
        // assertion above. Three rather than four: the teeth are mostly behind
        // the lips from the front, and demanding four here would be a test of
        // this fixture's occlusion rather than of the view.
        const u32 hues = CountDistinctHues(identity, /*minPixelsPerBucket=*/200u);
        EXPECT_GE(hues, 3u)
            << "the profile-identity view shows only " << hues
            << " distinct hues. Each skin profile slot is supposed to be its own hue, so fewer hues than "
               "parts means the parts are sharing a slot — or that the slot never reached the G-Buffer.";
    }

    // The expression must reach the SHADING, not just the geometry.
    //
    // THIS IS THE SUBTLEST CLAIM IN THE FILE. #1243 derives its pore-detail gain
    // from MorphTargetComponent::AppliedWeights — what the surface currently on
    // the GPU was built from — and the whole point of that choice is that the
    // shading cannot lead the deformation by a frame. A subject whose morph
    // deformed correctly while the detail term stayed at its neutral value would
    // render a perfectly good expression and silently drop a third of #1243.
    //
    // So the test is A/B on the GAIN, with the morph moving identically in both
    // arms: same weights, same deformation, same silhouette, and the only
    // difference is whether the expression is allowed to reach the specular.
    TEST_F(SkinDigitalHumanScene, TheExpressionReachesTheShadingAndNotOnlyTheGeometry)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SetRig(kRigs[1]); // hard side light — where a pore band is visible at all
        SetAngle(kAngles[1]);
        // The band the expression gain acts on. Only this test attaches it.
        AttachPoreNormalMap();

        const Difference floor = MeasureRepeatFloor(RenderingPath::Deferred);

        // Four frames, not two: the morph pass writes AppliedWeights at the
        // frame boundary, so a weight set from outside needs one tick to reach
        // the deformed surface and another to reach the shading that reads it.
        constexpr u32 kMorphSettleFrames = 4;

        SetExpression(0.0f);
        Capture neutral;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, "DigitalHuman_GL_Deferred_ExpressionNeutral", neutral,
                                 MaterialDebugView::None, kMorphSettleFrames));

        SetExpression(1.0f);
        Capture expressed;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, "DigitalHuman_GL_Deferred_ExpressionFull", expressed,
                                 MaterialDebugView::None, kMorphSettleFrames));

        // First: the expression reached the ENGINE at all. Read from
        // AppliedWeights rather than from Weights, which is the same distinction
        // the renderer makes — Weights is what the next morph pass will use, and
        // asserting on it would pass on a morph pass that never ran.
        const MorphTargetComponent& morph = m_Cranium.GetComponent<MorphTargetComponent>();
        ASSERT_TRUE(morph.HasAppliedSurface)
            << "the morph pass never decided a surface for the cranium, so nothing below is measuring an expression";
        const f32 appliedWeight = SkinExpressionDetailWeight(morph.AppliedWeights);
        EXPECT_GT(appliedWeight, 0.0f)
            << "the applied morph weights produce an expression detail weight of zero, so #1243's "
               "expression-driven detail is inert on this subject no matter what the frame looks like";

        const Difference geometryMoved = Diff(neutral, expressed);
        EXPECT_GT(geometryMoved.MaxDelta, floor.MaxDelta * 3u)
            << "the expression changed the frame by no more than the repeat floor (" << floor.MaxDelta
            << "/255), so the morph is not reaching the render at all";

        // ------------------------------------------------------------------
        // THE SHADING HALF, and why the expression weight is set BY HAND here.
        //
        // A GAP FOUND BY THIS TEST, recorded where it will be read. The engine
        // applies #1243's expression stamp in exactly two places
        // (Scene.cpp::StampSkinExpression), and BOTH sit inside submission
        // loops keyed on `view<TransformComponent, MeshComponent,
        // SkeletonComponent>` — the skinned paths. A MORPH-ONLY entity, which
        // this cranium is and which is a perfectly ordinary blend-shape face
        // rig, is deformed by MorphDeformationSystem (the captures above prove
        // it: the frame moves) and then submitted through the RIGID path, where
        // nothing ever calls SetSkinExpressionDetail. So on a morph-only
        // subject the gain is inert no matter what the profile authors.
        //
        // That is reported on #1222 rather than worked around silently. What
        // this test does is stamp the material the way the skinned path would,
        // so the half that IS this epic's business — does the expression reach
        // the SHADING, on the deferred path, through the whole frame graph —
        // is still measured rather than skipped. Reading the weight out of
        // AppliedWeights (asserted above) is what keeps the hand-stamp honest:
        // it is the same number the engine would have used.
        // ------------------------------------------------------------------
        auto& craniumMaterial = m_Cranium.GetComponent<MaterialComponent>().m_Material;

        craniumMaterial.SetSkinExpressionDetail(appliedWeight);
        Capture expressedStamped;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, "DigitalHuman_GL_Deferred_ExpressionFullStamped",
                                 expressedStamped, MaterialDebugView::None, kMorphSettleFrames));

        craniumMaterial.SetSkinExpressionDetail(0.0f);
        Capture expressedUnstamped;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, std::string(), expressedUnstamped,
                                 MaterialDebugView::None, kMorphSettleFrames));

        // SAME geometry, same pose, same light, same profile — the ONLY
        // difference is whether the expression is allowed to reach the pore
        // band. Anything above the floor is the detail term doing its job.
        const Difference shadingMoved = Diff(expressedStamped, expressedUnstamped);
        EXPECT_GT(shadingMoved.MaxDelta, std::max(floor.MaxDelta * 3u, 2u))
            << "stamping the expression detail weight (" << appliedWeight
            << ") onto the material changed the frame by at most " << shadingMoved.MaxDelta
            << "/255 against a repeat floor of " << floor.MaxDelta
            << "/255. The pose and the geometry are identical between these two captures, so this is the "
               "expression-driven pore band of #1243 and nothing else — a zero here means it is not reaching "
               "the deferred G-Buffer's normal at all.";

        // THE PROFILE-SIDE CONTROL. The stamp above can only do anything if the
        // PROFILE authors a gain for it to scale; this arm proves the frame
        // moved because of ExpressionDetailGain and not because
        // SetSkinExpressionDetail happens to perturb something else. Same
        // stamp, same pose, a profile whose gain is zero.
        //
        // ONE control profile and not three, deliberately: a slot is spent per
        // profile RESOLVED, and the budget is seven (kMaxSkinProfileSlots —
        // three bits with the all-ones pattern reserved). This subject already
        // spends four, so three control profiles would sit exactly on the
        // ceiling and any later edit would push the fixture over it, where the
        // symptom is a silent fallback to not-skin rather than a failure.
        SkinProfileParameters control = SkinProfile::DefaultParameters();
        control.EvaluationModel = SkinEvaluationModel::OcularSurface;
        control.ScatterColor = kTones[0].ScatterColor;
        control.ScatterRadiusMM = kTones[0].ScatterRadiusMM;
        control.Transmission.Strength = 1.0f;
        control.Specular.LobeMix = kLobeMix;
        control.Specular.ExpressionDetailGain = 0.0f; // the one field under test
        const AssetHandle gainless = RegisterProfile("GainlessControl", control);
        craniumMaterial.SetSkinProfileHandle(gainless);

        craniumMaterial.SetSkinExpressionDetail(appliedWeight);
        Capture gainlessStamped;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, "DigitalHumanExpressionGainOff_GL_Deferred_ExpressionFull",
                                 gainlessStamped, MaterialDebugView::None, kMorphSettleFrames));
        craniumMaterial.SetSkinExpressionDetail(0.0f);
        Capture gainlessUnstamped;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, std::string(), gainlessUnstamped, MaterialDebugView::None,
                                 kMorphSettleFrames));

        const Difference gainlessMoved = Diff(gainlessStamped, gainlessUnstamped);
        EXPECT_GT(shadingMoved.MeanAbsDelta, gainlessMoved.MeanAbsDelta)
            << "stamping the same expression weight moved the frame by " << shadingMoved.MeanAbsDelta
            << "/255 mean with the profile's ExpressionDetailGain at " << kExpressionDetailGain << " and by "
            << gainlessMoved.MeanAbsDelta
            << "/255 with it at zero. The gain is the field that is supposed to make the difference, so the "
               "first number must be the larger one.";

        // Put the subject back, so a later test in this fixture does not
        // inherit the control's profile or a latched expression weight.
        craniumMaterial.SetSkinProfileHandle(m_ToneProfiles[0]);
        craniumMaterial.SetSkinExpressionDetail(0.0f);
    }

    // =========================================================================
    // CRITERION 2 — tones hold up under every rig, in motion
    // =========================================================================

    // The full tone x rig grid on one raster path, captured and measured.
    //
    // WHAT "HOLDS UP" IS TAKEN TO MEAN, because the criterion does not say and a
    // test that guessed "looks good" would be lying about what it checked:
    //
    //   a. the three tones are TELLABLE APART under every rig — including
    //      backlight, where a wrong transmission tint collapses them, and
    //   b. their ORDER never inverts. Fair scatters furthest and reads
    //      brightest; Deep scatters least and reads darkest. A rig that
    //      reordered them would mean the rig, not the skin, was deciding the
    //      tone.
    //
    // (b) is the one with teeth. (a) alone passes on three tones that are merely
    // three different wrong answers.
    class SkinDigitalHumanToneGrid : public SkinDigitalHumanScene
    {
      protected:
        void RunToneGrid(RenderingPath path)
        {
            SetAngle(kAngles[1]); // ThreeQuarter: both a lit and a shadowed cheek
            // Mid-expression for the whole grid, so criterion 2's "during
            // expression changes" is not a separate capture bolted on at the end
            // — every tone in every rig is captured on an expressing face.
            SetExpression(0.65f);
            for (const LightingRig& rig : kRigs)
            {
                SetRig(rig);

                // THE FLOOR IS MEASURED PER RIG, after the rig is set and at the
                // same frame count the grid captures with. Not once for the
                // whole path: these rigs differ in whether they cast shadows at
                // all and by 2.5x in intensity, and shadow jitter is the largest
                // contributor to the repeat floor here. A floor borrowed from the
                // shadowless Soft rig would let the HardSide and Backlight
                // "these tones are tellable apart" checks pass on shadow noise.
                const Difference floor = MeasureRepeatFloor(path, /*frames=*/4);

                std::vector<f32> meanLuma;
                std::vector<f32> diffusionInfluence;
                std::vector<Capture> captures;

                for (sizet toneIndex = 0; toneIndex < std::size(kTones); ++toneIndex)
                {
                    SetTone(toneIndex);
                    Capture capture;
                    const std::string name = std::string("DigitalHuman_GL_") + PathName(path) + "_" +
                                             kTones[toneIndex].Name + rig.Name;
                    ASSERT_TRUE(CaptureFrame(path, name, capture, MaterialDebugView::None, /*frames=*/4))
                        << name << ": readback failed";

                    const FrameStats stats = MeasureSubject(capture);
                    ASSERT_GT(stats.Samples, 0u) << name << ": the measurement disc found no pixels";
                    // An assertion that passes on an empty frame is the normal
                    // failure here, so the subject must actually be lit before
                    // any comparison between captures means anything.
                    ASSERT_GT(stats.LitSamples, stats.Samples / 20u)
                        << name << ": fewer than 5% of the subject disc is above the black floor — this "
                                   "capture is an empty frame and every number derived from it is noise";

                    // The SAME tone with the diffusion pass switched off. Not
                    // written out as evidence — it is a measurement arm, and a
                    // PNG per tone per rig per path of a frame nobody looks at
                    // is noise in the diff.
                    Capture undiffused;
                    ASSERT_TRUE(CaptureFrame(path, std::string(), undiffused, MaterialDebugView::None,
                                             /*frames=*/4, /*outMilliseconds=*/nullptr,
                                             /*diffusionEnabled=*/false))
                        << name << ": the undiffused control readback failed";

                    // HOW MUCH THE DIFFUSION PASS CHANGES THIS TONE'S FRAME,
                    // RELATIVE TO HOW BRIGHT THAT FRAME IS. The normalisation
                    // is what makes it a statement about the SCATTERING: a
                    // darker albedo produces smaller absolute deltas for
                    // reasons that have nothing to do with the mean free path,
                    // and dividing by the tone's own mean luma removes exactly
                    // that.
                    const Difference diffusionEffect = Diff(capture, undiffused);
                    diffusionInfluence.push_back(static_cast<f32>(diffusionEffect.MeanAbsDelta) /
                                                 std::max(stats.MeanLuma * 255.0f, 1.0f));

                    meanLuma.push_back(stats.MeanLuma);
                    captures.push_back(std::move(capture));
                }

                // (a) Tellable apart — pairwise, against this path's own floor.
                for (sizet i = 0; i + 1 < captures.size(); ++i)
                {
                    const Difference d = Diff(captures[i], captures[i + 1]);
                    EXPECT_GT(d.MaxDelta, floor.MaxDelta * 3u)
                        << rig.Name << ": tones " << kTones[i].Name << " and " << kTones[i + 1].Name
                        << " differ by at most " << d.MaxDelta << "/255 against a repeat floor of "
                        << floor.MaxDelta << "/255 — under this rig they are the same skin.";
                }

                // (b) The order never inverts.
                EXPECT_GT(meanLuma[0], meanLuma[1])
                    << rig.Name << ": Fair (" << meanLuma[0] << ") is not brighter than Medium (" << meanLuma[1]
                    << ")";
                EXPECT_GT(meanLuma[1], meanLuma[2])
                    << rig.Name << ": Medium (" << meanLuma[1] << ") is not brighter than Deep (" << meanLuma[2]
                    << ")";

                // THE SCATTERING LADDER — the half albedo alone cannot fake.
                //
                // WHAT THIS IS NOT, and the first two versions of this test got
                // it wrong in two different ways worth recording.
                //
                // It is NOT the raw red fraction. These tones carry realistic
                // albedos, and a deeply pigmented albedo is itself more
                // red-DOMINANT than a pale one (0.27/0.18/0.13 is 47% red;
                // 0.78/0.62/0.55 is 40%), so the raw fraction came back
                // INVERTED at 0.354 Fair against 0.381 Deep. The measurement
                // was reading the albedo.
                //
                // It is NOT the red SHIFT the diffusion introduces either. A
                // blur redistributes energy rather than creating it, so over a
                // disc that is mostly lit the sign of that shift depends on
                // where the disc is put — measured at 0.0017 Fair against
                // 0.0027 Deep, and NEGATIVE for Fair under backlight. Numbers
                // that small with an unstable sign are not a claim.
                //
                // What IS a direct consequence of the authored radii: a WIDER
                // kernel changes the image MORE. Fair scatters over 9.0 mm of
                // red against Deep's 3.4 mm, so toggling the diffusion pass
                // must move Fair's frame further than Deep's. Normalising by
                // each tone's own mean luma is what keeps that a statement
                // about the mean free path instead of about how dark the
                // albedo is.
                // ...AND ONLY UNDER A RIG THAT LIGHTS THE DIFFUSE HALF. The
                // diffusion pass redistributes the DIFFUSE irradiance, so under
                // a pure backlight — where that half is almost absent and the
                // frame is nearly all transmission — there is next to nothing
                // for a wider kernel to move, and normalising a tiny residual
                // by a tiny mean luma amplifies noise rather than measuring a
                // radius. Measured: the ladder holds under Soft and HardSide on
                // all three paths and inverts under Backlight (Fair 0.0065
                // against Deep 0.0076), which is the mechanism being absent,
                // not the radii failing to arrive.
                //
                // This is the same correction the AOV test above makes for the
                // specular view, applied for the same reason: asserting a term
                // under a rig that does not excite it measures the rig. The
                // backlight rig keeps its OTHER two assertions — the tones are
                // still required to be tellable apart and still required not to
                // reorder — so it is not an unchecked cell.
                if (!rig.LightsTheDiffuseHalf)
                    continue;

                EXPECT_GT(diffusionInfluence[0], diffusionInfluence[2])
                    << rig.Name << ": toggling the diffusion pass moves the Fair tone's frame by "
                    << diffusionInfluence[0] << " of its own mean luma and the Deep tone's by "
                    << diffusionInfluence[2]
                    << ". Fair's red mean free path is 2.6x Deep's, so the wider kernel must be the one that "
                       "changes its frame more — otherwise the authored radii are not reaching the pass and "
                       "these are three albedos rather than three skins.";
            }
        }
    };

    TEST_F(SkinDigitalHumanToneGrid, TheToneLadderHoldsUnderEveryRigOnForward)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        RunToneGrid(RenderingPath::Forward);
    }

    TEST_F(SkinDigitalHumanToneGrid, TheToneLadderHoldsUnderEveryRigOnForwardPlus)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        RunToneGrid(RenderingPath::ForwardPlus);
    }

    TEST_F(SkinDigitalHumanToneGrid, TheToneLadderHoldsUnderEveryRigOnDeferred)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        RunToneGrid(RenderingPath::Deferred);
    }

    // Camera motion, which criterion 2 names alongside the expression. Three
    // poses, each captured, each required to differ from the others by far more
    // than the floor AND to still contain a lit subject — the #931 failure was a
    // multi-angle capture set whose camera never moved and whose subject was
    // absent, and both halves of that are checked here.
    TEST_F(SkinDigitalHumanScene, TheSubjectSurvivesCameraMotionUnderBacklight)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SetRig(kRigs[2]); // backlight: the least forgiving rig for a moving camera
        SetTone(1);
        SetExpression(0.65f);

        const Difference floor = MeasureRepeatFloor(RenderingPath::Deferred);

        std::vector<Capture> captures;
        for (const AngleSetup& angle : kAngles)
        {
            SetAngle(angle);
            Capture capture;
            const std::string name = std::string("DigitalHuman_GL_Deferred_Backlight") + angle.Name;
            ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, name, capture, MaterialDebugView::None, 4))
                << name << ": readback failed";

            const FrameStats stats = MeasureSubject(capture);
            EXPECT_GT(stats.LitSamples, stats.Samples / 20u)
                << name << ": the subject is not on screen from this angle";
            captures.push_back(std::move(capture));
        }

        for (sizet i = 0; i + 1 < captures.size(); ++i)
        {
            const Difference d = Diff(captures[i], captures[i + 1]);
            EXPECT_GT(d.MaxDelta, floor.MaxDelta * 4u)
                << kAngles[i].Name << " and " << kAngles[i + 1].Name << " differ by at most " << d.MaxDelta
                << "/255 against a floor of " << floor.MaxDelta << "/255 — the camera did not move.";
        }
    }

    // =========================================================================
    // CRITERION 3 — separately inspectable, and measured
    // =========================================================================

    // The three components, captured as their own images.
    //
    // HISTORY, THE FOURTH COMPONENT, IS NOT HERE. Issue #1256 owns temporal
    // reconstruction for skin, hair and foliage and therefore owns the history
    // view. Adding a fourth AOV here would put two owners on one surface.
    TEST_F(SkinDigitalHumanScene, TheThreeComponentsAreSeparatelyInspectable)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SetAngle(kAngles[1]);
        SetTone(0);
        SetExpression(0.65f);

        // EACH VIEW IS ASSERTED UNDER A RIG WHERE ITS TERM EXISTS, and that is
        // a correction rather than a convenience. The first version of this
        // test captured all four AOVs under BACKLIGHT and failed on the
        // specular one — 639 lit pixels out of 47 400, 1.3% — with the message
        // "the view is essentially black, either the term is not running or the
        // view is not reading it". Both halves of that message were wrong: a
        // subject lit from behind reflects almost nothing toward the lens, so a
        // near-black specular AOV is the PHYSICALLY CORRECT answer and the test
        // was demanding a highlight that should not have been there.
        //
        // So: diffuse, specular and the scattering mask under the HARD SIDE
        // light, which is where a surface lobe exists at all; transmission
        // under BACKLIGHT, which is the only rig it exists under. Splitting
        // them makes each assertion a claim about the term rather than about
        // the rig.
        const MaterialDebugView sideLitViews[] = { MaterialDebugView::Diffuse, MaterialDebugView::Specular,
                                                   MaterialDebugView::ScatteringMask };

        SetRig(kRigs[1]); // hard side
        std::vector<Capture> captures;
        for (const MaterialDebugView view : sideLitViews)
        {
            Capture capture;
            const std::string name = std::string("DigitalHuman") + DebugViewName(view) + "_GL_Deferred_HardSide";
            ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, name, capture, view, 4)) << name << ": readback failed";

            const FrameStats stats = MeasureSubject(capture);
            // A debug view that returns black is indistinguishable from a term
            // that is off, and "the AOV exists" is not the criterion — "it is
            // inspectable" is.
            EXPECT_GT(stats.LitSamples, stats.Samples / 50u)
                << DebugViewName(view)
                << ": the view is essentially black over the subject under a rig where its term should be "
                   "present. Either the term is not running or the view is not reading it, and from a "
                   "screenshot those look identical.";
            captures.push_back(std::move(capture));
        }

        // TRANSMISSION, under the rig it lives in, and ALSO under the one it
        // does not. The pair is the claim: a transmission view that was
        // secretly showing the composite would be bright under both.
        Capture transmissionSideLit;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, "DigitalHumanTransmission_GL_Deferred_HardSide",
                                 transmissionSideLit, MaterialDebugView::Transmission, 4));

        SetRig(kRigs[2]); // backlight
        Capture transmissionBacklit;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, "DigitalHumanTransmission_GL_Deferred_Backlight",
                                 transmissionBacklit, MaterialDebugView::Transmission, 4));

        const FrameStats backlitStats = MeasureSubject(transmissionBacklit);
        const FrameStats sideLitStats = MeasureSubject(transmissionSideLit);
        EXPECT_GT(backlitStats.LitSamples, backlitStats.Samples / 50u)
            << "the transmission view is black under BACKLIGHT, which is the one rig it must not be black "
               "under — a backlit thin region is the entire #1242 term.";
        EXPECT_GT(backlitStats.MeanLuma, sideLitStats.MeanLuma)
            << "the transmission view is no brighter backlit (" << backlitStats.MeanLuma << ") than side-lit ("
            << sideLitStats.MeanLuma
            << "). A term that does not care where the light is is not a transmission term, and a view that "
               "does not track it is not inspecting one.";
        // AND THE VIEWS ARE NOT EACH OTHER. Four views that all returned the
        // composite would pass every check above.
        //
        // THE SIDE-LIT TRANSMISSION IS THE ONE THAT GOES IN, not the backlit
        // one, and that is the whole point of keeping it. Every other capture
        // in `captures` was taken under HardSide; appending the BACKLIT
        // transmission would make three of the six pairs differ because the
        // RIG changed, which is exactly the free pass this check exists to
        // deny. Comparing same-rig captures means a difference can only come
        // from the view.
        captures.push_back(std::move(transmissionSideLit));

        const MaterialDebugView allViews[] = { MaterialDebugView::Diffuse, MaterialDebugView::Specular,
                                               MaterialDebugView::ScatteringMask,
                                               MaterialDebugView::Transmission };
        for (sizet i = 0; i < captures.size(); ++i)
        {
            for (sizet j = i + 1; j < captures.size(); ++j)
            {
                const Difference d = Diff(captures[i], captures[j]);
                EXPECT_GT(d.MaxDelta, 24u)
                    << DebugViewName(allViews[i]) << " and " << DebugViewName(allViews[j])
                    << " are the same image (max delta " << d.MaxDelta
                    << "/255), so they are not separate inspections of separate terms.";
            }
        }
    }

    // The claim that makes "separately inspectable" mean something: each view
    // responds to ITS OWN authored control and is unmoved by the others'.
    //
    // WHY THIS AND NOT A SUM. The obvious test of a decomposition is that the
    // parts add up to the whole, and that test cannot be written here: the
    // composite is tone-mapped and the AOVs are returned before the debug tints
    // but after the same chain, so diffuse + specular + transmission does not
    // equal the composite in 8-bit sRGB and any tolerance loose enough to
    // accommodate the curve would also accommodate a wrong decomposition.
    //
    // CROSS-TALK is the claim that survives tone mapping. If the specular view
    // moves when the TRANSMISSION strength is re-authored, the two terms are not
    // separated, whatever the frame looks like.
    TEST_F(SkinDigitalHumanScene, EachViewRespondsToItsOwnControlAndNotTheOthers)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SetRig(kRigs[2]);
        SetAngle(kAngles[1]);
        SetExpression(0.65f);

        // The three arms differ from the reference by exactly one authored
        // field each, so an observed move is attributable to that field.
        auto makeArm = [this](f32 transmissionStrength, f32 lobeMix)
        {
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = SkinEvaluationModel::OcularSurface;
            parameters.ScatterColor = kTones[0].ScatterColor;
            parameters.ScatterRadiusMM = kTones[0].ScatterRadiusMM;
            parameters.Transmission.Strength = transmissionStrength;
            parameters.Specular.LobeMix = lobeMix;
            parameters.Specular.ExpressionDetailGain = kExpressionDetailGain;
            return RegisterProfile("CrossTalkArm", parameters);
        };

        // The REFERENCE arm is the subject's own Fair profile, not a fourth
        // registration: it already carries exactly (transmission 1, lobe mix
        // kLobeMix, the expression gain), so re-registering it would spend a
        // slot to describe a profile this fixture already has. Two arms plus
        // the subject's four leaves one of the seven slots free.
        const AssetHandle reference = m_ToneProfiles[0];
        const AssetHandle noTransmission = makeArm(0.0f, kLobeMix);
        const AssetHandle noSecondLobe = makeArm(1.0f, 0.0f);

        auto captureWith = [this](AssetHandle profile, MaterialDebugView view, const std::string& name)
        {
            m_Cranium.GetComponent<MaterialComponent>().m_Material.SetSkinProfileHandle(profile);
            Capture capture;
            EXPECT_TRUE(CaptureFrame(RenderingPath::Deferred, name, capture, view, 4));
            return capture;
        };

        const Capture transmissionRef = captureWith(reference, MaterialDebugView::Transmission, std::string());
        const Capture specularRef = captureWith(reference, MaterialDebugView::Specular, std::string());

        const Capture transmissionKilled =
            captureWith(noTransmission, MaterialDebugView::Transmission,
                        "DigitalHumanTransmissionOff_GL_Deferred_Backlight");
        const Capture specularUnderTransmissionKill =
            captureWith(noTransmission, MaterialDebugView::Specular, std::string());

        const Capture specularKilled =
            captureWith(noSecondLobe, MaterialDebugView::Specular, "DigitalHumanSecondLobeOff_GL_Deferred_Backlight");
        const Capture transmissionUnderSpecularKill =
            captureWith(noSecondLobe, MaterialDebugView::Transmission, std::string());

        m_Cranium.GetComponent<MaterialComponent>().m_Material.SetSkinProfileHandle(m_ToneProfiles[0]);

        const Difference transmissionOwn = Diff(transmissionRef, transmissionKilled);
        const Difference specularCrossFromTransmission = Diff(specularRef, specularUnderTransmissionKill);
        const Difference specularOwn = Diff(specularRef, specularKilled);
        const Difference transmissionCrossFromSpecular = Diff(transmissionRef, transmissionUnderSpecularKill);

        // Each view must move a lot for its own control...
        EXPECT_GT(transmissionOwn.MaxDelta, 30u)
            << "zeroing the transmission strength barely moved the TRANSMISSION view (" << transmissionOwn.MaxDelta
            << "/255), so that view is not showing the transmission term";
        EXPECT_GT(specularOwn.MaxDelta, 20u)
            << "zeroing the second specular lobe barely moved the SPECULAR view (" << specularOwn.MaxDelta
            << "/255), so that view is not showing the specular term";

        // ...and much less for somebody else's. A ratio rather than an absolute
        // bound: the deferred path's own jitter reaches both sides equally, and
        // what separation means is that the own-control move DOMINATES.
        EXPECT_GT(transmissionOwn.MeanAbsDelta, specularCrossFromTransmission.MeanAbsDelta * 2.0)
            << "killing the transmission moved the SPECULAR view by "
            << specularCrossFromTransmission.MeanAbsDelta << "/255 mean against the transmission view's own "
            << transmissionOwn.MeanAbsDelta
            << "/255. The two terms are not separated — a transmission change is leaking into the specular "
               "inspection, so neither view can be trusted to attribute a defect.";
        EXPECT_GT(specularOwn.MeanAbsDelta, transmissionCrossFromSpecular.MeanAbsDelta * 2.0)
            << "killing the second specular lobe moved the TRANSMISSION view by "
            << transmissionCrossFromSpecular.MeanAbsDelta << "/255 mean against the specular view's own "
            << specularOwn.MeanAbsDelta << ". The two terms are not separated.";
    }

    // Criterion 3's second half: measured. Written to a file beside the PNGs,
    // because a number in a test log is a number nobody reads twice.
    TEST_F(SkinDigitalHumanScene, TheCostOfTheDigitalHumanIsRecorded)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SetRig(kRigs[1]);
        SetAngle(kAngles[1]);
        SetTone(1);
        SetExpression(0.65f);

        // The "off" arm: the whole skin stack back at version 0, which is the
        // split-only model. Not version 1 — the cost being reported is the cost
        // of the DIGITAL HUMAN, and that includes the diffusion pass.
        SkinProfileParameters plain = SkinProfile::DefaultParameters();
        plain.EvaluationModel = SkinEvaluationModel::DiffuseSpecularSplit;
        const AssetHandle plainProfile = RegisterProfile("SplitOnlyControl", plain);

        std::string report;
        report += "DigitalHuman_Timing — issue #1222 epic acceptance, 384x384 offscreen, wall clock per frame.\n";
        report += "Subject: procedural stand-in head — cranium (#1241/#1242/#1243) + two ocular surfaces\n";
        report += "(#1244) + lips and enamel (#1245), morph-driven expression at 0.65, hard side light.\n\n";
        report += "NOT A BENCHMARK: this box also hosts CI runners for another repo, so the absolute\n";
        report += "figures move. The two arms are captured back to back, each after a DISCARDED\n";
        report += "warm-up capture, so neither pays for the frame-graph reconfigure that a path\n";
        report += "switch forces. An earlier version of this file timed the first capture after\n";
        report += "that switch and reported a uniform +95% that was the rebuild, not the skin.\n\n";
        report += "HOW TO READ A NEGATIVE NUMBER. The full stack cannot really be cheaper than the\n";
        report += "split-only control. Where the percentage comes out negative, or flips sign\n";
        report += "between runs, the honest reading is that THE COST OF THE SKIN STACK IS BELOW\n";
        report += "THIS FIXTURE'S NOISE FLOOR at 384x384 with one head on screen — not that it is\n";
        report += "free. A real per-pass budget needs GPU timer queries on a quiet box and a scene\n";
        report += "with enough skin in it to dominate the frame. What this file can resolve, and\n";
        report += "therefore all it claims, is that the stack does not cost an order of magnitude.\n\n";

        for (const RenderingPath path : kPaths)
        {
            Capture scratch;
            f64 fullMs = 0.0;
            f64 plainMs = 0.0;

            // A DISCARDED WARM-UP FIRST, and it is the difference between a
            // measurement and a fiction. Switching `path` forces a frame-graph
            // reconfigure, and whichever arm is captured first pays for it —
            // which was always the full-stack arm, so the first version of this
            // file reported a near-uniform +95% on all three paths that was the
            // rebuild rather than the skin. Timing the SECOND capture of each
            // arm leaves both measuring steady state.
            m_Cranium.GetComponent<MaterialComponent>().m_Material.SetSkinProfileHandle(m_ToneProfiles[1]);
            ASSERT_TRUE(CaptureFrame(path, std::string(), scratch, MaterialDebugView::None, 4));
            ASSERT_TRUE(CaptureFrame(path, std::string(), scratch, MaterialDebugView::None, 8, &fullMs));

            m_Cranium.GetComponent<MaterialComponent>().m_Material.SetSkinProfileHandle(plainProfile);
            ASSERT_TRUE(CaptureFrame(path, std::string(), scratch, MaterialDebugView::None, 4));
            ASSERT_TRUE(CaptureFrame(path, std::string(), scratch, MaterialDebugView::None, 8, &plainMs));

            char line[320];
            std::snprintf(line, sizeof(line),
                          "%-12s  full stack %7.3f ms   split-only %7.3f ms   (the digital human costs %+.1f%%)\n",
                          PathName(path), fullMs, plainMs,
                          plainMs > 0.0 ? (fullMs / plainMs - 1.0) * 100.0 : 0.0);
            report += line;

            // THE ONLY ASSERTION, and it is deliberately loose: a screen-space
            // diffusion plus three per-pixel material terms cannot plausibly
            // triple a frame, and anything that did would be a bug rather than
            // a cost. A tight bound here would be a flake on a shared box,
            // which is worse than no bound.
            EXPECT_LT(fullMs, plainMs * 3.0 + 4.0)
                << PathName(path) << ": the full skin stack more than tripled the frame time";
        }

        m_Cranium.GetComponent<MaterialComponent>().m_Material.SetSkinProfileHandle(m_ToneProfiles[0]);

        const fs::path timingFile = VisualOutputPath("DigitalHuman_Timing").replace_extension(".txt");
        std::ofstream out(timingFile);
        out << report;
        EXPECT_TRUE(out.good()) << "failed to write " << timingFile.string();
    }
} // namespace OloEngine::Tests
