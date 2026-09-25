// =============================================================================
// CookedCutoutCoverageEvidenceTest.cpp — issue #1453, on a live GL context.
//
// Writes, per <Path> in {Forward, ForwardPlus, Deferred}:
// all four at cutoff 0.75, where the plain chain erases the most:
//   OloEditor/assets/tests/visual/CookedCutoutCoverage_GL_<Path>_PineFar.png      the cooked chain
//   OloEditor/assets/tests/visual/CookedCutoutCoverageOff_GL_<Path>_PineFar.png   the plain box chain (control)
//   OloEditor/assets/tests/visual/CookedCutoutCoverageLoose_GL_<Path>_PineFar.png the loose PNG's runtime chain
//   OloEditor/assets/tests/visual/CookedCutoutCoverageOneLevel_GL_<Path>_PineFar.png no mips (the reference)
// and, on Deferred, the same pair for the soft-alpha grass at 0.25:
//   CookedCutoutCoverage[Off]_GL_Deferred_GrassFar.png
// A cell that was not run is a file missing from the diff (task-loop 2a). Vulkan
// is not reachable from a headless fixture; its cells are a live editor session.
//
// ── What is measured ─────────────────────────────────────────────────────────
//
// A wall of alpha-tested cards (a Mask material, the consumer that reaches a
// cooked texture: an imported glTF whose source image is not on disk falls back
// to the asset pack's cooked blob) seen from three distances, so each card spans
// about 155, 39 and 13 pixels — the last well below the 64-texel level a cutout
// chain stops at. Screen coverage is the fraction of pixels the cards change
// against a frame without them. Four arms per distance:
//
//   ONE LEVEL   no mip chain at all: every pixel samples level 0, so the alpha
//               test sees the card's own coverage. The reference.
//   LOOSE       the source PNG with the runtime chain (#1441, histogram-matched
//               since #1453) — what the editor draws.
//   COOKED      TextureCompression's BC7 cook, the asset pack's format.
//   OFF         the same BC7 cook with the plain box chain, i.e. the cook before
//               #1453. The control: it must thin with distance.
//
// at the four cutoffs the repository uses (0.25, 0.3, 0.5, 0.75), for a binary
// card (pine_card) and a soft-alpha one (grass.png). One cooked texture serves
// all four cutoffs — that is the property #1453 decided on.
//
// Runs in the normal suite and SKIPs cleanly (not fails) when there is no GL 4.6
// context.
//
// OLO_TEST_LAYER: L8
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCompression.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/gtc/constants.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#define OLO_TEST_EDITOR_ROOT ""
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;

        constexpr u32 kColumns = 24;
        constexpr u32 kRows = 14;
        constexpr f32 kSpacing = 1.1f;

        // Camera distances from the wall. With a 60 degree vertical field of
        // view over 360 pixels a one-metre card spans about 312 / D pixels.
        constexpr std::array<f32, 3> kDistances = { 2.0f, 8.0f, 24.0f };
        constexpr std::array<const char*, 3> kDistanceNames = { "Near", "Mid", "Far" };
        constexpr std::array<f32, 4> kCutoffs = { 0.25f, 0.3f, 0.5f, 0.75f };

        struct Subject
        {
            const char* Name;
            const char* Path;
        };
        constexpr std::array<Subject, 2> kSubjects = { {
            { "Pine", "SandboxProject/Assets/Models/Vegetation/pine/Textures/pine_card.png" },
            { "Grass", "assets/textures/grass.png" },
        } };

        enum class Arm : u8
        {
            OneLevel,
            Loose,
            Cooked,
            Off,
        };
        constexpr std::array<Arm, 4> kArms = { Arm::OneLevel, Arm::Loose, Arm::Cooked, Arm::Off };
        constexpr std::array<const char*, 4> kArmNames = { "OneLevel", "Loose", "Cooked", "Off" };

        [[nodiscard]] std::string EditorPath(const char* relative)
        {
            return (fs::path(OLO_TEST_EDITOR_ROOT) / relative).string();
        }

        // Fraction of pixels whose colour differs perceptibly between two
        // frames: against a frame without the cards, their screen coverage.
        [[nodiscard]] f64 DifferingFraction(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 1.0;
            sizet differing = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                const int dr = std::abs(static_cast<int>(a[i + 0]) - static_cast<int>(b[i + 0]));
                const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
                const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                if (dr + dg + db > 24)
                    ++differing;
            }
            return static_cast<f64>(differing) / static_cast<f64>(a.size() / 4u);
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
            }
            return "Unknown";
        }

        // The four textures one subject is drawn with. All sRGB (an albedo), so
        // the arms differ in their mip chains and nothing else.
        struct SubjectTextures
        {
            std::array<Ref<Texture2D>, 4> ByArm;
        };

        [[nodiscard]] SubjectTextures LoadSubject(const Subject& subject)
        {
            SubjectTextures out;
            const std::string path = EditorPath(subject.Path);

            // The pixels, flipped as every loader in the engine flips them.
            ::stbi_set_flip_vertically_on_load_thread(1);
            int w = 0;
            int h = 0;
            int channels = 0;
            stbi_uc* pixels = ::stbi_load(path.c_str(), &w, &h, &channels, 4);
            ::stbi_set_flip_vertically_on_load_thread(0);
            if (pixels == nullptr)
                return out;

            TextureSpecification spec;
            spec.Width = static_cast<u32>(w);
            spec.Height = static_cast<u32>(h);
            spec.Format = ImageFormat::RGBA8;
            spec.SRGB = true;
            spec.GenerateMips = false;
            out.ByArm[0] = Texture2D::Create(spec);
            if (out.ByArm[0])
                out.ByArm[0]->SetData(pixels, static_cast<u32>(w * h * 4));

            out.ByArm[1] = Texture2D::Create(path, /*srgb=*/true);

            TextureCompression::CompressOptions options;
            options.UseImportSettings = false;
            options.AutoSRGBFromName = false;
            options.SRGB = true;
            CompressedTextureImage cooked;
            if (TextureCompression::CompressImageFile(path, options, cooked))
                out.ByArm[2] = Texture2D::Create(cooked);

            CompressedTextureImage plain = TextureCompression::EncodeBC7(pixels, static_cast<u32>(w), static_cast<u32>(h), 4,
                                                                         /*srgb=*/true, /*generateMips=*/true,
                                                                         /*preserveAlphaCoverage=*/false);
            plain.HasAlpha = true;
            out.ByArm[3] = Texture2D::Create(plain);

            ::stbi_image_free(pixels);
            return out;
        }
    } // namespace

    class CookedCutoutCoverageEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);
            scene.SetGridVisible(false);
            scene.SetWorldAxisHelperVisible(false);
            scene.SetLightGizmosVisible(false);
            scene.SetCameraFrustumsVisible(false);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                // Along the view axis: every card faces it, so a card's pixels
                // are its albedo times one constant.
                dl.m_Direction = glm::normalize(glm::vec3(0.0f, -0.2f, -1.0f));
                dl.m_Color = glm::vec3(1.0f);
                dl.m_Intensity = 3.0f;
            }

            m_Plane = MeshPrimitives::CreatePlane(1.0f, 1.0f);
            ASSERT_TRUE(m_Plane) << "no plane primitive";

            // A bright magenta backdrop well behind the wall, so a card pixel
            // and a hole in it differ by far more than the 24-level threshold.
            {
                Entity backdrop = scene.CreateEntity("Backdrop");
                auto& tc = backdrop.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 0.0f, -6.0f };
                tc.SetRotationEuler({ glm::half_pi<f32>(), 0.0f, 0.0f });
                tc.Scale = { 400.0f, 1.0f, 400.0f };
                auto& mc = backdrop.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Plane;
                mc.m_MeshSource = m_Plane->GetMeshSource();
                auto& mat = backdrop.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.1f, 0.8f, 1.0f));
                mat.m_Material.SetRoughnessFactor(1.0f);
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetFlag(MaterialFlag::TwoSided, true);
            }

            const f32 x0 = -0.5f * static_cast<f32>(kColumns - 1u) * kSpacing;
            const f32 y0 = -0.5f * static_cast<f32>(kRows - 1u) * kSpacing;
            for (u32 row = 0; row < kRows; ++row)
            {
                for (u32 column = 0; column < kColumns; ++column)
                {
                    Entity card = scene.CreateEntity("Card");
                    auto& tc = card.GetComponent<TransformComponent>();
                    tc.Translation = { x0 + static_cast<f32>(column) * kSpacing, y0 + static_cast<f32>(row) * kSpacing, 0.0f };
                    // The plane lies in XZ facing +Y; stood up it faces the camera.
                    tc.SetRotationEuler({ glm::half_pi<f32>(), 0.0f, 0.0f });
                    auto& mc = card.AddComponent<MeshComponent>();
                    mc.m_Primitive = MeshPrimitive::Plane;
                    mc.m_MeshSource = m_Plane->GetMeshSource();
                    auto& mat = card.AddComponent<MaterialComponent>();
                    mat.m_Material.SetBaseColorFactor(glm::vec4(1.0f));
                    mat.m_Material.SetRoughnessFactor(1.0f);
                    mat.m_Material.SetMetallicFactor(0.0f);
                    mat.m_Material.SetAlphaMode(AlphaMode::Mask);
                    mat.m_Material.SetFlag(MaterialFlag::TwoSided, true);
                    m_Cards.push_back(card);
                }
            }
        }

        void SetPath(RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
        }

        struct ScopedRenderPath
        {
            explicit ScopedRenderPath(CookedCutoutCoverageEvidenceTest& owner)
                : m_Owner(owner), m_Path(Renderer3D::GetRendererSettings().Path)
            {
            }
            ~ScopedRenderPath()
            {
                m_Owner.SetPath(m_Path);
            }
            ScopedRenderPath(const ScopedRenderPath&) = delete;
            auto operator=(const ScopedRenderPath&) -> ScopedRenderPath& = delete;

            CookedCutoutCoverageEvidenceTest& m_Owner;
            RenderingPath m_Path;
        };

        // Every card draws `texture` at `cutoff`; a null texture hides them.
        void ApplyArm(const Ref<Texture2D>& texture, f32 cutoff)
        {
            for (Entity card : m_Cards)
            {
                auto& tc = card.GetComponent<TransformComponent>();
                tc.Scale = texture ? glm::vec3(1.0f) : glm::vec3(0.0f);
                Material& material = card.GetComponent<MaterialComponent>().m_Material;
                if (texture)
                    material.SetAlbedoMap(texture);
                material.SetAlphaCutoff(cutoff);
            }
            // What an importer does for a Mask material (Model.cpp,
            // ImportedMaterialCodec): hand the texture its consumer's cutoff.
            // The cards share one texture, so one call covers them all.
            if (texture && !m_Cards.empty())
                m_Cards.front().GetComponent<MaterialComponent>().m_Material.PrepareAlphaTestMips();
        }

        void Capture(f32 distance, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 0.0f, distance), 0.0f, 0.0f);
            RunEditorFrames(camera, 3);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);
        }

        static void WritePng(const std::string& name, const std::vector<u8>& px)
        {
            std::vector<u8> flipped(px); // GL readback is bottom-up
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* a = flipped.data() + static_cast<sizet>(y) * rowBytes;
                u8* b = flipped.data() + static_cast<sizet>(kHeight - 1u - y) * rowBytes;
                std::vector<u8> tmp(a, a + rowBytes);
                std::memcpy(a, b, rowBytes);
                std::memcpy(b, tmp.data(), rowBytes);
            }
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ::stbi_write_png((dir / name).string().c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                             flipped.data(), static_cast<int>(kWidth) * 4);
        }

        Ref<Mesh> m_Plane;
        std::vector<Entity> m_Cards;
    };

    TEST_F(CookedCutoutCoverageEvidenceTest, ACookedCutoutKeepsItsScreenCoverageWithDistanceOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::array<SubjectTextures, kSubjects.size()> textures;
        for (sizet s = 0; s < kSubjects.size(); ++s)
        {
            textures[s] = LoadSubject(kSubjects[s]);
            for (sizet a = 0; a < kArms.size(); ++a)
            {
                ASSERT_TRUE(textures[s].ByArm[a] && textures[s].ByArm[a]->IsLoaded())
                    << kSubjects[s].Name << " " << kArmNames[a] << " did not load";
            }
            EXPECT_LT(textures[s].ByArm[2]->GetMipLevelCount(), textures[s].ByArm[3]->GetMipLevelCount())
                << kSubjects[s].Name << ": the cooked cutout should stop at the 64-texel level";
        }

        const ScopedRenderPath restorePath(*this);
        constexpr std::array<RenderingPath, 3> kPaths{ RenderingPath::Forward, RenderingPath::ForwardPlus,
                                                       RenderingPath::Deferred };
        for (const RenderingPath path : kPaths)
        {
            SCOPED_TRACE(PathName(path));
            SetPath(path);

            std::array<std::vector<u8>, kDistances.size()> empty;
            ApplyArm(nullptr, 0.5f);
            for (sizet d = 0; d < kDistances.size(); ++d)
                Capture(kDistances[d], empty[d]);

            for (sizet s = 0; s < kSubjects.size(); ++s)
            {
                // Worst relative far-pose deviation from ONE LEVEL, over the four cutoffs.
                f64 worstCooked = 0.0;
                f64 worstOff = 0.0;
                for (sizet c = 0; c < kCutoffs.size(); ++c)
                {
                    const f32 cutoff = kCutoffs[c];
                    // [arm][distance]
                    std::array<std::array<f64, kDistances.size()>, kArms.size()> coverage{};
                    for (sizet a = 0; a < kArms.size(); ++a)
                    {
                        ApplyArm(textures[s].ByArm[a], cutoff);
                        for (sizet d = 0; d < kDistances.size(); ++d)
                        {
                            std::vector<u8> frame;
                            Capture(kDistances[d], frame);
                            ASSERT_FALSE(HasFatalFailure());
                            coverage[a][d] = DifferingFraction(frame, empty[d]);

                            const bool pineHighFar = s == 0 && c == 3u && d + 1u == kDistances.size();
                            const bool grassQuarterFar = s == 1 && c == 0u && d + 1u == kDistances.size() &&
                                                         path == RenderingPath::Deferred &&
                                                         (kArms[a] == Arm::Cooked || kArms[a] == Arm::Off);
                            if (pineHighFar || grassQuarterFar)
                            {
                                const std::string feature = kArms[a] == Arm::Cooked ? "CookedCutoutCoverage"
                                                            : kArms[a] == Arm::Off  ? "CookedCutoutCoverageOff"
                                                                                    : std::string("CookedCutoutCoverage") +
                                                                                         kArmNames[a];
                                WritePng(feature + "_GL_" + PathName(path) + "_" + kSubjects[s].Name + "Far.png", frame);
                            }
                        }
                    }

                    for (sizet d = 0; d < kDistances.size(); ++d)
                    {
                        const f64 reference = coverage[0][d];
                        GTEST_LOG_(INFO) << PathName(path) << " " << kSubjects[s].Name << " cutoff " << cutoff << " "
                                         << kDistanceNames[d] << ": one level " << reference * 100.0 << "%, loose "
                                         << coverage[1][d] * 100.0 << "%, cooked " << coverage[2][d] * 100.0
                                         << "%, off " << coverage[3][d] * 100.0 << "%";
                    }

                    const sizet farIndex = kDistances.size() - 1u;
                    const f64 reference = coverage[0][farIndex];
                    ASSERT_GT(reference, 0.01) << kSubjects[s].Name << " at " << cutoff
                                               << ": the far wall covers almost nothing; the fixture is broken";
                    const f64 cookedDeviation = std::abs(coverage[2][farIndex] - reference) / reference;
                    worstCooked = std::max(worstCooked, cookedDeviation);
                    worstOff = std::max(worstOff, std::abs(coverage[3][farIndex] - reference) / reference);

                    // One cooked texture, every cutoff: it draws what the loose
                    // chain draws (measured within 0.4% of it), and it holds the
                    // far wall's coverage near the one-level reference. Measured
                    // at worst 11% (pine at 0.75, where a card 13 pixels tall
                    // loses its thinnest needles to bilinear filtering on either
                    // chain); #1441 measured its capped chain 8% under one level.
                    EXPECT_NEAR(coverage[2][farIndex], coverage[1][farIndex], reference * 0.03)
                        << kSubjects[s].Name << " at " << cutoff << ": cooked and loose disagree";
                    EXPECT_LT(cookedDeviation, 0.15)
                        << kSubjects[s].Name << " at " << cutoff << ": the cooked chain lost the far wall's coverage";
                }

                // The control: the cook before #1453. A box chain is wrong in a
                // cutoff-dependent direction — it thickens a card below 0.5 and
                // erases it above (measured +47% at 0.25, -83% at 0.75 on pine)
                // — so the claim is comparative, over all four cutoffs.
                EXPECT_GT(worstOff, 3.0 * worstCooked)
                    << kSubjects[s].Name << ": the plain chain's worst far-pose error (" << worstOff * 100.0
                    << "%) is not clearly worse than the cooked chain's (" << worstCooked * 100.0
                    << "%); the comparison is vacuous";
            }
        }
    }
} // namespace OloEngine::Tests
