// OLO_TEST_LAYER: L8
// =============================================================================
// SkinProfileParityEvidenceTest.cpp — a skin material's profile must mean the
// SAME THING on Forward, Forward+ and Deferred (issue #1231).
//
// WHY THIS TEST SHAPE. The profile's journey differs completely per path, and
// each hop is its own silent-failure opportunity:
//
//   Forward / Forward+  material UBO -> u_MaterialKind + u_SkinSpecularTint*
//                       -> oloApplySkinProfile in PBR_MultiLight.glsl
//   Deferred            SkinProfileTable slot -> G-Buffer RT2 alpha bits 1..5
//                       -> ComputeDeferredLit decode -> u_SkinProfileParams[slot]
//                       -> oloApplySkinProfile in DeferredLightingShared.glsl
//
// That is exactly the shape of the #975 regression this file's sibling
// (ClosureV2DeferredParityEvidenceTest) pins: a selector that was live on
// Forward and dead on Deferred, with the whole headless suite green because
// nothing compared the two paths.
//
// WHAT THE SCENE MEASURES, AND WHY IT IS THE RIGHT DISCRIMINATOR. Two identical
// low-roughness dielectric spheres under one directional key light. They differ
// in one thing: the right one is MaterialKind::Skin naming a profile whose
// SpecularTint is a strong red (1.0, 0.12, 0.12); the left one is Generic.
//
// A specular tint is the only profile parameter that does anything visible
// before #1241's diffusion exists — and it is the ideal probe for the SPLIT
// itself, because it must reach ONE half and not the other:
//
//   * the tinted sphere's specular HIGHLIGHT loses its blue and green;
//   * its DIFFUSE body does not, because the tint is applied to
//     OloSurfaceLighting::Specular alone.
//
// So the test asserts both halves of that claim. A "split" that simply
// multiplied the composite would fail the second assertion; a tint that never
// reached a path would fail the first on that path. Neither failure mode is
// visible in a frame you only look at.
//
// Evidence PNGs (written before any assertion):
//   OloEditor/assets/tests/visual/SkinProfileParity_Forward.png
//   OloEditor/assets/tests/visual/SkinProfileParity_ForwardPlus.png
//   OloEditor/assets/tests/visual/SkinProfileParity_Deferred.png
//
// Classification: L8 (full Scene pipeline on all three raster paths, RGBA8
// readback + PNG; SKIPs cleanly without a GL 4.6 context).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
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

        // The authored tint. Deliberately extreme: this is a contract test, not
        // a beauty shot, and a subtle tint would need a tolerance wide enough to
        // also pass with the tint missing.
        constexpr glm::vec3 kSkinSpecularTint{ 1.0f, 0.12f, 0.12f };

        [[nodiscard]] f32 Channel(const std::vector<u8>& px, std::size_t idx, u32 channel)
        {
            return static_cast<f32>(px[idx + channel]) / 255.0f;
        }

        [[nodiscard]] f32 LuminanceAt(const std::vector<u8>& px, std::size_t idx)
        {
            return 0.2126f * Channel(px, idx, 0) + 0.7152f * Channel(px, idx, 1) + 0.0722f * Channel(px, idx, 2);
        }

        [[nodiscard]] f32 PeakLumaInHalf(const std::vector<u8>& px, u32 w, u32 h, bool rightHalf)
        {
            const u32 x0 = rightHalf ? w / 2 : 8u;
            const u32 x1 = rightHalf ? w - 8u : w / 2;
            f32 peak = 0.0f;
            for (u32 y = 8; y < h - 8; ++y)
                for (u32 x = x0; x < x1; ++x)
                    peak = std::max(peak, LuminanceAt(px, (static_cast<std::size_t>(y) * w + x) * 4));
            return peak;
        }

        // Mean (G + B) / (R + G + B) over the pixels of one half whose luminance
        // is at or above `floorLuma`: the specular-dominated band.
        //
        // A MEAN OVER A BAND, NOT THE SINGLE BRIGHTEST PIXEL. The first version
        // of this test read the peak texel and could not tell the two spheres
        // apart (0.644 vs 0.664 with the tint fully live): a low-roughness
        // highlight blows past 1.0 in every channel, so BOTH spheres clip to
        // white at their brightest pixel and the tint is invisible exactly where
        // it is strongest. The band is chosen by an ABSOLUTE luminance the
        // caller derives from the control half, so both halves are measured over
        // the same brightness range even though the tinted one is darker —
        // deriving each half's band from its own peak would compare two
        // different regions and hide the effect a second way.
        //
        // The ratio is scale-free: it does not move when a path is a little
        // brighter overall, only when the colour balance changes, which is what
        // lets one threshold serve all three paths without pinning exposure.
        [[nodiscard]] f32 MeanChromaAboveLuma(const std::vector<u8>& px, u32 w, u32 h, bool rightHalf, f32 floorLuma)
        {
            const u32 x0 = rightHalf ? w / 2 : 8u;
            const u32 x1 = rightHalf ? w - 8u : w / 2;
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = 8; y < h - 8; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * w + x) * 4;
                    if (LuminanceAt(px, idx) < floorLuma)
                        continue;
                    const f32 r = Channel(px, idx, 0);
                    const f32 g = Channel(px, idx, 1);
                    const f32 b = Channel(px, idx, 2);
                    sum += static_cast<f64>((g + b) / std::max(r + g + b, 1.0e-4f));
                    ++count;
                }
            }
            return count == 0 ? 0.0f : static_cast<f32>(sum / static_cast<f64>(count));
        }

        // Mean luminance over one half's pixels inside an absolute luminance
        // window. Used for the DIFFUSE body: the window's ceiling keeps the
        // highlight out and its floor keeps the backdrop out, and BOTH halves
        // are measured over the same window for the reason above.
        [[nodiscard]] f32 MeanLumaInBand(const std::vector<u8>& px, u32 w, u32 h, bool rightHalf, f32 floorLuma, f32 ceilLuma)
        {
            const u32 x0 = rightHalf ? w / 2 : 8u;
            const u32 x1 = rightHalf ? w - 8u : w / 2;
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = 8; y < h - 8; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const f32 luma = LuminanceAt(px, (static_cast<std::size_t>(y) * w + x) * 4);
                    if (luma > floorLuma && luma < ceilLuma)
                    {
                        sum += static_cast<f64>(luma);
                        ++count;
                    }
                }
            }
            return count == 0 ? 0.0f : static_cast<f32>(sum / static_cast<f64>(count));
        }

        [[nodiscard]] fs::path VisualOutputPath(const char* prefix, const char* pathName)
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / (std::string(prefix) + pathName + ".png");
        }

        // The two spheres' screen positions, as fractions of the frame.
        //
        // A BOX ON THE SUBJECT, NOT A HALF-FRAME MEAN. The backdrop is a large
        // part of every half and it is IDENTICAL on both sides, so a half-frame
        // mean of a per-surface output is mostly backdrop: the scattering-mask
        // view, whose spheres are plainly black and white, came out 0.21 vs 0.48
        // rather than 0 vs 1. Comparing one sphere against the other needs the
        // measurement to be on the spheres — the same discipline
        // ab-diff-peak-is-not-a-location.md states for A/B differences.
        //
        // Derived from the scene, not fitted to a capture: the camera sits at
        // z = 4.5 looking down -Z, the spheres are unit radius at x = +-1.4, so
        // each projects to about a fifth of the frame width, centred just under
        // half a frame from its edge. The box is a quarter of that radius, which
        // leaves it inside the sphere with room to spare.
        constexpr f32 kGenericSphereCx = 0.19f;
        constexpr f32 kSkinSphereCx = 0.81f;
        constexpr f32 kSphereCy = 0.50f;
        constexpr f32 kSphereBoxHalfExtent = 0.045f;

        template<typename FMeasure>
        [[nodiscard]] f32 MeanInSphereBox(const std::vector<u8>& px, u32 w, u32 h, bool skinSphere, FMeasure measure)
        {
            const f32 cx = skinSphere ? kSkinSphereCx : kGenericSphereCx;
            const auto span = [](f32 centre, f32 extent, u32 size)
            {
                const i32 lo = static_cast<i32>((centre - extent) * static_cast<f32>(size));
                const i32 hi = static_cast<i32>((centre + extent) * static_cast<f32>(size));
                return std::pair<u32, u32>{ static_cast<u32>(std::max(lo, 0)),
                                            static_cast<u32>(std::min(hi, static_cast<i32>(size) - 1)) };
            };
            const auto [x0, x1] = span(cx, kSphereBoxHalfExtent, w);
            const auto [y0, y1] = span(kSphereCy, kSphereBoxHalfExtent, h);

            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = y0; y <= y1; ++y)
            {
                for (u32 x = x0; x <= x1; ++x)
                {
                    sum += static_cast<f64>(measure(px, (static_cast<std::size_t>(y) * w + x) * 4));
                    ++count;
                }
            }
            return count == 0 ? 0.0f : static_cast<f32>(sum / static_cast<f64>(count));
        }

        [[nodiscard]] f32 LumaAt(const std::vector<u8>& px, std::size_t idx)
        {
            return LuminanceAt(px, idx);
        }

        // "How far from grey is this pixel?" — max(channel) - min(channel).
        [[nodiscard]] f32 SaturationAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = Channel(px, idx, 0);
            const f32 g = Channel(px, idx, 1);
            const f32 b = Channel(px, idx, 2);
            return std::max({ r, g, b }) - std::min({ r, g, b });
        }

        [[nodiscard]] f32 MeanLumaInHalf(const std::vector<u8>& px, u32 w, u32 h, bool rightHalf)
        {
            const u32 x0 = rightHalf ? w / 2 : 8u;
            const u32 x1 = rightHalf ? w - 8u : w / 2;
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = 8; y < h - 8; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    sum += static_cast<f64>(LuminanceAt(px, (static_cast<std::size_t>(y) * w + x) * 4));
                    ++count;
                }
            }
            return count == 0 ? 0.0f : static_cast<f32>(sum / static_cast<f64>(count));
        }
    } // namespace

    class SkinProfileParityScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            // A throwaway project so AssetManager::AddMemoryOnlyAsset has an
            // active manager to register the profile with. The renderer resolves
            // profiles through the asset manager exactly as it does in the
            // editor, so going through it here is what makes this a test of the
            // real journey rather than of an injected shortcut.
            SetUpScratchProject();

            auto profile = Ref<SkinProfile>::Create();
            profile->SetName("ParityProbe");
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.SpecularTint = kSkinSpecularTint;
            ASSERT_TRUE(profile->SetParameters(parameters))
                << "the probe profile needed correcting — the authored tint is out of range";
            m_ProfileHandle = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
            ASSERT_NE(static_cast<u64>(m_ProfileHandle), 0ULL) << "AddMemoryOnlyAsset returned a zero handle";

            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 4.5f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity sun = GetScene().CreateEntity("Sun");
            auto& dirLight = sun.AddComponent<DirectionalLightComponent>();
            dirLight.m_Direction = { -0.3f, -0.5f, -0.8f };
            dirLight.m_Color = { 1.0f, 1.0f, 1.0f };
            // Dim enough that the specular band stays off the clipping ceiling —
            // see ApplyProbeMaterial for why that matters here.
            dirLight.m_Intensity = 1.5f;
            dirLight.m_CastShadows = false;

            const Ref<Mesh> sphere = MeshPrimitives::CreateSphere(1.0f, 32);

            // LEFT: Generic. The control.
            {
                Entity entity = GetScene().CreateEntity("GenericSphere");
                entity.AddComponent<MeshComponent>(sphere->GetMeshSource());
                entity.GetComponent<TransformComponent>().Translation = { -1.4f, 0.0f, 0.0f };
                auto& materialComp = entity.AddComponent<MaterialComponent>();
                ApplyProbeMaterial(materialComp.m_Material);
            }

            // RIGHT: the identical material, declared Skin and naming the
            // profile. Nothing else differs — same albedo, same roughness, same
            // closure version.
            {
                Entity entity = GetScene().CreateEntity("SkinSphere");
                entity.AddComponent<MeshComponent>(sphere->GetMeshSource());
                entity.GetComponent<TransformComponent>().Translation = { 1.4f, 0.0f, 0.0f };
                auto& materialComp = entity.AddComponent<MaterialComponent>();
                ApplyProbeMaterial(materialComp.m_Material);
                materialComp.m_Material.SetMaterialKind(MaterialKind::Skin);
                materialComp.m_Material.SetSkinProfileHandle(m_ProfileHandle);
            }

            EnableRendering(kSize, kSize);
        }

        void TearDown() override
        {
            // BEFORE anything that can fail, and in TearDown rather than at the
            // end of the test body: an ASSERT_* returns from the test function,
            // so a reset written after it never runs and leaves this
            // renderer-wide switch ARMED — every later fixture in the process
            // then renders a debug view instead of its own frame
            // (cross-test-renderer-state.md).
            Renderer3D::GetPostProcessSettings().MaterialDebug = MaterialDebugView::None;

            // The slot table is sticky by design (SkinProfileTable.h), so a
            // memory-only handle from this test must not outlive it and hold a
            // slot the next test's profile could have used.
            Renderer3D::GetSkinProfileTable().Reset();
            // Unload(), not just a Reset(): the project is a process-wide static
            // that owns the asset manager, and leaving it installed keeps the
            // manager alive until static destruction -- past the point where the
            // scratch directory below still exists. See the matching note in
            // SkinProfileTest.cpp and on Project::Unload itself.
            m_AssetManager.Reset();
            Project::Unload();
            std::error_code ec;
            if (!m_ProjectDir.empty())
                fs::remove_all(m_ProjectDir, ec);
            RendererAttachedTest::TearDown();
        }

        static void ApplyProbeMaterial(Material& material)
        {
            // A MID-TONE, MODERATELY ROUGH dielectric, tuned so neither half of
            // the frame clips. Both numbers are load-bearing:
            //
            //   roughness 0.32 spreads the specular lobe over enough pixels to
            //   average, and keeps its peak inside the tonemapper's linear-ish
            //   range — at 0.18 the lobe blew past 1.0 in every channel and both
            //   spheres clipped to white, hiding the tint completely;
            //   albedo 0.30 leaves the diffuse body well below the specular band
            //   so the two are separable, while still being bright enough to
            //   measure against the backdrop.
            material.SetBaseColorFactor(glm::vec4(0.30f, 0.29f, 0.28f, 1.0f));
            material.SetMetallicFactor(0.0f);
            material.SetRoughnessFactor(0.32f);
        }

        void SetUpScratchProject()
        {
            std::error_code ec;
            // TempDir() rather than std::filesystem::temp_directory_path():
            // it hands back a per-process root inside the repo's own scratch
            // area, which is what keeps concurrent test processes from sharing
            // a project directory (and keeps %TEMP% on another drive from
            // breaking path-relative asset resolution).
            m_ProjectDir = OloEngine::Tests::TempDir("skin-parity-project");
            fs::create_directories(m_ProjectDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create the scratch project dir at " << m_ProjectDir.string();

            const fs::path projectFile = m_ProjectDir / "SkinParity.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: SkinParity\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << projectFile.string();

            m_AssetManager = Ref<EditorAssetManager>::Create();
            // No file watcher: this test stages nothing and never hot-reloads,
            // and a watcher thread outliving the test races the next one.
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        // Renders on one path, writes the evidence PNG, and asserts both halves
        // of the contract.
        // Moves the camera without rebuilding the scene, so a second angle costs
        // two frames rather than a second fixture.
        void SetCameraPose(const glm::vec3& translation, const glm::vec3& rotationRadians)
        {
            Entity camera = GetScene().FindEntityByName("Camera");
            ASSERT_TRUE(static_cast<bool>(camera)) << "the probe scene lost its camera";
            auto& transform = camera.GetComponent<TransformComponent>();
            transform.Translation = translation;
            // SetRotationEuler, not the member: the authoritative rotation is a
            // quaternion and the euler triple is a derived view of it.
            transform.SetRotationEuler(rotationRadians);
        }

        void ExpectProfileReachesSpecularOnlyOnPath(RenderingPath path, const char* pathName)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            RunFrames(2);

            std::vector<u8> px;
            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(px, width, height)) << pathName << ": ReadbackComposite failed";
            ASSERT_EQ(px.size(), static_cast<std::size_t>(width) * height * 4u);

            const fs::path out = VisualOutputPath("SkinProfileParity_", pathName);
            const int wrote = ::stbi_write_png(out.string().c_str(), static_cast<int>(width), static_cast<int>(height),
                                               4, px.data(), static_cast<int>(width) * 4);
            EXPECT_NE(wrote, 0) << "failed to write " << out.string();

            // Both bands are derived from the CONTROL half's peak and then
            // applied to both halves, so the tinted sphere is measured over the
            // same brightness range as the untinted one rather than over its own
            // (lower) one. Deriving per half would compare two different regions
            // of two different spheres and call the result a colour difference.
            const f32 genericPeak = PeakLumaInHalf(px, width, height, /*rightHalf=*/false);
            ASSERT_GT(genericPeak, 0.05f)
                << pathName << ": the control sphere has no highlight at all — the scene did not render; see "
                << out.string();
            // 0.80 of the control's peak, not 0.55: at 0.55 the band still
            // swallowed most of the DIFFUSE body (a 0.30-albedo sphere sits
            // around 0.6 of the highlight's tone-mapped level), so the mean was
            // dominated by pixels the tint does not touch and the two spheres
            // came out 0.01 apart on a frame where the highlights are plainly
            // white and red. 0.80 keeps the band on the highlight core.
            //
            // The diffuse ceiling stays well under it, so the two bands cannot
            // overlap and the second assertion measures a region the first one
            // does not.
            const f32 specularFloor = genericPeak * 0.80f;
            // 0.25, not 0.45. At an OBLIQUE view the specular lobe spreads over
            // far more of the sphere, so a band that reached 0.45 of the peak
            // contained enough tinted specular tail to move the skin sphere's
            // mean by 13% — a measurement artefact reported as a leak across the
            // split. Lowering the ceiling makes the band actually diffuse rather
            // than making the tolerance forgiving; the DIRECT version of this
            // claim, on the isolated Diffuse output with no specular in it at
            // all, is asserted by the sibling test below.
            const f32 diffuseCeiling = genericPeak * 0.25f;

            const f32 genericChroma = MeanChromaAboveLuma(px, width, height, /*rightHalf=*/false, specularFloor);
            const f32 skinChroma = MeanChromaAboveLuma(px, width, height, /*rightHalf=*/true, specularFloor);

            // Non-vacuity: both halves actually contain a lit sphere. A
            // background-only half reports a stable chroma ratio too, so the
            // comparison below would otherwise "pass" on an empty frame.
            const f32 genericDiffuse = MeanLumaInBand(px, width, height, /*rightHalf=*/false, 0.01f, diffuseCeiling);
            const f32 skinDiffuse = MeanLumaInBand(px, width, height, /*rightHalf=*/true, 0.01f, diffuseCeiling);
            EXPECT_GT(genericDiffuse, 0.02f) << pathName << ": left half looks empty; see " << out.string();
            EXPECT_GT(skinDiffuse, 0.02f) << pathName << ": right half looks empty; see " << out.string();
            EXPECT_GT(genericChroma, 0.0f)
                << pathName << ": no pixel in the control half reached the specular band; see " << out.string();

            // (1) The tint REACHED the specular half on this path. A neutral
            // highlight sits around a chroma ratio of 0.67 (equal channels); the
            // red tint drives the skin sphere's well below that. 0.05 of
            // separation is far outside tone-mapping wobble and collapses to ~0
            // if the profile never arrived — the measured gap with the tint live
            // is several times this.
            EXPECT_LT(skinChroma, genericChroma - 0.05f)
                << pathName << ": the skin sphere's highlight chroma (" << skinChroma
                << ") is not measurably redder than the generic sphere's (" << genericChroma
                << ") — the profile's specular tint did not reach this path; evidence: " << out.string();

            // (2) The tint did NOT reach the diffuse half. This is the assertion
            // that makes the change a SPLIT rather than a composite multiply: if
            // oloApplySkinProfile touched OloSurfaceLighting::Diffuse, the skin
            // sphere's body would lose its green and blue too and its mean
            // luminance would drop hard. 12% of relative tolerance covers the
            // small, real difference the specular lobe's dim tail makes inside
            // the sampled band.
            const f32 diffuseRatio = skinDiffuse / std::max(genericDiffuse, 1.0e-4f);
            EXPECT_NEAR(diffuseRatio, 1.0f, 0.12f)
                << pathName << ": the skin sphere's diffuse body (" << skinDiffuse
                << ") differs from the generic sphere's (" << genericDiffuse
                << ") — the specular tint leaked into the DIFFUSE half, which means the two "
                   "outputs are not actually separate; evidence: "
                << out.string();
        }

        AssetHandle m_ProfileHandle{};
        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_ProjectDir;
    };

    TEST_F(SkinProfileParityScene, ProfileReachesTheSpecularHalfOnEveryRasterPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Forward (material-UBO journey) ...
        ExpectProfileReachesSpecularOnlyOnPath(RenderingPath::Forward, "Forward");
        // ... Forward+ (the clustered loop's own split accumulator) ...
        ExpectProfileReachesSpecularOnlyOnPath(RenderingPath::ForwardPlus, "ForwardPlus");
        // ... and Deferred (the G-Buffer flags-lane + profile-table journey),
        // last so a Deferred-only failure reads last.
        ExpectProfileReachesSpecularOnlyOnPath(RenderingPath::Deferred, "Deferred");

        // A SECOND CAMERA ANGLE, raised and pulled back so both spheres are seen
        // obliquely rather than head-on. The tint is a per-material multiply
        // with no view-dependent term, so this is a control as much as a
        // capture: a result that held head-on and collapsed here would mean the
        // profile is reaching the shader through something view-dependent — a
        // mis-decoded G-Buffer lane read at the wrong texel, say — rather than
        // through the material.
        SetCameraPose(glm::vec3(0.0f, 1.6f, 5.2f), glm::vec3(-0.28f, 0.0f, 0.0f));
        ExpectProfileReachesSpecularOnlyOnPath(RenderingPath::Deferred, "Deferred_Oblique");
        ExpectProfileReachesSpecularOnlyOnPath(RenderingPath::Forward, "Forward_Oblique");
    }

    // =========================================================================
    // The four outputs a skin surface EXPOSES (issue #1231, acceptance
    // criterion 2).
    //
    // Captures each of the four material debug views on the deferred path and
    // checks the one property that distinguishes it from "the view did nothing":
    // each output must differ from the composite in the way its own definition
    // predicts, and the SKIN half must differ from the GENERIC half wherever the
    // output is skin-specific. A view wired to the wrong lane, or one whose
    // uniform never reached the shader, returns the composite and fails.
    //
    // Evidence PNGs, all four written before any assertion:
    //   OloEditor/assets/tests/visual/SkinOutputs_<view>.png
    // =========================================================================
    TEST_F(SkinProfileParityScene, TheFourSeparatedOutputsAreExposedOnTheDeferredPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        struct Capture
        {
            MaterialDebugView View;
            const char* Name;
            std::vector<u8> Pixels;
            u32 Width = 0;
            u32 Height = 0;
        };

        std::array<Capture, 5> captures = { {
            { MaterialDebugView::None, "Composite", {}, 0, 0 },
            { MaterialDebugView::Diffuse, "Diffuse", {}, 0, 0 },
            { MaterialDebugView::Specular, "Specular", {}, 0, 0 },
            { MaterialDebugView::ProfileIdentity, "ProfileIdentity", {}, 0, 0 },
            { MaterialDebugView::ScatteringMask, "ScatteringMask", {}, 0, 0 },
        } };

        for (Capture& capture : captures)
        {
            Renderer3D::GetPostProcessSettings().MaterialDebug = capture.View;
            RunFrames(2);
            ASSERT_TRUE(ReadbackComposite(capture.Pixels, capture.Width, capture.Height))
                << capture.Name << ": ReadbackComposite failed";

            const fs::path out = VisualOutputPath("SkinOutputs_", capture.Name);
            const int wrote = ::stbi_write_png(out.string().c_str(), static_cast<int>(capture.Width),
                                               static_cast<int>(capture.Height), 4, capture.Pixels.data(),
                                               static_cast<int>(capture.Width) * 4);
            EXPECT_NE(wrote, 0) << "failed to write " << out.string();
        }
        const Capture& composite = captures[0];
        const Capture& diffuse = captures[1];
        const Capture& specular = captures[2];
        const Capture& identity = captures[3];
        const Capture& mask = captures[4];

        // Diffuse and specular are each DIMMER than the composite they sum to —
        // the weakest claim that still cannot be satisfied by returning the
        // composite, which is the failure mode "the uniform never arrived" has.
        const f32 compositeLuma = MeanLumaInHalf(composite.Pixels, composite.Width, composite.Height, true);
        const f32 diffuseLuma = MeanLumaInHalf(diffuse.Pixels, diffuse.Width, diffuse.Height, true);
        const f32 specularLuma = MeanLumaInHalf(specular.Pixels, specular.Width, specular.Height, true);
        EXPECT_GT(compositeLuma, 0.02f) << "the composite frame is empty — nothing rendered";
        EXPECT_LT(diffuseLuma, compositeLuma)
            << "the diffuse view is not dimmer than the composite it is half of — it is returning the composite";
        EXPECT_LT(specularLuma, compositeLuma)
            << "the specular view is not dimmer than the composite it is half of — it is returning the composite";

        // Everything from here compares one SPHERE against the other, so every
        // measurement is a box on the subject rather than a half-frame mean —
        // see MeanInSphereBox for the reading that forced that.

        // The specular output is where the tint lives, so it is the one that is
        // NOT grey on the skin sphere. The diffuse output must stay grey — the
        // same split claim as the sibling test, restated on the ISOLATED
        // outputs, where nothing else can be responsible for the colour.
        const f32 specularSkinSat = MeanInSphereBox(specular.Pixels, specular.Width, specular.Height, true, SaturationAt);
        const f32 specularGenericSat = MeanInSphereBox(specular.Pixels, specular.Width, specular.Height, false, SaturationAt);
        const f32 diffuseSkinSat = MeanInSphereBox(diffuse.Pixels, diffuse.Width, diffuse.Height, true, SaturationAt);
        // Two one-sided claims rather than a margin. The box sits at the sphere
        // CENTRE and the highlight does not, so the specular output is dim there
        // and its absolute saturation is small (~0.02) even with the tint fully
        // live — a margin wide enough to look convincing would simply not fit.
        // What does not depend on where the box lands is that the control is
        // EXACTLY grey and the skin sphere is not, which is the claim.
        EXPECT_LT(specularGenericSat, 0.005f)
            << "the generic sphere's SPECULAR output is tinted (" << specularGenericSat
            << ") — something is applying a profile to a material that names none";
        EXPECT_GT(specularSkinSat, 0.010f)
            << "the skin sphere's SPECULAR output is grey (" << specularSkinSat
            << ") — the profile's tint did not reach the specular half";
        EXPECT_LT(diffuseSkinSat, specularSkinSat)
            << "the skin sphere's DIFFUSE output (" << diffuseSkinSat << ") is as tinted as its specular one ("
            << specularSkinSat << ") — the tint leaked across the split";

        // Profile identity: a hue on the skin sphere, BLACK on the generic one
        // (which names no profile). Saturation rather than luminance because the
        // hue is the signal and black is exactly zero saturation.
        const f32 identitySkin = MeanInSphereBox(identity.Pixels, identity.Width, identity.Height, true, SaturationAt);
        const f32 identityGeneric = MeanInSphereBox(identity.Pixels, identity.Width, identity.Height, false, SaturationAt);
        EXPECT_GT(identitySkin, 0.10f)
            << "the skin sphere shows no profile identity (" << identitySkin
            << ") — the G-Buffer slot did not survive to the lighting pass";
        EXPECT_LT(identityGeneric, 0.02f)
            << "the generic sphere, which names no profile, is showing one (" << identityGeneric
            << ") — the slot field is being read for a surface whose lane never carried it";

        // Scattering mask: greyscale, ~1 on the skin sphere (metallic 0, so
        // 1 - metallic is 1) and exactly 0 on the generic one. The generic
        // sphere is the anti-vacuous control: a mask that is simply "on
        // everywhere" fails here, and one that is off everywhere fails above.
        const f32 maskSkin = MeanInSphereBox(mask.Pixels, mask.Width, mask.Height, true, LumaAt);
        const f32 maskGeneric = MeanInSphereBox(mask.Pixels, mask.Width, mask.Height, false, LumaAt);
        EXPECT_GT(maskSkin, 0.5f) << "the skin sphere's scattering mask (" << maskSkin << ") is not saturated";
        EXPECT_LT(maskGeneric, 0.05f)
            << "the generic sphere scatters (" << maskGeneric << ") — the mask is not gated on the material kind";
    }
} // namespace OloEngine::Tests
