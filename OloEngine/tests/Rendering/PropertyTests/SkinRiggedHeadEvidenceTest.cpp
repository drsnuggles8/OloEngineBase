// OLO_TEST_LAYER: L8
// =============================================================================
// SkinRiggedHeadEvidenceTest.cpp -- issue #1259: the ANIMATED digital-human
// close-up subject of the integrated acceptance scene.
//
// The subject is the rigged InfiniteScanHead (assets/models/InfiniteScanHead/
// HeadRigged.gltf, CC BY 3.0 -- see that directory's README.md), playing its
// LookAround clip, with the cooked hair coat GroomAnimals.olo binds to it.
// SkinReferenceHeadEvidenceTest measures the STATIC scan (Head.fbx) as the skin
// reference; this file measures the head the acceptance scene animates, and
// compares the two at a matching pose.
//
// WHAT HAD TO CHANGE FOR THIS HEAD TO BE SKIN AT ALL:
//   * the glTF's only material, HeadSkin, carries the thickness map MEASURED
//     from this mesh (tools/skin-thickness/bake_thickness.py --mesh
//     HeadRigged.gltf) as a KHR_materials_volume thickness texture, factor
//     0.02 m (the map encodes thickness / 20 mm);
//   * the skin KIND and PROFILE cannot ride in a glTF, so the scene patches
//     HeadSkin with a MaterialOverridesComponent -- kAcceptanceHumanEntity below
//     is the exact block, and the scene tests pin its handles to the ones the
//     committed scenes use;
//   * a one-mesh rigged model used to load with an entity-wide copy of its
//     imported material as a MaterialComponent, which shadows every patch; the
//     loader no longer invents one for an entity that patches its materials.
//
// Evidence PNGs: OloEditor/assets/tests/visual/SkinRiggedHead*_GL_<Path>_<Cell>.png.
// GL only (L8: the full Scene pipeline on all three raster paths, RGBA8
// readback; SKIPs cleanly without a GL 4.6 context). The Vulkan rows are live.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Build/GameBuildPipeline.h"
#include "OloEngine/Core/YAMLConverters.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/MaterialOverride.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SkinDiffusion.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/SkinProfileTable.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Groom/GroomGuideSimulation.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // THE ACCEPTANCE SCENE'S HUMAN, as an .olo entity. Every key is spelled as
        // a committed scene spells it: the body from GroomAnimals.olo's "Human",
        // the patch from Dog.olo's MaterialOverridesComponent. GroomAnimals.olo's
        // "HumanCoat" binds to this Entity UUID and was cooked against this
        // transform; move the two together. The fixture below loads THIS text
        // (with the asset handles swapped for memory-only copies), so a key that
        // stops parsing fails here, not in the acceptance scene.
        //
        // Kind 2 is MaterialKind::Skin. SkinProfile is ReferenceHead.oloskin,
        // the profile DigitalHuman.olo's Cranium uses. ThicknessFactor is the
        // glTF's own 0.02 m: a patch always writes its factor, so it must restate
        // it. The thickness MAP is not here -- it comes from the glTF.
        constexpr const char* kAcceptanceHumanEntity = R"(- Entity: 8600225018266145435
  TagComponent:
    Tag: Human
  TransformComponent:
    Translation: [0, 1.24, 3.6]
    Rotation: [0, 0, 0]
    Scale: [1, 1, 1]
  MeshComponent:
    {}
  AnimationStateComponent:
    State: 0
    CurrentTime: 0
    BlendDuration: 0.3
    CurrentClipIndex: 0
    IsPlaying: true
    SourceFilePath: assets/models/InfiniteScanHead/HeadRigged.gltf
    CurrentClipName: LookAround
  SkeletonComponent:
    {}
  MaterialOverridesComponent:
    Overrides:
      - MaterialName: HeadSkin
        Kind: 2
        SkinProfile: 15952688685437936278
        ThicknessFactor: 0.02
        OverrideBaseColor: false
        BaseColor: [1, 1, 1, 1]
        OverrideRoughness: false
        Roughness: 0.5
        OverrideMetallic: false
        Metallic: 0
)";

        constexpr u64 kHumanEntity = 8600225018266145435ULL;
        constexpr u64 kReferenceHeadProfileHandle = 15952688685437936278ULL;
        constexpr f32 kThicknessFactorMetres = 0.02f;
        constexpr const char* kThicknessMapUri = "Textures/HeadRigged_Thickness.png";
        constexpr const char* kHeadMaterial = "HeadSkin";

        // GroomAnimals.olo's placement of the bust (base on the plinth).
        const glm::vec3 kHumanPosition{ 0.0f, 1.24f, 3.6f };
        // The head's centre in the bust's own space (metres, face toward +Z),
        // measured from HeadRigged.bin: ears at y 0.25-0.32, crown 0.43.
        const glm::vec3 kHeadCentre{ 0.0f, 0.32f, 0.01f };

        // Measurement boxes, bust space, measured from the mesh: the RIGHT ear
        // is the box around every vertex whose baked thickness is under 8 mm
        // with x > 5 cm, padded 8 mm; the forehead is the skin above the brow
        // on the midline; the eyelids are the closed-lid band. The forehead
        // and the lids are the controls: the map reads them thick (the lids by
        // the bake's far-side test), so they must not change when it is removed.
        const glm::vec3 kRightEarMin{ 0.063f, 0.244f, -0.020f };
        const glm::vec3 kRightEarMax{ 0.101f, 0.324f, 0.026f };
        const glm::vec3 kForeheadMin{ -0.025f, 0.335f, 0.105f };
        const glm::vec3 kForeheadMax{ 0.025f, 0.360f, 0.120f };
        const glm::vec3 kEyelidsMin{ -0.050f, 0.290f, 0.090f };
        const glm::vec3 kEyelidsMax{ 0.050f, 0.315f, 0.125f };

        constexpr u32 kSize = 640;
        constexpr f32 kCaptureTime = 12.0f;

        [[nodiscard]] fs::path EditorRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT };
        }
        [[nodiscard]] fs::path ProjectRoot()
        {
            return EditorRoot() / "SandboxProject";
        }
        [[nodiscard]] fs::path HeadDir()
        {
            return EditorRoot() / "assets" / "models" / "InfiniteScanHead";
        }
        [[nodiscard]] fs::path ScenePath(const char* name)
        {
            return ProjectRoot() / "Assets" / "Scenes" / name;
        }

        [[nodiscard]] std::vector<u8> ReadBytes(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        }

        [[nodiscard]] YAML::Node FindEntity(const YAML::Node& scene, const std::string& tag)
        {
            for (const YAML::Node& entity : scene["Entities"])
            {
                if (entity["TagComponent"] && entity["TagComponent"]["Tag"].as<std::string>("") == tag)
                    return entity;
            }
            return YAML::Node{};
        }

        // The glTF is JSON, which YAML reads as a subset.
        [[nodiscard]] YAML::Node LoadHeadGltf()
        {
            return YAML::LoadFile((HeadDir() / "HeadRigged.gltf").string());
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

        struct Capture
        {
            std::vector<u8> Pixels;
            u32 Width = 0;
            u32 Height = 0;
        };

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
            f64 MeanLuma = 0.0;
            f64 MeanRed = 0.0;
            f64 MeanGreen = 0.0;
            // Red-dominant pixels: transmitted light through skin is red by
            // construction (see SkinReferenceHeadEvidenceTest's Measure).
            u64 RedPixels = 0;
            u64 ChangedPixels = 0;
            u32 MaxDelta = 0;
        };

        [[nodiscard]] WindowStats Measure(const Capture& capture, const Window& window, const Capture* against = nullptr)
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
                    luma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    red += r;
                    green += g;
                    if (r > 0.04 && r > 1.5 * g)
                        ++stats.RedPixels;
                    if (against != nullptr)
                    {
                        u32 worst = 0;
                        for (u32 c = 0; c < 3; ++c)
                        {
                            worst = std::max(worst, static_cast<u32>(std::abs(static_cast<i32>(capture.Pixels[idx + c]) -
                                                                              static_cast<i32>(against->Pixels[idx + c]))));
                        }
                        if (worst > 0)
                            ++stats.ChangedPixels;
                        stats.MaxDelta = std::max(stats.MaxDelta, worst);
                    }
                }
            }
            const auto n = static_cast<f64>(window.Area());
            stats.MeanLuma = luma / n;
            stats.MeanRed = red / n;
            stats.MeanGreen = green / n;
            return stats;
        }

        // Pixels that differ from `empty` (the same frame with no subject) by
        // more than a small threshold on any channel: the subject's coverage.
        [[nodiscard]] std::vector<u8> Coverage(const Capture& capture, const Capture& empty)
        {
            std::vector<u8> mask(static_cast<sizet>(capture.Width) * capture.Height, 0);
            for (sizet i = 0; i < mask.size(); ++i)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    if (std::abs(static_cast<i32>(capture.Pixels[i * 4u + c]) - static_cast<i32>(empty.Pixels[i * 4u + c])) > 10)
                    {
                        mask[i] = 1;
                        break;
                    }
                }
            }
            return mask;
        }

        void FlipRowsInPlace(std::vector<u8>& rgba, u32 width, u32 height)
        {
            const sizet rowBytes = static_cast<sizet>(width) * 4u;
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
        // process cwd into OloEditor/.
        [[nodiscard]] fs::path VisualOutputPath(const std::string& name)
        {
            const fs::path dir = EditorRoot() / "assets" / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / (name + ".png");
        }

        // Camera poses, relative to the head centre in bust space.
        struct Pose
        {
            const char* Name;
            glm::vec3 Eye;
            glm::vec3 Target;
        };

        constexpr Pose kFront{ "Front", { 0.0f, 0.02f, 0.62f }, { 0.0f, -0.01f, 0.0f } };
        constexpr Pose kThreeQuarter{ "ThreeQuarter", { 0.42f, 0.04f, 0.46f }, { 0.0f, -0.01f, 0.0f } };
        constexpr Pose kProfile{ "Profile", { 0.62f, 0.02f, 0.0f }, { 0.0f, -0.01f, 0.0f } };
        constexpr Pose kCrown{ "Crown", { 0.10f, 0.45f, 0.43f }, { 0.0f, 0.03f, 0.0f } };
        constexpr Pose kHairMotion{ "MotionShoulders", { 0.0f, 0.04f, 0.90f }, { 0.0f, -0.05f, 0.0f } };
        // Front-right, looking back toward the light through the right ear
        // (#1242's exit lobe peaks when the view looks INTO the light).
        constexpr Pose kBacklitEar{ "BacklitEar", { 0.24f, 0.04f, 0.40f }, { 0.06f, -0.04f, 0.0f } };
        // The same backlight, framed wider for the person judging it: the whole
        // right side of the head, ear, jaw and hair against the light.
        constexpr Pose kBacklit{ "Backlit", { 0.36f, 0.06f, 0.52f }, { 0.02f, -0.03f, 0.0f } };

        // The light rigs, DigitalHuman.olo's three lights placed for this
        // bust's size (1.34x the static scene's head): the soft key and hard
        // side are points, the backlight directional and UNSHADOWED, for the
        // reasons SkinReferenceHeadScene pins.
        enum class Rig : u8
        {
            Studio,
            Backlit
        };
        const glm::vec3 kBacklightDirection = glm::normalize(glm::vec3(-0.237f, -0.19f, 0.953f));
    } // namespace

    // =========================================================================
    // The asset and the scene block (no GPU)
    // =========================================================================

    // The rigged head carries ITS OWN measured map, at the factor the map's
    // encoding defines, on its only material -- and its albedo is the scan's
    // own, not darkened by the exporter's 0.8 default.
    TEST(SkinRiggedHeadAsset, HeadSkinCarriesTheMeasuredThicknessMapAsAVolumeTexture)
    {
        const YAML::Node gltf = LoadHeadGltf();
        ASSERT_TRUE(gltf["materials"] && gltf["materials"].size() == 1u) << "HeadRigged.gltf should have one material";
        const YAML::Node material = gltf["materials"][0];
        EXPECT_EQ(material["name"].as<std::string>(""), kHeadMaterial);

        const YAML::Node volume = material["extensions"]["KHR_materials_volume"];
        ASSERT_TRUE(volume) << "HeadSkin has no KHR_materials_volume: the ear is as thick as the skull";
        EXPECT_NEAR(volume["thicknessFactor"].as<f32>(0.0f), kThicknessFactorMetres, 1.0e-6f)
            << "the map encodes thickness / 20 mm; any other factor makes factor * map a wrong thickness";
        const u32 texture = volume["thicknessTexture"]["index"].as<u32>(~0u);
        ASSERT_LT(texture, gltf["textures"].size());
        const u32 image = gltf["textures"][texture]["source"].as<u32>(~0u);
        ASSERT_LT(image, gltf["images"].size());
        EXPECT_EQ(gltf["images"][image]["uri"].as<std::string>(""), kThicknessMapUri);
        EXPECT_TRUE(fs::exists(HeadDir() / kThicknessMapUri)) << "the glTF names a thickness map that is not there";

        const YAML::Node baseColor = material["pbrMetallicRoughness"]["baseColorFactor"];
        ASSERT_TRUE(baseColor && baseColor.size() == 4u);
        for (u32 c = 0; c < 3u; ++c)
        {
            EXPECT_NEAR(baseColor[c].as<f32>(0.0f), 1.0f, 1.0e-6f)
                << "baseColorFactor darkens the scan's measured albedo (the exporter's 0.8 default)";
        }

        bool declared = false;
        for (const YAML::Node& extension : gltf["extensionsUsed"])
            declared = declared || extension.as<std::string>("") == "KHR_materials_volume";
        EXPECT_TRUE(declared) << "KHR_materials_volume is used but not in extensionsUsed";
    }

    // A sanity check that the file is a measurement of THIS bust: square, a thin
    // region, mostly clamped thick. Where the thin region lands (the ears, not
    // the lids or the brow) is the GPU fixture's job.
    TEST(SkinRiggedHeadAsset, TheBakedMapHasAThinRegionAndAThickOne)
    {
        const fs::path file = HeadDir() / kThicknessMapUri;
        int w = 0;
        int h = 0;
        int channels = 0;
        stbi_uc* data = ::stbi_load(file.string().c_str(), &w, &h, &channels, 4);
        ASSERT_NE(data, nullptr) << "could not decode " << file.string();
        u64 thin = 0;
        u64 clamped = 0;
        u8 thinnest = 255;
        const auto texels = static_cast<sizet>(w) * static_cast<sizet>(h);
        for (sizet i = 0; i < texels; ++i)
        {
            const u8 v = data[i * 4u];
            thinnest = std::min(thinnest, v);
            thin += v < 64 ? 1u : 0u; // under 5 mm
            clamped += v == 255 ? 1u : 0u;
        }
        ::stbi_image_free(data);
        EXPECT_EQ(w, h);
        EXPECT_LT(thinnest, 51) << "nothing on the bust measures under 4 mm";
        EXPECT_GT(static_cast<f64>(thin) / static_cast<f64>(texels), 0.001) << "the thin region is vanishingly small";
        EXPECT_GT(static_cast<f64>(clamped) / static_cast<f64>(texels), 0.5) << "a map that is mostly thin is not this bake";
    }

    // The block a scene author copies names the SAME things the committed scenes
    // do: the reference profile, the glTF's material and factor, GroomAnimals'
    // Human (so its cooked coat binds), and Dog.olo's spelling of the skin kind.
    TEST(SkinRiggedHeadScene, TheAcceptanceBlockNamesTheCommittedHandles)
    {
        const YAML::Node block = YAML::Load(kAcceptanceHumanEntity);
        ASSERT_TRUE(block.IsSequence() && block.size() == 1u);
        const YAML::Node human = block[0];
        const YAML::Node patch = human["MaterialOverridesComponent"]["Overrides"][0];
        ASSERT_TRUE(patch);

        const YAML::Node digitalHuman = YAML::LoadFile(ScenePath("DigitalHuman.olo").string());
        const YAML::Node cranium = FindEntity(digitalHuman, "Cranium");
        ASSERT_TRUE(cranium);
        EXPECT_EQ(patch["SkinProfile"].as<u64>(0), cranium["MaterialComponent"]["SkinProfile"].as<u64>(1));
        EXPECT_EQ(patch["SkinProfile"].as<u64>(0), kReferenceHeadProfileHandle);
        EXPECT_EQ(patch["Kind"].as<i32>(-1), cranium["MaterialComponent"]["MaterialKind"].as<i32>(-2))
            << "the patch's kind is not the reference head's";
        EXPECT_EQ(patch["Kind"].as<i32>(-1), static_cast<i32>(MaterialKind::Skin));

        const YAML::Node gltf = LoadHeadGltf();
        EXPECT_EQ(patch["MaterialName"].as<std::string>(""), gltf["materials"][0]["name"].as<std::string>("?"));
        EXPECT_NEAR(patch["ThicknessFactor"].as<f32>(0.0f),
                    gltf["materials"][0]["extensions"]["KHR_materials_volume"]["thicknessFactor"].as<f32>(-1.0f), 1.0e-6f)
            << "a patch always writes its factor, so it must restate the glTF's";

        const YAML::Node groomAnimals = YAML::LoadFile(ScenePath("GroomAnimals.olo").string());
        const YAML::Node shipped = FindEntity(groomAnimals, "Human");
        const YAML::Node coat = FindEntity(groomAnimals, "HumanCoat");
        ASSERT_TRUE(shipped && coat);
        EXPECT_EQ(human["Entity"].as<u64>(0), shipped["Entity"].as<u64>(1));
        EXPECT_EQ(coat["GroomBindingComponent"]["TargetEntity"].as<u64>(0), kHumanEntity)
            << "GroomAnimals' HumanCoat no longer binds to this entity";
        const auto ours = human["TransformComponent"]["Translation"].as<glm::vec3>();
        const auto theirs = shipped["TransformComponent"]["Translation"].as<glm::vec3>();
        for (int c = 0; c < 3; ++c)
            EXPECT_NEAR(ours[c], theirs[c], 1.0e-6f) << "the coat's binding was cooked against GroomAnimals' placement";
        EXPECT_EQ(human["AnimationStateComponent"]["SourceFilePath"].as<std::string>(""),
                  shipped["AnimationStateComponent"]["SourceFilePath"].as<std::string>("?"));
    }

    // THE PACKAGED PATH. A shipped game reads the head by its content path, so
    // the build stages the glTF and every file it opens. The thickness map rides
    // in the glTF's images, and must arrive beside it.
    TEST(SkinRiggedHeadScene, TheGameBuildStagesTheThicknessMapWithTheHead)
    {
        const fs::path root = TempDir("rigged-head-stage");
        const fs::path project = root / "Project";
        const fs::path output = root / "Game";
        std::error_code ec;
        fs::create_directories(project / "Assets" / "Scenes", ec);
        ASSERT_FALSE(ec);
        const fs::path scene = project / "Assets" / "Scenes" / "Head.olo";
        {
            std::ofstream out(scene);
            out << "Scene: Head.olo\nVersion: 1\nEntities:\n";
            std::istringstream block(kAcceptanceHumanEntity);
            for (std::string line; std::getline(block, line);)
                out << "  " << line << "\n";
        }

        sizet copied = 0;
        std::vector<std::string> unresolved;
        std::string error;
        ASSERT_TRUE(StageSceneReferencedContent({ scene }, project, "Assets", EditorRoot(), output, copied, unresolved, error))
            << error;
        for (const std::string& missing : unresolved)
            ADD_FAILURE() << "unresolved: " << missing;
        const fs::path staged = output / "assets" / "models" / "InfiniteScanHead";
        EXPECT_TRUE(fs::exists(staged / "HeadRigged.gltf"));
        EXPECT_TRUE(fs::exists(staged / "HeadRigged.bin"));
        EXPECT_TRUE(fs::exists(staged / "Textures" / "lambertian.jpg"));
        EXPECT_TRUE(fs::exists(staged / kThicknessMapUri))
            << "the packaged game would load a uniformly thick head: the thickness map was not staged";
        fs::remove_all(root, ec);
    }

    // =========================================================================
    // The subject on the GPU
    // =========================================================================

    class SkinRiggedHeadCloseUp : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            if (!RenderPropertyFixture::IsGpuAvailable())
                return;

            m_SavedPath = Renderer3D::GetRendererSettings().Path;
            EnableRendering(kSize, kSize);
            SetUpScratchProject();
            ASSERT_FALSE(HasFatalFailure());
            auto& rs = Renderer3D::GetRendererSettings();
            rs.EditorDebugDrawsEnabled = false;
            rs.ShowComponentGizmos = false;
            rs.ShowGrid = false;
            rs.ShowWorldAxisHelper = false;

            // The shipped profile, through the shipped reader.
            {
                const std::string yaml = []
                {
                    std::ifstream in(ProjectRoot() / "Assets" / "Materials" / "ReferenceHead.oloskin");
                    std::stringstream s;
                    s << in.rdbuf();
                    return s.str();
                }();
                auto profile = Ref<SkinProfile>::Create();
                ASSERT_TRUE(SkinProfileSerializer{}.DeserializeFromYAML(yaml, profile));
                m_Profile = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
            }

            // The cooked coat GroomAnimals.olo binds to this head, read with the
            // shipped decoders.
            const fs::path groomDir = ProjectRoot() / "Assets" / "Grooms" / "Animals";
            std::string reason;
            Ref<GroomAsset> groom;
            Ref<GroomBindingAsset> binding;
            {
                const std::vector<u8> bytes = ReadBytes(groomDir / "Human.ologroom");
                ASSERT_TRUE(GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), groom, reason)) << reason;
            }
            {
                const std::vector<u8> bytes = ReadBytes(groomDir / "Human.ologroombinding");
                ASSERT_TRUE(GroomBindingSerializer::DecodeFromBytes(bytes.data(), bytes.size(), binding, reason)) << reason;
            }
            const AssetHandle groomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(groom);
            const AssetHandle bindingHandle = AssetManager::AddMemoryOnlyAsset<GroomBindingAsset>(binding);

            // THE ACCEPTANCE BLOCK and GroomAnimals' coat, loaded by the scene
            // serializer; only the asset handles are swapped for the memory copies.
            YAML::Node human = YAML::Load(kAcceptanceHumanEntity)[0];
            human["MaterialOverridesComponent"]["Overrides"][0]["SkinProfile"] = static_cast<u64>(m_Profile);
            YAML::Node coat = YAML::Clone(FindEntity(YAML::LoadFile(ScenePath("GroomAnimals.olo").string()), "HumanCoat"));
            ASSERT_TRUE(coat) << "GroomAnimals.olo has no HumanCoat";
            coat["GroomComponent"]["Groom"] = static_cast<u64>(groomHandle);
            coat["GroomBindingComponent"]["Binding"] = static_cast<u64>(bindingHandle);
            YAML::Node entities(YAML::NodeType::Sequence);
            entities.push_back(human);
            entities.push_back(coat);
            const TArray<UUID> created = SceneSerializer(GetSceneRef()).DeserializeAdditive(entities);
            ASSERT_EQ(created.Num(), 2);
            m_Human = GetScene().GetEntityByUUID(UUID(kHumanEntity));
            m_Coat = GetScene().GetEntityByUUID(created[1]);
            ASSERT_TRUE(m_Human && m_Coat);
            ASSERT_TRUE(m_Human.HasComponent<MeshComponent>()) << "the head's glTF did not load";

            m_Camera = GetScene().CreateEntity("Camera");
            {
                auto& cc = m_Camera.AddComponent<CameraComponent>();
                cc.Primary = true;
                cc.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
                cc.Camera.SetPerspective(glm::radians(30.0f), 0.01f, 100.0f);
                cc.Camera.SetViewportSize(kSize, kSize);
            }
            m_SoftKey = GetScene().CreateEntity("Soft Key");
            {
                auto& p = m_SoftKey.AddComponent<PointLightComponent>();
                p.m_Color = { 1.0f, 0.97f, 0.93f };
                p.m_Range = 4.0f;
                p.m_CastShadows = true;
            }
            m_HardSide = GetScene().CreateEntity("Hard Side");
            {
                auto& p = m_HardSide.AddComponent<PointLightComponent>();
                p.m_Color = { 1.0f, 0.96f, 0.90f };
                p.m_Range = 4.0f;
                p.m_CastShadows = true;
            }
            m_Backlight = GetScene().CreateEntity("Backlight");
            {
                auto& d = m_Backlight.AddComponent<DirectionalLightComponent>();
                d.m_Color = { 0.94f, 0.95f, 1.0f };
                d.m_CastShadows = false;
            }
            PlaceLights();
            SetRig(Rig::Studio);
            HoldPose(0.0f);
        }

        void TearDown() override
        {
            Renderer3D::GetSkinDiffusionSettings() = SkinDiffusionSettings{};
            Renderer3D::GetPostProcessSettings().MaterialDebug = MaterialDebugView::None;
            Renderer3D::GetPostProcessSettings().TAAEnabled = false;
            Renderer3D::GetRendererSettings().Path = m_SavedPath;
            Renderer3D::GetSkinProfileTable().Reset();
            RestoreThicknessMap();

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
            m_ProjectDir = TempDir("skin-rigged-head-project");
            fs::create_directories(m_ProjectDir / "Assets", ec);
            ASSERT_FALSE(ec);
            const fs::path projectFile = m_ProjectDir / "SkinRiggedHead.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: SkinRiggedHead\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile));
            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        // Bust space (the glTF's metres) to world, through the body's CURRENT
        // placement, so every camera, light and window follows a moved body.
        [[nodiscard]] glm::vec3 BustToWorld(const glm::vec3& bust) const
        {
            return glm::vec3(m_Body * glm::vec4(bust, 1.0f));
        }

        [[nodiscard]] glm::vec3 HeadWorld(const glm::vec3& offsetFromCentre) const
        {
            return BustToWorld(kHeadCentre + offsetFromCentre);
        }

        // The lights are authored relative to the head, so they move with it.
        void PlaceLights()
        {
            m_SoftKey.GetComponent<TransformComponent>().Translation = HeadWorld({ 0.30f, 0.40f, 0.56f });
            m_HardSide.GetComponent<TransformComponent>().Translation = HeadWorld({ -0.62f, 0.05f, 0.08f });
            m_Backlight.GetComponent<DirectionalLightComponent>().m_Direction =
                glm::normalize(glm::mat3(m_Body) * kBacklightDirection);
        }

        // Puts the body somewhere else and turns it about +Y, as a scene does.
        void PlaceBody(const glm::vec3& translation, f32 yawRadians)
        {
            auto& transform = m_Human.GetComponent<TransformComponent>();
            transform.Translation = translation;
            transform.SetRotationEuler(glm::vec3(0.0f, yawRadians, 0.0f));
            m_Body = transform.GetTransform();
            PlaceLights();
        }

        void SetRig(Rig rig)
        {
            const bool studio = rig == Rig::Studio;
            m_SoftKey.GetComponent<PointLightComponent>().m_Intensity = studio ? 10.0f : 0.5f;
            m_HardSide.GetComponent<PointLightComponent>().m_Intensity = studio ? 1.0f : 0.5f;
            m_Backlight.GetComponent<DirectionalLightComponent>().m_Intensity = studio ? 0.4f : 6.0f;
        }

        // Freeze the clip at `seconds` (the rest pose at 0).
        void HoldPose(f32 seconds)
        {
            auto& anim = m_Human.GetComponent<AnimationStateComponent>();
            anim.m_CurrentTime = seconds;
            anim.m_IsPlaying = false;
        }

        void ShowHair(bool show)
        {
            m_Coat.GetComponent<GroomComponent>().m_RenderStrands = show;
        }

        void PoseCamera(const Pose& pose)
        {
            auto& transform = m_Camera.GetComponent<TransformComponent>();
            transform.Translation = HeadWorld(pose.Eye);
            const glm::vec3 target = HeadWorld(pose.Target);
            transform.SetRotation(glm::quatLookAt(glm::normalize(target - transform.Translation), glm::vec3(0.0f, 1.0f, 0.0f)));
        }

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
                const glm::vec3 p = BustToWorld(glm::vec3{ (corner & 1u) ? hi.x : lo.x, (corner & 2u) ? hi.y : lo.y,
                                                           (corner & 4u) ? hi.z : lo.z });
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

        struct Settings
        {
            RenderingPath Path = RenderingPath::Forward;
            MaterialDebugView View = MaterialDebugView::None;
            // TAA on for the frames a person judges (the coat is
            // stochastic-alpha and resolves through it), off for the frames a
            // number is read from, so a temporal blend cannot be a confound.
            bool Taa = false;
            u32 Frames = 6;
            u32 Width = kSize;
            u32 Height = kSize;
        };

        [[nodiscard]] bool CaptureFrame(const Settings& s, const Pose& pose, const std::string& name, Capture& out)
        {
            if (s.Width != m_CaptureWidth || s.Height != m_CaptureHeight)
            {
                ResizeRenderTarget(s.Width, s.Height);
                m_CaptureWidth = s.Width;
                m_CaptureHeight = s.Height;
            }
            Renderer3D::GetSkinDiffusionSettings().Enabled = true;
            Renderer3D::GetSkinDiffusionSettings().Quality = SkinDiffusionQuality::High;
            Renderer3D::GetRendererSettings().Path = s.Path;
            auto& post = Renderer3D::GetPostProcessSettings();
            post.MaterialDebug = s.View;
            post.TAAEnabled = s.Taa;
            post.TAAFeedback = 0.9f;
            post.TAASharpness = 0.0f;
            Renderer3D::ApplyRendererSettings();

            const ScopedMockTime clock(kCaptureTime);
            PoseCamera(pose);
            RunFrames(s.Frames);
            if (!ReadbackComposite(out.Pixels, out.Width, out.Height) || out.Width != s.Width || out.Height != s.Height)
                return false;
            FlipRowsInPlace(out.Pixels, out.Width, out.Height);
            if (!name.empty())
            {
                const fs::path file = VisualOutputPath(name);
                EXPECT_NE(::stbi_write_png(file.string().c_str(), static_cast<int>(out.Width), static_cast<int>(out.Height), 4,
                                           out.Pixels.data(), static_cast<int>(out.Width) * 4),
                          0)
                    << "failed to write " << file.string();
            }
            return true;
        }

        // THE CONTROL ARM: the same head with the thickness map taken off its
        // imported material, so HeadSkin is uniformly ThicknessFactor (20 mm)
        // thick. The patch cache is dropped so the patched copy is rebuilt from
        // the edited material; RestoreThicknessMap undoes both (the imported
        // material is shared through the model cache).
        void RemoveThicknessMap()
        {
            Ref<Material> imported = ImportedHeadSkin();
            ASSERT_TRUE(imported);
            m_RemovedMap = imported->GetThicknessMap();
            ASSERT_TRUE(m_RemovedMap) << "HeadSkin has no thickness map to remove";
            imported->SetThicknessMap(nullptr);
            m_Human.GetComponent<MaterialOverridesComponent>().m_Cache.Reset();
        }

        void RestoreThicknessMap()
        {
            if (!m_RemovedMap)
                return;
            if (Ref<Material> imported = ImportedHeadSkin())
                imported->SetThicknessMap(m_RemovedMap);
            m_RemovedMap.Reset();
            if (m_Human && m_Human.HasComponent<MaterialOverridesComponent>())
                m_Human.GetComponent<MaterialOverridesComponent>().m_Cache.Reset();
        }

        [[nodiscard]] Ref<Material> ImportedHeadSkin() const
        {
            if (!m_Human || !m_Human.HasComponent<MeshComponent>() || !m_Human.GetComponent<MeshComponent>().m_MeshSource)
                return nullptr;
            for (const Ref<Material>& material : m_Human.GetComponent<MeshComponent>().m_MeshSource->GetImportedMaterials())
            {
                if (material && material->GetName().ToView() == kHeadMaterial)
                    return material;
            }
            return nullptr;
        }

        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_ProjectDir;
        AssetHandle m_Profile{};
        Entity m_Human;
        Entity m_Coat;
        Entity m_Camera;
        Entity m_SoftKey;
        Entity m_HardSide;
        Entity m_Backlight;
        Ref<Texture2D> m_RemovedMap;
        glm::mat4 m_Body = glm::translate(glm::mat4(1.0f), kHumanPosition);
        RenderingPath m_SavedPath = RenderingPath::Forward;
        u32 m_CaptureWidth = kSize;
        u32 m_CaptureHeight = kSize;
    };

    // The import carries the map, and the scene block makes the head SKIN: the
    // entity shades with the patched copy (no MaterialComponent shadows it),
    // and the copy keeps the albedo and the measured map.
    TEST_F(SkinRiggedHeadCloseUp, TheLoadedHeadShadesAsSkinWithItsMeasuredMap)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const Ref<Material> imported = ImportedHeadSkin();
        ASSERT_TRUE(imported) << "the head's mesh has no imported HeadSkin";
        ASSERT_TRUE(imported->GetThicknessMap()) << "the importer dropped HeadSkin's KHR_materials_volume thickness texture";
        EXPECT_FALSE(imported->GetThicknessMap()->GetSpecification().SRGB) << "a thickness is not a colour";
        EXPECT_NE(imported->GetThicknessMap()->GetPath().find("HeadRigged_Thickness"), std::string::npos);
        EXPECT_NEAR(imported->GetThicknessFactor(), kThicknessFactorMetres, 1.0e-6f);

        EXPECT_FALSE(m_Human.HasComponent<MaterialComponent>())
            << "a MaterialComponent overrides every submesh, so the HeadSkin patch is inactive";
        const Ref<MeshSource> source = m_Human.GetComponent<MeshComponent>().m_MeshSource;
        const MaterialOverrideCache* patches = GetScene().PrepareMaterialOverrides(m_Human, source->GetImportedMaterials());
        ASSERT_NE(patches, nullptr) << "the HeadSkin patch is inactive";
        const Material* skin = ResolveMaterialPatch(patches, source->GetImportedMaterialPtrForSubmesh(0));
        ASSERT_NE(skin, nullptr) << "submesh 0 does not shade with the patched copy";
        EXPECT_EQ(skin->GetMaterialKind(), MaterialKind::Skin);
        EXPECT_EQ(static_cast<u64>(skin->GetSkinProfileHandle()), static_cast<u64>(m_Profile));
        EXPECT_NEAR(skin->GetThicknessFactor(), kThicknessFactorMetres, 1.0e-6f);
        EXPECT_EQ(skin->GetThicknessMap(), imported->GetThicknessMap()) << "the patched copy lost the measured map";
        EXPECT_TRUE(skin->GetAlbedoMap()) << "the patched copy lost the scan's albedo";
    }

    // THE CLAIM THE MAP EXISTS FOR, with its negative control: under the
    // backlight the right ear transmits, and removing the map (uniform 20 mm)
    // takes that away. The forehead and the closed lids are thick in both arms,
    // so they must not move: a change confined to the ear is the map, not the
    // frame. Hair hidden and TAA off so neither is a confound; every path.
    TEST_F(SkinRiggedHeadCloseUp, TheBacklitEarTransmitsThroughTheMeasuredMapOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ShowHair(false);
        SetRig(Rig::Backlit);

        for (const RenderingPath path : kPaths)
        {
            const char* p = PathName(path);
            const Settings s{ path };
            Capture repeatA;
            Capture repeatB;
            Capture mapped;
            Capture uniform;
            ASSERT_TRUE(CaptureFrame(s, kBacklitEar, {}, repeatA));
            ASSERT_TRUE(CaptureFrame(s, kBacklitEar, {}, repeatB));
            ASSERT_TRUE(CaptureFrame(s, kBacklitEar, std::string("SkinRiggedHeadSkin_GL_") + p + "_BacklitEar", mapped));
            ASSERT_NO_FATAL_FAILURE(RemoveThicknessMap());
            ASSERT_TRUE(CaptureFrame(s, kBacklitEar, std::string("SkinRiggedHeadSkinNoMap_GL_") + p + "_BacklitEar", uniform));
            RestoreThicknessMap();

            Window ear;
            Window brow;
            Window lids;
            ASSERT_TRUE(ProjectBox(kBacklitEar, kRightEarMin, kRightEarMax, ear)) << "the ear is off-frame";
            ASSERT_TRUE(ProjectBox(kBacklitEar, kForeheadMin, kForeheadMax, brow)) << "the forehead is off-frame";
            ASSERT_TRUE(ProjectBox(kBacklitEar, kEyelidsMin, kEyelidsMax, lids)) << "the eyelids are off-frame";

            const u32 floor = Measure(repeatA, Window{ 0, 0, kSize - 1u, kSize - 1u }, &repeatB).MaxDelta;
            const WindowStats earMapped = Measure(mapped, ear, &uniform);
            const WindowStats earUniform = Measure(uniform, ear);
            const WindowStats browDelta = Measure(mapped, brow, &uniform);
            const WindowStats lidDelta = Measure(mapped, lids, &uniform);
            std::printf("[RiggedHead %s] ear (%ux%u): red-dominant %llu (map) vs %llu (uniform 20 mm), mean red %.4f vs "
                        "%.4f, %llu px changed max %u; forehead changed %llu max %u; lids changed %llu max %u; repeat "
                        "floor %u\n",
                        p, ear.X1 - ear.X0 + 1u, ear.Y1 - ear.Y0 + 1u, static_cast<unsigned long long>(earMapped.RedPixels),
                        static_cast<unsigned long long>(earUniform.RedPixels), earMapped.MeanRed, earUniform.MeanRed,
                        static_cast<unsigned long long>(earMapped.ChangedPixels), earMapped.MaxDelta,
                        static_cast<unsigned long long>(browDelta.ChangedPixels), browDelta.MaxDelta,
                        static_cast<unsigned long long>(lidDelta.ChangedPixels), lidDelta.MaxDelta, floor);

            EXPECT_GT(earMapped.MaxDelta, floor + 8u)
                << p << ": the backlit ear looks the same with the measured map as at a uniform 20 mm";
            EXPECT_GT(earMapped.MeanRed, earUniform.MeanRed)
                << p << ": the thin ear got no redder -- transmission through it adds light";
            EXPECT_GT(earMapped.RedPixels, earUniform.RedPixels) << p << ": no more of the ear transmits with the map";
            EXPECT_LE(browDelta.MaxDelta, floor + 2u)
                << p << ": the forehead changed when the map was removed. It is 20 mm in both arms, so the map is not "
                        "landing where it was measured (an upside-down map, or the wrong UV set)";
            // The lids keep a thin crease line in the bake (tools/skin-thickness
            // README: "thin lines at the eyelid crease"), so they may move, but
            // far less than the ear.
            EXPECT_LT(lidDelta.ChangedPixels * ear.Area(), earMapped.ChangedPixels * lids.Area())
                << p << ": the closed lids change as much per pixel as the ear";
        }
    }

    // The same A/B in the deferred transmission AOV, where nothing but the
    // transmitted term is drawn: lit at the ear with the map, dark without.
    TEST_F(SkinRiggedHeadCloseUp, TheTransmissionViewIsLitAtTheEarOnlyWithTheMap)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ShowHair(false);
        SetRig(Rig::Backlit);
        const Settings s{ RenderingPath::Deferred, MaterialDebugView::Transmission };

        Capture mapped;
        Capture uniform;
        ASSERT_TRUE(CaptureFrame(s, kBacklitEar, "SkinRiggedHeadTransmission_GL_Deferred_BacklitEar", mapped));
        ASSERT_NO_FATAL_FAILURE(RemoveThicknessMap());
        ASSERT_TRUE(CaptureFrame(s, kBacklitEar, "SkinRiggedHeadTransmissionNoMap_GL_Deferred_BacklitEar", uniform));
        RestoreThicknessMap();

        Window ear;
        Window brow;
        ASSERT_TRUE(ProjectBox(kBacklitEar, kRightEarMin, kRightEarMax, ear));
        ASSERT_TRUE(ProjectBox(kBacklitEar, kForeheadMin, kForeheadMax, brow));
        const WindowStats earMapped = Measure(mapped, ear);
        const WindowStats earUniform = Measure(uniform, ear);
        const WindowStats browMapped = Measure(mapped, brow);
        const f64 earShare = static_cast<f64>(earMapped.RedPixels) / static_cast<f64>(ear.Area());
        const f64 browShare = static_cast<f64>(browMapped.RedPixels) / static_cast<f64>(brow.Area());
        std::printf("[RiggedHead transmission AOV] ear red-dominant %llu of %llu (map) vs %llu (uniform); forehead "
                    "%llu of %llu\n",
                    static_cast<unsigned long long>(earMapped.RedPixels), static_cast<unsigned long long>(ear.Area()),
                    static_cast<unsigned long long>(earUniform.RedPixels),
                    static_cast<unsigned long long>(browMapped.RedPixels), static_cast<unsigned long long>(brow.Area()));
        EXPECT_GT(earMapped.RedPixels, ear.Area() / 100u) << "the transmission view is black over the ear";
        EXPECT_LT(earUniform.RedPixels * 4u, earMapped.RedPixels) << "a uniform 20 mm transmits as much as the map";
        EXPECT_GT(earShare, browShare * 4.0) << "the glow is not where the map is thin";
    }

    // The close-up framings a person judges, with the coat, on every path:
    // front, three-quarter, profile under the studio rig, and the backlit ear.
    // The numbers only guard against an empty or blown frame; the PNGs are the
    // evidence.
    TEST_F(SkinRiggedHeadCloseUp, CloseUpFramingsOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ShowHair(true);

        for (const RenderingPath path : kPaths)
        {
            const char* p = PathName(path);
            for (const Pose& pose : { kFront, kThreeQuarter, kProfile, kBacklit })
            {
                SetRig(std::string_view(pose.Name) == kBacklit.Name ? Rig::Backlit : Rig::Studio);
                Capture frame;
                ASSERT_TRUE(CaptureFrame(Settings{ path, MaterialDebugView::None, /*Taa=*/true, /*Frames=*/48 }, pose,
                                         std::string("SkinRiggedHead_GL_") + p + "_" + pose.Name, frame));
                const WindowStats all = Measure(frame, Window{ 0, 0, kSize - 1u, kSize - 1u });
                std::printf("[RiggedHead %s %s] frame mean luma %.4f\n", p, pose.Name, all.MeanLuma);
                EXPECT_GT(all.MeanLuma, 0.02) << p << " " << pose.Name << ": the frame is black";
                EXPECT_LT(all.MeanLuma, 0.9) << p << " " << pose.Name << ": the frame is blown out";
            }
        }
    }

    // The changed shipping pair goes through the real pack builder and runtime
    // manager, not a hand-built payload or an editor fallback. The unchanged
    // head's path-backed dependencies are staged by the scene test above.
    TEST_F(SkinRiggedHeadCloseUp, NativePackedHairKeepsItsCookedPairAndBinding)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const Settings settings{ RenderingPath::Forward, MaterialDebugView::None, true, 48, 1920, 1080 };
        Capture loose;
        ASSERT_TRUE(CaptureFrame(settings, kThreeQuarter, "HumanHairNative_GL_Forward_Loose", loose));
        const auto profile = AssetManager::GetAsset<SkinProfile>(m_Profile);
        ASSERT_TRUE(profile);
        AssetRegistry selected;
        std::array<AssetHandle, 2> handles{};
        constexpr std::array<const char*, 2> files{ "Human.ologroom", "Human.ologroombinding" };
        for (sizet i = 0; i < files.size(); ++i)
        {
            const fs::path destination = m_ProjectDir / "Assets" / files[i];
            fs::copy_file(ProjectRoot() / "Assets/Grooms/Animals" / files[i], destination);
            handles[i] = m_AssetManager->ImportAsset(destination);
            ASSERT_NE(static_cast<u64>(handles[i]), 0u);
            selected.AddAsset(m_AssetManager->GetMetadata(handles[i]));
        }
        ASSERT_TRUE(m_AssetManager->SerializeAssetRegistry());
        AssetPackBuilder::BuildSettings build;
        build.m_OutputPath = m_ProjectDir / "Game/Assets/AssetPack.olopack";
        build.m_IncludeScriptModule = false;
        build.m_IncludeLocalizationFiles = false;
        std::atomic<f32> progress{ 0.0f };
        const auto packed = AssetPackBuilder::BuildFromRegistry(selected, build, progress);
        ASSERT_TRUE(packed.m_Success) << packed.m_ErrorMessage;
        EXPECT_EQ(packed.m_FailedAssetCount, 0u);
        struct RestoreManager
        {
            Ref<AssetManagerBase> Previous = Project::GetAssetManager();
            ~RestoreManager()
            {
                Project::SetAssetManager(Previous);
            }
        } restore;
        auto runtime = Ref<RuntimeAssetManager>::Create(false);
        ASSERT_TRUE(runtime->LoadAssetPack(build.m_OutputPath));
        Project::SetAssetManager(runtime);
        runtime->AddMemoryOnlyAsset(profile); // unchanged skin control, not hair
        const auto groom = AssetManager::GetAsset<GroomAsset>(handles[0]);
        const auto binding = AssetManager::GetAsset<GroomBindingAsset>(handles[1]);
        ASSERT_TRUE(groom && binding);
        std::string reason;
        std::vector<u8> bytes;
        ASSERT_TRUE(GroomSerializer::EncodeToBytes(*groom, bytes, reason)) << reason;
        EXPECT_EQ(bytes, ReadBytes(ProjectRoot() / "Assets/Grooms/Animals/Human.ologroom"));
        ASSERT_TRUE(GroomBindingSerializer::EncodeToBytes(*binding, bytes, reason)) << reason;
        EXPECT_EQ(bytes, ReadBytes(ProjectRoot() / "Assets/Grooms/Animals/Human.ologroombinding"));
        m_Coat.GetComponent<GroomComponent>().m_Groom = handles[0];
        m_Coat.GetComponent<GroomBindingComponent>().m_Binding = handles[1];
        Capture frame;
        ASSERT_TRUE(CaptureFrame(settings, kThreeQuarter, "HumanHairNative_GL_Forward_Packed", frame));
        const auto* pass = Renderer3D::GetGroomRenderPass();
        ASSERT_NE(pass, nullptr);
        EXPECT_EQ(pass->GetStats().GroomsBindingRefused, 0u);
        EXPECT_EQ(pass->GetStats().RootsHeldAtRest, 0u);
        EXPECT_EQ(pass->GetStats().GroomsDeformed, 1u);
    }

    // A reproducible standalone distribution for the cold-process asset cell.
    // Export only on request; ordinary CI exercises the pack round-trip above.
    TEST_F(SkinRiggedHeadCloseUp, ExportsHumanHairGame)
    {
        const char* output = std::getenv("OLO_HUMAN_HAIR_GAME");
        if (output == nullptr || *output == '\0')
            GTEST_SKIP() << "Set OLO_HUMAN_HAIR_GAME to a scratch output directory";
        OLO_ENSURE_GPU_OR_SKIP();
        const std::array<fs::path, 3> sources{
            ProjectRoot() / "Assets/Grooms/Animals/Human.ologroom",
            ProjectRoot() / "Assets/Grooms/Animals/Human.ologroombinding",
            ProjectRoot() / "Assets/Materials/ReferenceHead.oloskin"
        };
        std::array<AssetHandle, 3> handles{};
        for (sizet i = 0; i < sources.size(); ++i)
        {
            const auto destination = m_ProjectDir / "Assets" / sources[i].filename();
            fs::copy_file(sources[i], destination);
            handles[i] = m_AssetManager->ImportAsset(destination);
            ASSERT_NE(static_cast<u64>(handles[i]), 0u);
        }
        m_Coat.GetComponent<GroomComponent>().m_Groom = handles[0];
        m_Coat.GetComponent<GroomBindingComponent>().m_Binding = handles[1];
        m_Human.GetComponent<MaterialOverridesComponent>().m_Overrides[0].SkinProfile = handles[2];
        auto& anim = m_Human.GetComponent<AnimationStateComponent>();
        anim.m_CurrentTime = 0.0f;
        anim.m_IsPlaying = true;
        anim.m_PlaybackSpeed = 0.25f;
        Entity camera = m_Camera;
        auto& transform = camera.GetComponent<TransformComponent>();
        transform.Translation = BustToWorld(kHeadCentre + kHairMotion.Eye);
        transform.SetRotationEuler({ -std::atan2(0.09f, 0.9f), 0.0f, 0.0f });
        auto& cc = camera.GetComponent<CameraComponent>();
        cc.Camera.SetPerspective(glm::radians(30.0f), 0.01f, 400.0f);
        cc.Primary = true;
        u32 primaryCameras = 0;
        auto cameras = GetScene().GetAllEntitiesWith<CameraComponent>();
        for (const auto entity : cameras)
            primaryCameras += cameras.get<CameraComponent>(entity).Primary ? 1u : 0u;
        ASSERT_EQ(primaryCameras, 1u);
        ASSERT_EQ(GetScene().GetPrimaryCameraEntity(), camera);
        const fs::path scenePath = m_ProjectDir / "Assets/Hair1593.olo";
        SceneSerializer(GetSceneRef()).Serialize(scenePath.string());
        ASSERT_NE(static_cast<u64>(m_AssetManager->ImportAsset(scenePath)), 0u);
        ASSERT_TRUE(m_AssetManager->SerializeAssetRegistry());
        GameBuildSettings settings;
        settings.GameName = "HumanHair1593";
        settings.OutputDirectory = fs::path(output);
        settings.StartScene = "Hair1593.olo";
        settings.IncludeScriptModule = false;
        std::atomic<f32> progress{ 0.0f };
        const auto built = GameBuildPipeline::Build(settings, progress);
        ASSERT_TRUE(built.Success) << built.ErrorMessage;
        EXPECT_TRUE(fs::exists(built.OutputPath / "Assets/AssetPack.olopack"));
        EXPECT_TRUE(fs::exists(built.OutputPath / "assets/models/InfiniteScanHead/HeadRigged.bin"));
    }

    // #1593: native-resolution authoring evidence. These are inspected images,
    // not a numerical realism verdict. Each path uses the same lights and pose,
    // includes the hair-off control and exposes the physical fibre lobes.
    TEST_F(SkinRiggedHeadCloseUp, NativeHairAuthoringEvidenceOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const RenderingPath path : kPaths)
        {
            const std::string prefix = std::string("HumanHairNative_GL_") + PathName(path) + "_";
            const Settings settings{ path, MaterialDebugView::None, true, 48, 1920, 1080 };
            HoldPose(0.0f);
            ShowHair(true);
            for (const Pose& pose : { kFront, kThreeQuarter, kProfile, kCrown, kBacklit })
            {
                SetRig(std::string_view(pose.Name) == kBacklit.Name ? Rig::Backlit : Rig::Studio);
                Capture frame;
                ASSERT_TRUE(CaptureFrame(settings, pose, prefix + pose.Name, frame));
                const WindowStats all = Measure(frame, Window{ 0, 0, frame.Width - 1u, frame.Height - 1u });
                EXPECT_GT(all.MeanLuma, 0.02) << "black evidence frame";
                EXPECT_LT(all.MeanLuma, 0.9) << "blown evidence frame";
            }
            SetRig(Rig::Studio);
            ShowHair(false);
            Capture without;
            ASSERT_TRUE(CaptureFrame(settings, kFront, prefix + "HairOff", without));
            ShowHair(true);
            auto& fibre = m_Coat.GetComponent<GroomFibreComponent>();
            for (const GroomFibreDebugMode lobe : { GroomFibreDebugMode::LobeR, GroomFibreDebugMode::LobeTT,
                                                    GroomFibreDebugMode::LobeTRT, GroomFibreDebugMode::MultipleScattering })
            {
                fibre.m_DebugMode = static_cast<u8>(lobe);
                Capture tap;
                ASSERT_TRUE(CaptureFrame(settings, kFront, prefix + "Lobe" + std::to_string(fibre.m_DebugMode), tap));
            }
            fibre.m_DebugMode = static_cast<u8>(GroomFibreDebugMode::Full);

            // Advance the real Scene continuously through both turns and the
            // nod, rather than substituting static poses for motion evidence.
            auto& anim = m_Human.GetComponent<AnimationStateComponent>();
            anim.m_CurrentTime = 0.0f;
            anim.m_IsPlaying = true;
            u32 playedFrames = 0;
            for (const f32 time : { 0.05f, 0.45f, 1.5f, 2.3f, 3.4f, 4.45f, 5.15f, 6.0f, 7.24f })
            {
                Settings moving = settings;
                const u32 targetFrames = static_cast<u32>(std::ceil(time * 60.0f));
                moving.Frames = std::max(1u, targetFrames - playedFrames);
                playedFrames += moving.Frames;
                Capture frame;
                ASSERT_TRUE(CaptureFrame(moving, kHairMotion,
                                         prefix + "LookAround_" + std::to_string(std::lround(time * 100.0f)), frame));
                const auto pass = Renderer3D::GetGroomRenderPass();
                ASSERT_TRUE(pass);
                const GroomRenderStats& stats = pass->GetStats();
                EXPECT_EQ(stats.GroomsBindingRefused, 0u);
                EXPECT_EQ(stats.RootsHeldAtRest, 0u);
                EXPECT_GE(stats.GroomsDeformed, 1u);
                const auto* guides = GetScene().FindGroomGuideSimulation(m_Coat.GetUUID());
                ASSERT_TRUE(guides && guides->Initialized);
                ASSERT_EQ(guides->Curr.size(), guides->LastTargets.size());
                f32 worstDeviation = 0.0f;
                for (sizet i = 0; i < guides->Curr.size(); ++i)
                {
                    worstDeviation = std::max(worstDeviation, glm::length(guides->Curr[i] - guides->LastTargets[i]));
                }
                // The rest tips clear the shoulders by 24 mm. Retain a margin
                // through the authored motion; the old rope-soft setting let
                // the scalp arcs collapse by centimetres despite valid binding.
                EXPECT_LT(worstDeviation, 0.015f);
            }
        }
    }

    // THE ANIMATED SUBJECT: LookAround played from the rest pose, captured as it
    // plays (TAA on, the coat simulating), so the PNGs show what a viewer sees:
    // the turn to each side, the holds, the nod. The head must actually move.
    TEST_F(SkinRiggedHeadCloseUp, TheLookAroundClipTurnsTheHeadAndTheCoatFollows)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ShowHair(true);
        SetRig(Rig::Studio);
        auto& anim = m_Human.GetComponent<AnimationStateComponent>();
        anim.m_CurrentTime = 0.0f;
        anim.m_IsPlaying = true;

        // Clip seconds: rest, mid-turn, held right, swinging left, held left,
        // the nod down, the nod up.
        constexpr std::array<f32, 7> kTimes = { 0.05f, 0.45f, 1.5f, 2.3f, 3.4f, 4.45f, 5.15f };
        Capture rest;
        Capture held;
        f32 played = 0.0f;
        for (const f32 t : kTimes)
        {
            const u32 frames = std::max(1u, static_cast<u32>(std::lround((t - played) * 60.0f)));
            played += static_cast<f32>(frames) / 60.0f;
            Capture frame;
            ASSERT_TRUE(CaptureFrame(Settings{ RenderingPath::Forward, MaterialDebugView::None, /*Taa=*/true, frames },
                                     kFront, "SkinRiggedHead_GL_Forward_LookAround_" + std::to_string(std::lround(t * 100.0f)),
                                     frame));
            if (t < 0.1f)
                rest = frame;
            else if (t > 1.4f && t < 1.6f)
                held = frame;
        }
        Window face;
        ASSERT_TRUE(ProjectBox(kFront, glm::vec3(-0.08f, 0.22f, 0.0f), glm::vec3(0.08f, 0.40f, 0.14f), face));
        const WindowStats moved = Measure(held, face, &rest);
        std::printf("[RiggedHead LookAround] face window %llu px of %llu changed between rest and the held turn\n",
                    static_cast<unsigned long long>(moved.ChangedPixels), static_cast<unsigned long long>(face.Area()));
        EXPECT_GT(moved.ChangedPixels, face.Area() / 4u) << "the head did not turn: the clip is not driving the skin";
    }

    // THE STATIC REFERENCE AT THE MATCHING POSE. Head.fbx placed with exactly
    // the normalisation prepare_head_rig.py bakes into HeadRigged (scaled to a
    // 0.43 m bust, base centred on the origin), carrying DigitalHuman.olo's
    // material -- its thickness factor scaled by the same factor, since its map
    // was measured at 0.38 m per unit. The two silhouettes must coincide (the
    // rest pose IS the scan), and the ear must transmit comparably.
    TEST_F(SkinRiggedHeadCloseUp, TheRestPoseMatchesTheStaticReferenceCranium)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ShowHair(false);

        std::array<Capture, 2> rigged;
        const std::array<Pose, 2> poses = { kFront, kBacklitEar };
        for (u32 i = 0; i < 2u; ++i)
        {
            SetRig(i == 0u ? Rig::Studio : Rig::Backlit);
            ASSERT_TRUE(CaptureFrame(Settings{ RenderingPath::Deferred }, poses[i],
                                     std::string("SkinRiggedHeadRest_GL_Deferred_") + poses[i].Name, rigged[i]));
        }
        // The transmitted term alone: the composites differ in albedo (the rig
        // carries the scan's texture, the reference a flat colour), and skin
        // albedo is red-dominant too, so only the AOV compares the two ears.
        const Settings aov{ RenderingPath::Deferred, MaterialDebugView::Transmission };
        Capture riggedTransmission;
        ASSERT_TRUE(
            CaptureFrame(aov, kBacklitEar, "SkinRiggedHeadRestTransmission_GL_Deferred_BacklitEar", riggedTransmission));

        GetScene().DestroyEntity(m_Coat);
        GetScene().DestroyEntity(m_Human);
        m_Coat = {};
        m_Human = {};
        std::array<Capture, 2> empty;
        for (u32 i = 0; i < 2u; ++i)
        {
            SetRig(i == 0u ? Rig::Studio : Rig::Backlit);
            ASSERT_TRUE(CaptureFrame(Settings{ RenderingPath::Deferred }, poses[i], {}, empty[i]));
        }

        const fs::path fbx = HeadDir() / "Head.fbx";
        auto model = Ref<Model>::Create(fbx.string());
        ASSERT_TRUE(model && model->GetMeshCount() > 0);
        const BoundingBox box = model->GetBoundingBox();
        const f32 scale = 0.43f / (box.Max.y - box.Min.y);
        const glm::vec3 base{ 0.5f * (box.Min.x + box.Max.x), box.Min.y, 0.5f * (box.Min.z + box.Max.z) };
        Ref<Texture2D> map = Texture2D::Create(
            (ProjectRoot() / "Assets" / "Textures" / "InfiniteScanHead_Thickness.png").string(), /*srgb=*/false);
        ASSERT_TRUE(map && map->IsLoaded());

        Entity cranium = GetScene().CreateEntity("Cranium");
        auto& tc = cranium.GetComponent<TransformComponent>();
        tc.Scale = glm::vec3(scale);
        tc.Translation = kHumanPosition - scale * base;
        cranium.AddComponent<ModelComponent>(model, fbx.string());
        auto& material = cranium.AddComponent<MaterialComponent>().m_Material;
        // DigitalHuman.olo's Cranium, field for field, except the factor.
        material.SetBaseColorFactor({ 0.66f, 0.49f, 0.42f, 1.0f });
        material.SetMetallicFactor(0.0f);
        material.SetRoughnessFactor(0.42f);
        material.SetMaterialKind(MaterialKind::Skin);
        material.SetSkinProfileHandle(m_Profile);
        material.SetThicknessFactor(kThicknessFactorMetres * scale / 0.38f);
        material.SetThicknessMap(map);
        std::printf("[RiggedHead cranium] scale %.5f (rig/scene ratio %.4f), thickness factor %.5f\n", scale, scale / 0.38f,
                    material.GetThicknessFactor());

        std::array<Capture, 2> reference;
        for (u32 i = 0; i < 2u; ++i)
        {
            SetRig(i == 0u ? Rig::Studio : Rig::Backlit);
            ASSERT_TRUE(CaptureFrame(Settings{ RenderingPath::Deferred }, poses[i],
                                     std::string("SkinRiggedHeadCranium_GL_Deferred_") + poses[i].Name, reference[i]));
        }
        SetRig(Rig::Backlit);
        Capture referenceTransmission;
        ASSERT_TRUE(CaptureFrame(aov, kBacklitEar, "SkinRiggedHeadCraniumTransmission_GL_Deferred_BacklitEar",
                                 referenceTransmission));

        for (u32 i = 0; i < 2u; ++i)
        {
            const std::vector<u8> a = Coverage(rigged[i], empty[i]);
            const std::vector<u8> b = Coverage(reference[i], empty[i]);
            u64 both = 0;
            u64 either = 0;
            for (sizet k = 0; k < a.size(); ++k)
            {
                both += (a[k] & b[k]) ? 1u : 0u;
                either += (a[k] | b[k]) ? 1u : 0u;
            }
            const f64 iou = either > 0 ? static_cast<f64>(both) / static_cast<f64>(either) : 0.0;
            std::printf("[RiggedHead vs cranium %s] silhouette IoU %.4f (%llu px covered)\n", poses[i].Name, iou,
                        static_cast<unsigned long long>(either));
            EXPECT_GT(iou, 0.95) << poses[i].Name << ": the rest pose is not the scan's pose";
        }

        Window ear;
        ASSERT_TRUE(ProjectBox(kBacklitEar, kRightEarMin, kRightEarMax, ear));
        const WindowStats rig = Measure(riggedTransmission, ear);
        const WindowStats ref = Measure(referenceTransmission, ear);
        std::printf("[RiggedHead vs cranium] transmission AOV at the ear: red-dominant %llu vs %llu of %llu, mean red "
                    "%.4f vs %.4f\n",
                    static_cast<unsigned long long>(rig.RedPixels), static_cast<unsigned long long>(ref.RedPixels),
                    static_cast<unsigned long long>(ear.Area()), rig.MeanRed, ref.MeanRed);
        EXPECT_GT(rig.RedPixels, ear.Area() / 200u) << "the rigged ear does not transmit";
        EXPECT_GT(rig.RedPixels * 3u, ref.RedPixels) << "the rigged ear transmits far less than the reference's";
        EXPECT_GT(ref.RedPixels * 3u, rig.RedPixels) << "the rigged ear transmits far more than the reference's";
    }

    // THE ACCEPTANCE SCENE'S PLACEMENT. There the bust stands at
    // [-34, 1.24, 196], turned -2.2143 rad about +Y, and HumanCoat is PARENTED to
    // Human with an identity local transform -- while the cooked binding was
    // built with the body at [0, 1.24, 3.6] and the coat at the origin. A binding
    // is surface-relative: the Scene maps the body into the groom's space every
    // frame (inverse(groomWorld) * bodyWorld), so a moved, turned, re-parented
    // pair must bind, deform every root, and draw the same coat on the same
    // scalp. Measured as the hair's coverage of the frame from a camera placed
    // identically relative to the head, at both placements.
    TEST_F(SkinRiggedHeadCloseUp, TheCoatBindsAndFollowsAtTheAcceptanceScenePlacement)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        SetRig(Rig::Studio);

        struct Arm
        {
            std::vector<u8> Hair; // 1 where the coat changes the frame
            u64 HairPixels = 0;
            u64 HairInsideHead = 0;
            u32 MinDeformed = 0xFFFFFFFFu;
            u32 MaxRefused = 0;
            u32 MaxHeldAtRest = 0;
            u32 StrandsDrawn = 0; // the last frame's: what the coat's LOD bought
        };
        const auto measure = [this](const char* name, Arm& arm)
        {
            ShowHair(true);
            // The camera first: the coat's LOD answers to it, and a camera left
            // where the body USED to be measures a coat 200 m away.
            PoseCamera(kThreeQuarter);
            for (u32 f = 0; f < 48u; ++f)
            {
                RunFrames(1, 1.0f / 60.0f);
                const auto* pass = Renderer3D::GetGroomRenderPass();
                ASSERT_NE(pass, nullptr);
                const GroomRenderStats& stats = pass->GetStats();
                arm.MinDeformed = std::min(arm.MinDeformed, stats.GroomsDeformed);
                arm.MaxRefused = std::max(arm.MaxRefused, stats.GroomsBindingRefused);
                arm.MaxHeldAtRest = std::max(arm.MaxHeldAtRest, stats.RootsHeldAtRest);
                arm.StrandsDrawn = stats.StrandsDrawn;
            }
            const Settings s{ RenderingPath::Forward, MaterialDebugView::None, /*Taa=*/true, /*Frames=*/48 };
            Capture with;
            Capture without;
            ASSERT_TRUE(CaptureFrame(s, kThreeQuarter, std::string("SkinRiggedHeadPlacement_GL_Forward_") + name, with));
            ShowHair(false);
            ASSERT_TRUE(CaptureFrame(s, kThreeQuarter, {}, without));
            ShowHair(true);
            arm.Hair = Coverage(with, without);
            Window head;
            ASSERT_TRUE(ProjectBox(kThreeQuarter, glm::vec3(-0.16f, 0.05f, -0.18f), glm::vec3(0.16f, 0.47f, 0.20f), head));
            for (u32 y = 0; y < kSize; ++y)
            {
                for (u32 x = 0; x < kSize; ++x)
                {
                    if (arm.Hair[static_cast<sizet>(y) * kSize + x] == 0u)
                        continue;
                    ++arm.HairPixels;
                    if (x >= head.X0 && x <= head.X1 && y >= head.Y0 && y <= head.Y1)
                        ++arm.HairInsideHead;
                }
            }
        };

        Arm cooked;
        ASSERT_NO_FATAL_FAILURE(measure("Cooked", cooked));

        constexpr f32 kAcceptanceYaw = -2.2143f;
        m_Coat.SetParent(m_Human);
        auto& coatTransform = m_Coat.GetComponent<TransformComponent>();
        coatTransform.Translation = glm::vec3(0.0f);
        coatTransform.SetRotationEuler(glm::vec3(0.0f));
        coatTransform.Scale = glm::vec3(1.0f);
        PlaceBody(glm::vec3(-34.0f, 1.24f, 196.0f), kAcceptanceYaw);
        Arm acceptance;
        ASSERT_NO_FATAL_FAILURE(measure("Acceptance", acceptance));

        // Coverage compared in 16-pixel cells: the coat is stochastic, so
        // per-pixel masks differ by dither even for one placement.
        constexpr u32 kCell = 16;
        f64 cellError = 0.0;
        u32 cells = 0;
        for (u32 cy = 0; cy < kSize / kCell; ++cy)
        {
            for (u32 cx = 0; cx < kSize / kCell; ++cx)
            {
                u32 a = 0;
                u32 b = 0;
                for (u32 y = cy * kCell; y < (cy + 1u) * kCell; ++y)
                {
                    for (u32 x = cx * kCell; x < (cx + 1u) * kCell; ++x)
                    {
                        a += cooked.Hair[static_cast<sizet>(y) * kSize + x];
                        b += acceptance.Hair[static_cast<sizet>(y) * kSize + x];
                    }
                }
                if (a == 0u && b == 0u)
                    continue;
                cellError += std::abs(static_cast<f64>(a) - static_cast<f64>(b)) / static_cast<f64>(kCell * kCell);
                ++cells;
            }
        }
        const f64 meanCellError = cells > 0 ? cellError / cells : 1.0;
        std::printf("[RiggedHead placement] cooked: %llu hair px (%.3f inside the head box), deformed>=%u refused<=%u "
                    "held<=%u; acceptance: %llu hair px (%.3f inside), deformed>=%u refused<=%u held<=%u; mean cell "
                    "coverage difference %.4f over %u cells\n",
                    static_cast<unsigned long long>(cooked.HairPixels),
                    static_cast<f64>(cooked.HairInsideHead) / std::max<f64>(1.0, static_cast<f64>(cooked.HairPixels)),
                    cooked.MinDeformed, cooked.MaxRefused, cooked.MaxHeldAtRest,
                    static_cast<unsigned long long>(acceptance.HairPixels),
                    static_cast<f64>(acceptance.HairInsideHead) /
                        std::max<f64>(1.0, static_cast<f64>(acceptance.HairPixels)),
                    acceptance.MinDeformed, acceptance.MaxRefused, acceptance.MaxHeldAtRest, meanCellError, cells);

        for (const Arm* arm : { &cooked, &acceptance })
        {
            const char* which = arm == &cooked ? "the cooked placement" : "the acceptance placement";
            EXPECT_EQ(arm->MaxRefused, 0u) << which << ": the binding was refused";
            EXPECT_GE(arm->MinDeformed, 1u) << which << ": the coat was not deformed by its body";
            EXPECT_EQ(arm->MaxHeldAtRest, 0u) << which << ": roots were held at rest instead of following the scalp";
            EXPECT_GT(arm->HairPixels, static_cast<u64>(kSize) * kSize / 20u) << which << ": the coat is not in the frame";
            EXPECT_GT(static_cast<f64>(arm->HairInsideHead), 0.9 * static_cast<f64>(arm->HairPixels))
                << which << ": the coat is drawn away from the head it is bound to";
        }
        EXPECT_NEAR(static_cast<f64>(acceptance.HairPixels) / static_cast<f64>(std::max<u64>(1u, cooked.HairPixels)), 1.0, 0.15)
            << "the coat covers a different share of the frame once the body is moved, turned and parented";
        // 0.11 before the coat's LOD measured the coat where the body puts it
        // (the rest bounds read it as 3.7 m away and thinned it), 0.013 after.
        EXPECT_LT(meanCellError, 0.05) << "the coat lands in a different place relative to the head";
        // The same coat, the same camera relative to the head: the LOD must buy
        // the same strands. It is measured from where the coat IS, and a coat
        // measured at its cook-time placement reads as metres away once the
        // body moves, and thins out under the camera.
        std::printf("[RiggedHead placement] strands drawn: cooked %u, acceptance %u\n", cooked.StrandsDrawn,
                    acceptance.StrandsDrawn);
        EXPECT_NEAR(static_cast<f64>(acceptance.StrandsDrawn), static_cast<f64>(cooked.StrandsDrawn),
                    0.02 * static_cast<f64>(cooked.StrandsDrawn))
            << "the coat's LOD sees the moved body's coat at a different size than the same coat at its cook placement";
    }
} // namespace OloEngine::Tests
