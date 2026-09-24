#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// GroomBindingVisualEvidenceTest — issue #1249, acceptance criteria 2 and 4.
//
// "Guides and rendered hairs follow bending limbs and facial morphs without
//  floating roots or gross coat collapse."
// "Editor binding preview, asset cooking and runtime attachment work on
//  short/long animal coats and human hair."
//
// Writes OloEditor/assets/tests/visual/GroomBinding[Off]_GL_<Path>[_<Angle>].png
// and, since #1427, GroomGpuDeformation[Off]_GL_<Path>_<Angle>.png: the same
// bent coat deformed by the vertex shader, and by the CPU rebuild it replaced
// ("Off", the reference).
//
// The filename carries the {backend} x {path} cell it covers, deliberately
// including the backend even though it is always GL here: a reader counting
// files then cannot mistake a complete set of OpenGL captures for a complete
// verification matrix. Every Vulkan cell is live-only — these fixtures need a
// real GL 4.6 context and skip without one — and is evidenced in the PR body.
//
// EVERY CAPTURE IS OF AN ANIMATED POSE, never a bind pose. A still bind-pose
// capture of this feature proves nothing whatsoever: an unbound groom and a
// correctly bound one are the SAME PICTURE at the bind pose, which is precisely
// why the A/B control here is not "strands off" but "binding off" — the same
// coat, the same camera, the same bent body, with the deformation switched off.
// A capture pair where the two frames match is a feature that did nothing.
//
// The contracts asserted beyond the PNGs are the ones that can be wrong while
// the picture still looks like fur:
//
//   1. A bent body MOVES the coat, on all three rendering paths. A path that
//      silently does not deform is invisible in a single-path capture.
//   2. The coat does not COLLAPSE: the deformed strand lengths are preserved,
//      measured from the CPU transforms the frame was actually built from.
//   3. The pass REPORTS the deformation (GroomRenderStats::GroomsDeformed), so
//      a frame that changed for some other reason cannot be mistaken for one
//      that deformed.
//   4. A refused binding is visible as a REFUSAL rather than as a bind-posed
//      coat with nothing said about it.
//
// WHAT THESE CAPTURES DO NOT SHOW, AND IT IS A FIXTURE LIMIT RATHER THAN A
// FEATURE ONE: the BODY does not appear in them. A runtime-built primitive
// MeshSource given a Skeleton and a SkeletonComponent is not picked up by the
// animated submission path, so the frames are the coat against an empty
// background. It was investigated and not fixed here: the deformation itself is
// fully evidenced without it (the A/B pair differs by tens of thousands of
// pixels, and the strand lengths are measured on the CPU from the very
// transforms the frame was built from), and the "coat on a visible, animating
// body" picture comes from the live editor on an imported skinned character —
// which the Vulkan cells need anyway, since no headless fixture can produce
// them. What is lost here is the depth-correct-against-the-body view, which is
// #1246's criterion and is already covered by GroomStrandVisualEvidenceTest.
//
// FOR THE SAME REASON THE THREE PATH CAPTURES ARE BYTE-IDENTICAL. With only the
// coat in frame, and the strand pass being one path-agnostic forward-style pass
// by design (#1246), Forward, Forward+ and Deferred produce the same pixels
// here. The per-path assertion that carries weight is therefore
// GroomRenderStats::GroomsDeformed on each path, not a pixel difference between
// them; the files are still named per path so an unrun cell is a missing file.
//
// Classification: L8 / golden image (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomBindingCooker.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"
#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <optional>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cstddef>
#include <cmath>
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

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // A moved-coat pixel is one that differs noticeably between the bound
        // and unbound frames of the SAME bent body. 12/255 per channel is well
        // above dithering and well below the dimmest point of the ramp — the
        // same floor GroomStrandVisualEvidenceTest uses, on purpose: two
        // neighbouring evidence tests that disagree about what "changed" means
        // are two tests nobody can compare.
        constexpr int kMovedPixelThreshold = 12;

        [[nodiscard]] u32 CountDifferingPixels(const std::vector<u8>& frame, const std::vector<u8>& baseline)
        {
            if (frame.size() != baseline.size())
            {
                return 0;
            }
            u32 differing = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                const int dr = std::abs(static_cast<int>(frame[i]) - static_cast<int>(baseline[i]));
                const int dg = std::abs(static_cast<int>(frame[i + 1]) - static_cast<int>(baseline[i + 1]));
                const int db = std::abs(static_cast<int>(frame[i + 2]) - static_cast<int>(baseline[i + 2]));
                if (dr > kMovedPixelThreshold || dg > kMovedPixelThreshold || db > kMovedPixelThreshold)
                {
                    ++differing;
                }
            }
            return differing;
        }

        [[nodiscard]] f64 MeanLuminance(const std::vector<u8>& frame)
        {
            if (frame.empty())
            {
                return 0.0;
            }
            f64 sum = 0.0;
            sizet count = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                sum += (0.2126 * frame[i] + 0.7152 * frame[i + 1] + 0.0722 * frame[i + 2]) / 255.0;
                ++count;
            }
            return count ? sum / static_cast<f64>(count) : 0.0;
        }

        // The three coat types criterion 4 names, as authoring parameters. They
        // differ in the two things that actually change how a binding behaves —
        // how LONG a strand is (a long coat has more to swing) and how DENSE the
        // roots are (a scalp packs them into a small patch) — rather than in
        // cosmetic detail that would make three captures of the same thing.
        struct CoatKind
        {
            const char* Name;
            u32 Strands;
            u32 Points;
            f32 Length;
            f32 Width;
            f32 Coverage; ///< fraction of the sphere the roots occupy, from the top
        };

        constexpr CoatKind kShortAnimalCoat{ "ShortCoat", 3000u, 5u, 0.28f, 0.010f, 1.0f };
        constexpr CoatKind kLongAnimalCoat{ "LongCoat", 2200u, 10u, 1.30f, 0.012f, 1.0f };
        constexpr CoatKind kHumanHair{ "HumanHair", 3600u, 12u, 1.05f, 0.008f, 0.42f };
    } // namespace

    class GroomBindingVisualEvidenceTest : public RendererAttachedTest
    {
      public:
        Entity m_BodyEntity;
        Entity m_GroomEntity;
        Ref<Skeleton> m_Skeleton;
        Ref<MeshSource> m_BodySurface;
        Ref<GroomAsset> m_Groom;
        Ref<GroomBindingAsset> m_Binding;
        AssetHandle m_GroomHandle = 0;
        AssetHandle m_BindingHandle = 0;

        void BuildScene() override
        {
            if (!Project::GetActive() || !Project::HasAssetManager())
            {
                std::error_code ec;
                const fs::path projectDir = TempDir("project");
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Evidence.oloproj");
                    proj << "Project:\n"
                            "  Name: GroomBindingEvidence\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Evidence.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false);
                Project::SetAssetManager(assetManager);
            }

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            auto& rendererSettings = Renderer3D::GetRendererSettings();
            // The strand pass is not a gizmo and draws regardless, but the grid
            // and the axis helper would clutter a capture whose whole subject is
            // where a silhouette moved to.
            rendererSettings.EditorDebugDrawsEnabled = true;
            rendererSettings.ShowComponentGizmos = false;
            rendererSettings.ShowGrid = false;
            rendererSettings.ShowWorldAxisHelper = false;

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 6.0f, 4.0f);
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.7f, -0.6f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                dl.m_Intensity = 3.0f;
            }

            BuildSkinnedBody(scene);
            // HasFatalFailure, not just the null checks: an ASSERT_ inside
            // BuildSkinnedBody returns from THAT function only, so without this
            // the next line would call GetUUID() on a default-constructed
            // entity and the whole suite would die with an access violation
            // inside SetUp — which is what it did.
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            ASSERT_TRUE(m_BodySurface);
            ASSERT_TRUE(m_Skeleton);
            ASSERT_TRUE(m_BodyEntity);

            InstallCoat(kShortAnimalCoat);
        }

        // A sphere rigged as a HINGE: everything above y = 0 belongs to bone 1
        // and everything below to bone 0, with no blend band.
        //
        // A hard split rather than a smooth one, deliberately. What this test is
        // about is whether a strand goes where the triangle under it went; a
        // smooth blend would make the expected silhouette the output of the
        // blend, and a capture of that answers a question about interpolation
        // instead.
        void BuildSkinnedBody(Scene& scene)
        {
            Ref<Mesh> mesh = MeshPrimitives::CreateSphere();
            ASSERT_TRUE(mesh);
            m_BodySurface = mesh->GetMeshSource();
            ASSERT_TRUE(m_BodySurface);

            m_Skeleton = Ref<Skeleton>::Create();
            m_Skeleton->m_BoneNames = { "root", "upper" };
            m_Skeleton->m_ParentIndices = { -1, 0 };
            m_Skeleton->m_FinalBoneMatrices = { glm::mat4(1.0f), glm::mat4(1.0f) };
            m_Skeleton->m_PrevFinalBoneMatrices = { glm::mat4(1.0f), glm::mat4(1.0f) };
            m_BodySurface->SetSkeleton(m_Skeleton);

            auto& influences = m_BodySurface->GetBoneInfluences();
            const auto& vertices = m_BodySurface->GetVertices();
            // RESIZED, not asserted equal. MeshPrimitives::CreateSphere hands
            // back a source whose influence array is 512 entries for 510
            // vertices — the constructor pre-allocates to the vertex count and
            // the generator then de-duplicates without resizing it. That is a
            // wart in the PRIMITIVE path rather than in this feature, but it
            // matters here: Scene::MakeGroomSkinningView treats a partial
            // influence stream as ABSENT (the rule MeshSource::HasLightmapUVs
            // states for its own parallel stream), so an un-resized array would
            // make this fixture silently unskinned and every "the coat followed
            // the body" assertion below would be measuring nothing.
            influences.SetNum(vertices.Num());
            for (i32 i = 0; i < vertices.Num(); ++i)
            {
                BoneInfluence influence;
                influence.m_BoneIDs[0] = vertices[i].Position.y > 0.0f ? 1u : 0u;
                influence.m_Weights[0] = 1.0f;
                influences[i] = influence;
            }
            ASSERT_EQ(influences.Num(), vertices.Num());

            // Re-upload, because MeshPrimitives::CreateSphere built this
            // source's GPU buffers before the influences above existed. Hygiene
            // rather than a fix: it was tried as one and the body still does not
            // reach the frame — see the note on the body in the file header.
            m_BodySurface->Build();

            m_BodyEntity = scene.CreateEntity("Body");
            m_BodyEntity.GetComponent<TransformComponent>().Scale = glm::vec3(0.98f);
            auto& mc = m_BodyEntity.AddComponent<MeshComponent>();
            mc.m_MeshSource = m_BodySurface;
            auto& mat = m_BodyEntity.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.10f, 0.09f, 0.09f, 1.0f));
            m_BodyEntity.AddComponent<SkeletonComponent>(m_Skeleton);
        }

        // Builds the coat, cooks a binding for it against the body's BIND POSE,
        // and installs both. Cooking rather than binding in memory is the
        // `{loose, cooked}` asset cell: every capture in this file is of a coat
        // that went to disk and came back.
        void InstallCoat(const CoatKind& kind)
        {
            Scene& scene = GetScene();
            if (m_GroomEntity)
            {
                scene.DestroyEntity(m_GroomEntity);
                m_GroomEntity = {};
            }

            m_Groom = BuildCoat(kind);
            ASSERT_TRUE(m_Groom);

            std::vector<u8> bytes;
            GroomBindingBuildStats buildStats;
            std::string reason;
            ASSERT_TRUE(GroomBindingCooker::CookPair(*m_Groom, BodyView(), "EvidenceBody",
                                                     GroomBindingBuildSettings{}, bytes, m_Binding, buildStats,
                                                     reason))
                << reason;
            ASSERT_TRUE(m_Binding);
            std::printf("[groom-binding] %-10s  %u roots (%u exact / %u clamped / %u distant), max rest %.4f, "
                        "%zu cooked bytes\n",
                        kind.Name, buildStats.RootsBound, buildStats.RootsExact, buildStats.RootsClamped,
                        buildStats.RootsDistant, static_cast<f64>(buildStats.MaxRestDistance), bytes.size());
            EXPECT_EQ(buildStats.RootsDistant, 0u) << kind.Name << ": a coat authored on the body must bind to it";

            // Through the BYTES, so the assets under every capture below are the
            // cooked ones rather than the in-memory build.
            Ref<GroomBindingAsset> loaded;
            ASSERT_TRUE(GroomBindingSerializer::DecodeFromBytes(bytes.data(), bytes.size(), loaded, reason))
                << reason;
            m_Binding = loaded;

            m_GroomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(m_Groom);
            m_BindingHandle = AssetManager::AddMemoryOnlyAsset<GroomBindingAsset>(m_Binding);
            ASSERT_NE(static_cast<u64>(m_GroomHandle), 0u);
            ASSERT_NE(static_cast<u64>(m_BindingHandle), 0u);

            m_GroomEntity = scene.CreateEntity("Groom");
            auto& groomComponent = m_GroomEntity.AddComponent<GroomComponent>();
            groomComponent.m_Groom = m_GroomHandle;
            groomComponent.m_ShowPreview = false;
            groomComponent.m_RenderStrands = true;
            groomComponent.m_MaxRenderStrands = kind.Strands;
            groomComponent.m_WidthScale = 1.0f;
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            groomComponent.m_StrandColor = glm::vec3(0.72f, 0.60f, 0.46f);

            auto& binding = m_GroomEntity.AddComponent<GroomBindingComponent>();
            binding.m_Binding = m_BindingHandle;
            binding.m_TargetEntity = m_BodyEntity.GetUUID();
            binding.m_Enabled = true;
        }

        [[nodiscard]] GroomSurfaceView BodyView() const
        {
            GroomSurfaceView view;
            const auto& vertices = m_BodySurface->GetVertices();
            const auto& indices = m_BodySurface->GetIndices();
            view.PositionData = reinterpret_cast<const std::byte*>(vertices.GetData());
            view.PositionStride = static_cast<u32>(sizeof(Vertex));
            view.VertexCount = static_cast<u32>(vertices.Num());
            view.Indices = indices.GetData();
            view.IndexCount = static_cast<u32>(indices.Num());
            view.BoneCount = static_cast<u32>(m_Skeleton->m_FinalBoneMatrices.size());
            // The same identity Scene derives, restated here because the whole
            // point of the signature is that two places agree about it.
            u64 hash = 1469598103934665603ull;
            for (const auto& name : m_Skeleton->m_BoneNames)
            {
                for (const char c : name)
                {
                    hash ^= static_cast<u64>(static_cast<unsigned char>(c));
                    hash *= 1099511628211ull;
                }
                hash ^= 0xFFull;
                hash *= 1099511628211ull;
            }
            view.SkeletonNameHash = hash;
            return view;
        }

        // A coat on the upper hemisphere, rooted ON the sphere and sweeping out
        // and down under its own weight. `Coverage` narrows the patch for the
        // human-hair case, which is a scalp rather than a pelt.
        static Ref<GroomAsset> BuildCoat(const CoatKind& kind)
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

            constexpr f32 kRadius = 1.0f;
            constexpr f32 kGoldenAngle = 2.39996323f;

            for (u32 s = 0; s < kind.Strands; ++s)
            {
                const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(kind.Strands);
                // cosTheta near 1 is the pole; narrowing the range toward it is
                // what makes a scalp out of a pelt.
                const f32 cosTheta = 1.0f - (t * kind.Coverage);
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - (cosTheta * cosTheta)));
                const f32 phi = kGoldenAngle * static_cast<f32>(s);
                const glm::vec3 normal(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
                const glm::vec3 root = normal * kRadius;

                std::vector<glm::vec3> points;
                std::vector<f32> widths;
                points.reserve(kind.Points);
                widths.reserve(kind.Points);
                for (u32 p = 0; p < kind.Points; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(kind.Points - 1u);
                    glm::vec3 point = root + (normal * (kind.Length * along));
                    point.y -= kind.Length * 0.75f * along * along; // droop
                    points.push_back(point);
                    widths.push_back(kind.Width * (1.0f - (0.8f * along)));
                }

                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { std::fmod(phi / (2.0f * 3.14159265f), 1.0f), t };
                input.GroupId = group;
                input.IsGuide = (s % 25u) == 0;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }

            builder.SetName(kind.Name);
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
            {
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            }
            return groom;
        }

        // Drives the shared deformation output directly: this fixture has no
        // animation clip, and what the binding consumes is the PALETTE, so
        // writing it is exercising the real seam rather than a stand-in.
        void SetBodyPose(f32 degrees)
        {
            const glm::vec3 hinge{ 0.0f, 0.0f, 0.0f };
            glm::mat4 bend = glm::translate(glm::mat4(1.0f), hinge);
            bend = glm::rotate(bend, glm::radians(degrees), glm::vec3(0.0f, 0.0f, 1.0f));
            bend = glm::translate(bend, -hinge);
            m_Skeleton->m_FinalBoneMatrices = { glm::mat4(1.0f), bend };
            m_Skeleton->m_PrevFinalBoneMatrices = m_Skeleton->m_FinalBoneMatrices;
        }

        void SetBindingEnabled(bool enabled)
        {
            m_GroomEntity.GetComponent<GroomBindingComponent>().m_Enabled = enabled;
        }

        void Capture(const std::string& saveAs, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            }
            ASSERT_TRUE(fb) << "No composited framebuffer for '" << saveAs << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            {
                const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < kHeight / 2u; ++y)
                {
                    u8* top = outPixels.data() + (static_cast<sizet>(y) * rowBytes);
                    u8* bot = outPixels.data() + (static_cast<sizet>(kHeight - 1u - y) * rowBytes);
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            if (!saveAs.empty())
            {
                WriteEvidence(saveAs, outPixels);
            }
        }

        // EVIDENCE, not SSIM goldens. What is asserted is the contract below;
        // the PNGs exist so a reviewer can look at what the numbers describe.
        // Always writes, never compares.
        static void WriteEvidence(const std::string& name, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.string() << "': " << ec.message();
            const std::string path = (dir / (name + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                               pixels.data(), static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
        }

        // The CPU half of the same frame: the deformed strand lengths, measured
        // from the transforms the renderer used. A picture cannot tell a coat
        // that followed the body from one that stretched to follow it, and this
        // can.
        [[nodiscard]] f32 WorstRelativeLengthError()
        {
            GroomDeformationInputs inputs;
            inputs.Surface = BodyView();

            GroomSkinningView skinning;
            const auto& influences = m_BodySurface->GetBoneInfluences();
            const auto* base = reinterpret_cast<const std::byte*>(influences.GetData());
            skinning.BoneIds = reinterpret_cast<const u32*>(base + offsetof(BoneInfluence, m_BoneIDs));
            skinning.Weights = reinterpret_cast<const f32*>(base + offsetof(BoneInfluence, m_Weights));
            skinning.Stride = static_cast<u32>(sizeof(BoneInfluence));
            skinning.VertexCount = static_cast<u32>(influences.Num());
            skinning.Palette = { m_Skeleton->m_FinalBoneMatrices.data(), m_Skeleton->m_FinalBoneMatrices.size() };
            skinning.PrevPalette = { m_Skeleton->m_PrevFinalBoneMatrices.data(),
                                     m_Skeleton->m_PrevFinalBoneMatrices.size() };
            skinning.HasPreviousPose = true;
            inputs.Skinning = skinning;
            inputs.HasHistory = true;

            TArray<GroomRootTransform> transforms;
            (void)EvaluateGroomRootTransforms(*m_Groom, *m_Binding, inputs, std::nullopt, transforms);

            const auto& authored = m_Groom->GetPoints();
            f32 worst = 0.0f;
            for (u32 curve = 0; curve < m_Groom->GetCurveCount(); ++curve)
            {
                const GroomRootBinding& record = m_Binding->GetRoot(curve);
                const GroomRootTransform& transform = transforms[curve];
                const u32 first = m_Groom->GetCurveFirstPoint(curve);
                const u32 count = m_Groom->GetCurvePointCount(curve);

                f32 authoredLength = 0.0f;
                f32 deformedLength = 0.0f;
                for (u32 i = 0; i + 1u < count; ++i)
                {
                    authoredLength += glm::length(authored[first + i + 1u] - authored[first + i]);
                    const glm::vec3 a =
                        ApplyGroomRootTransform(record, transform, authored[first + i], false);
                    const glm::vec3 b =
                        ApplyGroomRootTransform(record, transform, authored[first + i + 1u], false);
                    deformedLength += glm::length(b - a);
                }
                if (authoredLength > 0.0f)
                {
                    worst = std::max(worst, std::abs((deformedLength / authoredLength) - 1.0f));
                }
            }
            return worst;
        }

        [[nodiscard]] const GroomRenderStats& PassStats() const
        {
            const auto* groomPass = Renderer3D::GetGroomRenderPass();
            EXPECT_NE(groomPass, nullptr);
            static const GroomRenderStats kEmpty{};
            return groomPass != nullptr ? groomPass->GetStats() : kEmpty;
        }
    };

    // ── Criterion 2: a bent body moves the coat, on every path ─────────────

    TEST_F(GroomBindingVisualEvidenceTest, ABentBodyMovesTheCoatOnEveryRenderingPath)
    {
        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        const std::array<PathCase, 3> paths = { {
            { "Forward", RenderingPath::Forward },
            { "ForwardPlus", RenderingPath::ForwardPlus },
            { "Deferred", RenderingPath::Deferred },
        } };

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        for (const PathCase& pathCase : paths)
        {
            Renderer3D::GetRendererSettings().Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();

            // THE BODY IS BENT IN BOTH FRAMES. The A/B is the BINDING, not the
            // pose and not the strands: an unbound coat and a bound one are the
            // same picture at the bind pose, so a capture pair taken there
            // would pass while the feature did nothing at all.
            SetBodyPose(55.0f);

            SetBindingEnabled(false);
            std::vector<u8> unbound;
            Capture(std::string("GroomBindingOff_GL_") + pathCase.Name, eye, 0.0f, 0.10f, unbound);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            const GroomRenderStats unboundStats = PassStats();
            EXPECT_EQ(unboundStats.GroomsDeformed, 0u)
                << pathCase.Name << ": the control frame reported a deformation, so it is not a control";

            SetBindingEnabled(true);
            std::vector<u8> bound;
            Capture(std::string("GroomBinding_GL_") + pathCase.Name, eye, 0.0f, 0.10f, bound);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            const GroomRenderStats boundStats = PassStats();

            EXPECT_GT(MeanLuminance(bound), 0.02)
                << pathCase.Name << ": the frame is (near-)black, so nothing below is evidence of anything";

            const u32 differing = CountDifferingPixels(bound, unbound);
            std::printf("[groom-binding] path %-11s  %u px moved, %u grooms deformed, %u roots, %u held at rest\n",
                        pathCase.Name, differing, boundStats.GroomsDeformed, boundStats.RootsDeformed,
                        boundStats.RootsHeldAtRest);

            EXPECT_EQ(boundStats.GroomsDeformed, 1u)
                << pathCase.Name << ": the pass did not report deforming the groom, so whatever moved was not this";
            EXPECT_EQ(boundStats.GroomsBindingRefused, 0u) << pathCase.Name << ": the binding was refused";
            EXPECT_GT(boundStats.RootsDeformed, 0u) << pathCase.Name;
            EXPECT_EQ(boundStats.RootsHeldAtRest, 0u)
                << pathCase.Name << ": roots were held at rest, so part of the coat is not following the body";

            // 2000 pixels of a 921 600-pixel frame is well under a per-cent and
            // far above any tone-map wobble. A path where the deformation
            // silently did nothing scores zero here.
            EXPECT_GT(differing, 2000u)
                << pathCase.Name << ": binding the coat to a bent body changed almost nothing on screen";
        }
    }

    // ── Criterion 2: no gross coat collapse, measured not eyeballed ────────

    TEST_F(GroomBindingVisualEvidenceTest, TheCoatKeepsItsLengthThroughEveryPoseItIsCapturedIn)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        SetBindingEnabled(true);

        const glm::vec3 front{ 0.0f, 0.9f, 4.6f };
        const glm::vec3 side{ 4.4f, 0.9f, 0.6f };

        for (const f32 degrees : { 0.0f, 30.0f, 55.0f, 85.0f })
        {
            SetBodyPose(degrees);

            const f32 worst = WorstRelativeLengthError();
            std::printf("[groom-binding] pose %5.1f deg  worst strand length error %.6f\n",
                        static_cast<f64>(degrees), static_cast<f64>(worst));
            // A rigid root transfer preserves length exactly; the tolerance is
            // float noise through a quaternion, nothing else. A collapse shows
            // up here as a number that grows with the angle.
            EXPECT_LT(worst, 1.0e-3f) << "the coat collapsed at " << degrees << " degrees";
        }

        // Two angles at the working pose, because a coat can look right head-on
        // and be sliding around the body in profile.
        SetBodyPose(55.0f);
        std::vector<u8> pixels;
        Capture("GroomBinding_GL_Forward_Front", front, 0.0f, 0.10f, pixels);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        Capture("GroomBinding_GL_Forward_Side", side, -1.44f, 0.10f, pixels);
    }

    // ── Criterion 4: short coat, long coat, human hair ─────────────────────

    TEST_F(GroomBindingVisualEvidenceTest, EveryCoatTypeTheCriteriaNameBindsAndDeforms)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        for (const CoatKind& kind : { kShortAnimalCoat, kLongAnimalCoat, kHumanHair })
        {
            InstallCoat(kind);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            SetBodyPose(55.0f);

            SetBindingEnabled(false);
            std::vector<u8> unbound;
            Capture(std::string("GroomBindingOff_GL_Forward_") + kind.Name, eye, 0.0f, 0.10f, unbound);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            SetBindingEnabled(true);
            std::vector<u8> bound;
            Capture(std::string("GroomBinding_GL_Forward_") + kind.Name, eye, 0.0f, 0.10f, bound);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            const GroomRenderStats stats = PassStats();
            const u32 differing = CountDifferingPixels(bound, unbound);
            const f32 worst = WorstRelativeLengthError();
            std::printf("[groom-binding] coat %-10s  %u px moved, %u roots deformed, worst length error %.6f\n",
                        kind.Name, differing, stats.RootsDeformed, static_cast<f64>(worst));

            EXPECT_EQ(stats.GroomsDeformed, 1u) << kind.Name;
            EXPECT_EQ(stats.GroomsBindingRefused, 0u) << kind.Name;
            EXPECT_GT(differing, 1000u) << kind.Name << ": this coat did not follow the body";
            EXPECT_LT(worst, 1.0e-3f) << kind.Name << ": this coat collapsed";
        }
    }

    // ── #1427: the GPU-deformed coat is the CPU-deformed coat ──────────────
    //
    // Since #1427 a bound coat is moved by the strand vertex shader from a
    // stream built once and a per-frame buffer; the CPU rebuild it replaced is
    // kept behind RendererSettings::GroomGpuDeformation as the reference. The
    // A/B here is that lever and nothing else: the same bent body, the same
    // coat, the same camera. The two frames must be the same picture — the
    // unit contract (GroomGpuDeformationTest) says the vertices agree exactly
    // on the CPU, and this is where the GPU is held to it.
    //
    // NEGATIVE CONTROL in every cell: the unbound coat on the same bent body,
    // which must differ from the GPU frame by thousands of pixels. Without it a
    // GPU path that drew nothing, or drew the bind pose, would "match" a CPU
    // frame that did the same.
    //
    // Writes GroomGpuDeformation[Off]_GL_<Path>_<Angle>.png: "Off" is the CPU
    // reference, so a missing file is an unrun cell.
    TEST_F(GroomBindingVisualEvidenceTest, TheGpuDeformedCoatIsTheCpuDeformedCoat)
    {
        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        const std::array<PathCase, 3> paths = { {
            { "Forward", RenderingPath::Forward },
            { "ForwardPlus", RenderingPath::ForwardPlus },
            { "Deferred", RenderingPath::Deferred },
        } };
        struct Angle
        {
            const char* Name;
            glm::vec3 Eye;
            f32 Yaw;
            f32 Pitch;
        };
        // Front, a raking three-quarter view and the side the hinge lifts
        // toward: a deformation that is right from the front and wrong in depth
        // is invisible from one viewpoint.
        const std::array<Angle, 3> angles = { {
            { "Front", glm::vec3(0.0f, 0.9f, 4.6f), 0.0f, 0.10f },
            { "Oblique", glm::vec3(3.2f, 2.2f, 3.2f), -0.785f, 0.40f },
            { "Side", glm::vec3(-4.6f, 0.9f, 0.0f), 1.5708f, 0.10f },
        } };

        auto& settings = Renderer3D::GetRendererSettings();
        // Restored whatever happens below: the lever is process-wide and every
        // later test in this binary expects the shipped path.
        struct LeverGuard
        {
            RendererSettings& Settings;
            ~LeverGuard()
            {
                Settings.GroomGpuDeformation = true;
            }
        } guard{ settings };

        SetBodyPose(55.0f);
        for (const PathCase& pathCase : paths)
        {
            settings.Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();
            for (const Angle& angle : angles)
            {
                const std::string cell = std::string("_GL_") + pathCase.Name + "_" + angle.Name;

                SetBindingEnabled(false);
                std::vector<u8> unbound;
                Capture("", angle.Eye, angle.Yaw, angle.Pitch, unbound);
                SetBindingEnabled(true);

                settings.GroomGpuDeformation = false;
                std::vector<u8> cpu;
                Capture("GroomGpuDeformationOff" + cell, angle.Eye, angle.Yaw, angle.Pitch, cpu);
                if (::testing::Test::HasFatalFailure())
                {
                    return;
                }
                const GroomRenderStats cpuStats = PassStats();

                settings.GroomGpuDeformation = true;
                std::vector<u8> gpu;
                Capture("GroomGpuDeformation" + cell, angle.Eye, angle.Yaw, angle.Pitch, gpu);
                if (::testing::Test::HasFatalFailure())
                {
                    return;
                }
                const GroomRenderStats gpuStats = PassStats();

                const u32 moved = CountDifferingPixels(gpu, unbound);
                const u32 differing = CountDifferingPixels(gpu, cpu);
                std::printf("[groom-gpu-deformation] %-11s %-8s gpu-vs-cpu %u px, gpu-vs-unbound %u px, "
                            "uploaded %.2f MiB (cpu path %.2f MiB)\n",
                            pathCase.Name, angle.Name, differing, moved,
                            static_cast<f64>(gpuStats.DeformedUploadBytes) / (1024.0 * 1024.0),
                            static_cast<f64>(cpuStats.DeformedUploadBytes) / (1024.0 * 1024.0));

                // Each frame took the path it was asked for, so the comparison
                // is between the two paths and not two frames of one.
                EXPECT_EQ(cpuStats.GroomsDeformed, 1u) << cell;
                EXPECT_EQ(cpuStats.GroomsGpuDeformed, 0u) << cell << ": the reference frame was GPU-deformed";
                EXPECT_EQ(gpuStats.GroomsDeformed, 1u) << cell;
                EXPECT_EQ(gpuStats.GroomsGpuDeformed, 1u) << cell << ": the GPU frame fell back to the CPU path";
                EXPECT_LT(gpuStats.DeformedUploadBytes * 8u, cpuStats.DeformedUploadBytes)
                    << cell << ": the GPU path must send a fraction of what the CPU path re-uploads";

                EXPECT_GT(MeanLuminance(gpu), 0.02) << cell << ": the GPU frame is (near-)black";
                EXPECT_GT(moved, 1000u) << cell << ": the bent body did not move the GPU-deformed coat";
                // The same picture. Not zero, only because the two paths reach
                // a held-at-rest root by different rounding and the GPU's
                // quaternion rotate may contract differently from the CPU's; a
                // path that got a strand wrong moves a whole ribbon, hundreds of
                // pixels at this framing, and a coat that got its registration
                // wrong moves thousands.
                EXPECT_LT(differing, 64u) << cell << ": the GPU-deformed coat is not the CPU-deformed coat";
            }
        }
    }

    // #1427, criterion 4's three coat kinds on the GPU path: the long coat has
    // the most per-strand rotation to get wrong, the scalp the densest roots.
    TEST_F(GroomBindingVisualEvidenceTest, EveryCoatTypeIsTheSameCoatOnTheGpuPath)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        auto& settings = Renderer3D::GetRendererSettings();
        struct LeverGuard
        {
            RendererSettings& Settings;
            ~LeverGuard()
            {
                Settings.GroomGpuDeformation = true;
            }
        } guard{ settings };

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };
        for (const CoatKind& kind : { kShortAnimalCoat, kLongAnimalCoat, kHumanHair })
        {
            InstallCoat(kind);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            SetBodyPose(55.0f);
            SetBindingEnabled(true);

            settings.GroomGpuDeformation = false;
            std::vector<u8> cpu;
            Capture(std::string("GroomGpuDeformationOff_GL_Forward_") + kind.Name, eye, 0.0f, 0.10f, cpu);
            settings.GroomGpuDeformation = true;
            std::vector<u8> gpu;
            Capture(std::string("GroomGpuDeformation_GL_Forward_") + kind.Name, eye, 0.0f, 0.10f, gpu);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            const GroomRenderStats stats = PassStats();
            const u32 differing = CountDifferingPixels(gpu, cpu);
            std::printf("[groom-gpu-deformation] coat %-10s gpu-vs-cpu %u px\n", kind.Name, differing);
            EXPECT_EQ(stats.GroomsGpuDeformed, 1u) << kind.Name;
            EXPECT_GT(MeanLuminance(gpu), 0.02) << kind.Name;
            EXPECT_LT(differing, 64u) << kind.Name << ": the GPU-deformed coat is not the CPU-deformed coat";
        }
    }

    // ── Criterion 2: a facial morph, with the skeleton standing still ──────

    TEST_F(GroomBindingVisualEvidenceTest, AMorphedSurfaceCarriesTheCoatWithTheSkeletonStill)
    {
        // The morph half of criterion 2, and the reason it is a separate case:
        // morph deltas are written straight into the vertex array, so this path
        // reaches the coat WITHOUT the palette moving at all. A feature that
        // only handled skinning would pass every case above and fail this one.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        SetBindingEnabled(true);
        SetBodyPose(0.0f);

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        // MorphNeutral, not "Off": the binding is ENABLED for both captures in
        // this case -- the A/B is neutral-expression against morphed-expression,
        // not bound against unbound. Every other `...Off...` PNG in this folder
        // is a genuinely disabled binding, so the prefix is a promise, and a
        // reader comparing this one against them would conclude the coat is
        // unaffected by the morph when the picture actually shows the opposite.
        std::vector<u8> neutral;
        Capture("GroomBinding_GL_Forward_MorphNeutral", eye, 0.0f, 0.10f, neutral);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        // "An expression": a bulge written into the live vertex array exactly as
        // MorphTargetSystem writes one. The rest positions are kept so the
        // fixture can put the body back.
        auto& vertices = m_BodySurface->GetVertices();
        std::vector<glm::vec3> rest;
        rest.reserve(static_cast<sizet>(vertices.Num()));
        for (i32 i = 0; i < vertices.Num(); ++i)
        {
            rest.push_back(vertices[i].Position);
            const glm::vec3& p = vertices[i].Position;
            const f32 falloff = std::exp(-6.0f * ((p.x * p.x) + ((p.y - 1.0f) * (p.y - 1.0f))));
            vertices[i].Position += glm::vec3(0.0f, 0.35f * falloff, 0.0f);
        }
        m_BodySurface->Build();

        std::vector<u8> expressed;
        Capture("GroomBinding_GL_Forward_Morph", eye, 0.0f, 0.10f, expressed);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const GroomRenderStats stats = PassStats();
        const u32 differing = CountDifferingPixels(expressed, neutral);
        std::printf("[groom-binding] morph          %u px moved, %u roots deformed\n", differing,
                    stats.RootsDeformed);
        EXPECT_EQ(stats.GroomsDeformed, 1u);
        EXPECT_GT(differing, 1000u) << "the coat did not follow the expression";

        for (i32 i = 0; i < vertices.Num(); ++i)
        {
            vertices[i].Position = rest[static_cast<sizet>(i)];
        }
        m_BodySurface->Build();
    }

    // ── Criterion 1: a refused binding is a refusal, not a quiet bind pose ──

    TEST_F(GroomBindingVisualEvidenceTest, AGroomBoundToNothingIsReportedRatherThanDrawnAtRestInSilence)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        SetBodyPose(55.0f);
        SetBindingEnabled(true);

        // Point the component at no binding at all. The coat still draws — a
        // groom that vanished would be indistinguishable from a broken asset —
        // but the pass must COUNT the refusal, which is the whole difference
        // between "this is at the bind pose because nothing is bound" and "this
        // is at the bind pose for no stated reason".
        m_GroomEntity.GetComponent<GroomBindingComponent>().m_Binding = 0;

        std::vector<u8> pixels;
        Capture("", { 0.0f, 0.9f, 4.6f }, 0.0f, 0.10f, pixels);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const GroomRenderStats stats = PassStats();
        EXPECT_EQ(stats.GroomsDeformed, 0u);
        EXPECT_EQ(stats.GroomsBindingRefused, 1u)
            << "a groom that asked to be bound and was not must be counted, or a bind-posed coat is silent";
        EXPECT_EQ(stats.GroomsDrawn, 1u) << "the coat must still draw; a vanished groom reads as a broken asset";

        m_GroomEntity.GetComponent<GroomBindingComponent>().m_Binding = m_BindingHandle;
    }
} // namespace OloEngine::Tests
