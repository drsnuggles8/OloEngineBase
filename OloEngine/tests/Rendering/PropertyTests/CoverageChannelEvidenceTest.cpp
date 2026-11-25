// OLO_TEST_LAYER: L8
// =============================================================================
// CoverageChannelEvidenceTest.cpp
//
// The one thing every other #1256 test cannot show: that a strand's COVERAGE
// actually reaches a pixel.
//
// The model tests pin the arithmetic, the GPU parity probe pins the GLSL twin,
// the contract test pins that every writer declares four channels — and all
// three would still pass if RT3's .b lane were wired to nothing. GLSL says an
// unwritten MRT component is UNDEFINED, not zero, so "the shader assigns a
// vec4" and "the blue channel holds this strand's alpha" are different claims
// and only a readback settles the second.
//
// So this test renders real strands through the real pass and READS RT3 BACK:
//
//   * the background must be exactly 0 — nothing drew there, so there is no
//     surface and no coverage. This is also what catches a writer that misses
//     RT3 entirely, because an unwritten lane is whatever the tile held;
//   * the opaque body must be exactly 1 — a surface that fills its pixel;
//   * and the strands must produce values STRICTLY BETWEEN the two. That is
//     the whole claim. A groom whose coverage came back as a flat 1.0 would
//     mean the lane is carrying the opaque default and the feature is inert
//     for the subject it exists for.
//
// The strands are deliberately authored THIN — sub-pixel, then widened to one
// pixel with a compensating alpha, which is what GroomCoverage.h describes as
// the load-bearing step of every production hair renderer. That widening is
// exactly where a fractional coverage comes from, so a width scale large
// enough to make fat strands would make this test pass for the wrong reason.
//
// Runs in the normal suite and SKIPs (not fails) without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"
#include "RenderPropertyTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 320u;
        constexpr u32 kHeight = 240u;
        /// RT3 on the forward scene framebuffer. GroomStrand.glsl targets that
        /// layout (colour / entity id / view normal / velocity / skin diffuse),
        /// not the six-attachment deferred G-Buffer.
        constexpr u32 kVelocityAttachment = 3u;
        /// fp16 storage, so an exact 1.0 survives but a comparison should not
        /// ask for bit equality.
        constexpr f32 kEpsilon = 1.0e-3f;
        // OLO_STOCHASTIC_COVERAGE_MARK (include/SurfaceCoverageMark.glsl).
        constexpr f32 kStochasticCoverageMark = -1.0f;
    } // namespace

    class CoverageChannelEvidenceTest : public RendererAttachedTest
    {
      public:
        AssetHandle m_GroomHandle = 0;

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
                            "  Name: CoverageChannelEvidence\n"
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
            rendererSettings.ShowComponentGizmos = false;
            rendererSettings.ShowGrid = false;
            rendererSettings.ShowWorldAxisHelper = false;

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 6.0f, 4.0f);
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.7f, -0.6f));
                dl.m_Color = glm::vec3(1.0f);
                dl.m_Intensity = 3.0f;
            }

            // The opaque control. Without a surface that definitely fills its
            // pixels, "the strands are below 1.0" has nothing to be below.
            {
                Entity body = scene.CreateEntity("Body");
                auto& tc = body.GetComponent<TransformComponent>();
                tc.Scale = glm::vec3(0.55f);
                auto& mc = body.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Sphere;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateSphere())
                {
                    mc.m_MeshSource = mesh->GetMeshSource();
                }
                auto& mat = body.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.10f, 0.09f, 0.09f, 1.0f));
            }

            Ref<GroomAsset> groom = BuildThinGroom();
            ASSERT_TRUE(groom);
            m_GroomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(groom);
            ASSERT_NE(static_cast<u64>(m_GroomHandle), 0u);

            Entity groomEntity = scene.CreateEntity("Groom");
            auto& groomComponent = groomEntity.AddComponent<GroomComponent>();
            groomComponent.m_Groom = m_GroomHandle;
            groomComponent.m_ShowPreview = false;
            groomComponent.m_RenderStrands = true;
            groomComponent.m_MaxRenderStrands = 4000;
            // StochasticAlpha, deliberately, and the first version of this
            // test got it wrong. OpaqueRibbon applies a hard `alpha < cutoff ->
            // discard` at 0.5, and a genuinely sub-pixel strand has a widened
            // alpha well under that — so every strand fragment was discarded
            // and the frame contained no strands at all, which reads
            // identically to "the coverage lane is inert". StochasticAlpha has
            // no cutoff: a fragment survives with probability alpha and keeps
            // that alpha, so fractional coverage reaches the target at any
            // width. It is also the mode the dead band exists for.
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::StochasticAlpha);
            groomComponent.m_StrandColor = glm::vec3(0.72f, 0.60f, 0.46f);
        }

        /// A small hemisphere of THIN strands. The thinness is the point: a
        /// sub-pixel strand is widened to one pixel and pays for it in alpha,
        /// and that alpha is the coverage this test is looking for.
        static Ref<GroomAsset> BuildThinGroom()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

            constexpr u32 kStrands = 2000u;
            constexpr u32 kPoints = 8u;
            constexpr f32 kRadius = 0.6f;
            constexpr f32 kLength = 0.7f;
            constexpr f32 kGoldenAngle = 2.39996323f;

            for (u32 s = 0u; s < kStrands; ++s)
            {
                const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(kStrands);
                const f32 cosTheta = 1.0f - t;
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - (cosTheta * cosTheta)));
                const f32 phi = kGoldenAngle * static_cast<f32>(s);
                const glm::vec3 normal(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
                const glm::vec3 root = normal * kRadius;

                std::vector<glm::vec3> points;
                std::vector<f32> widths;
                points.reserve(kPoints);
                widths.reserve(kPoints);
                for (u32 p = 0u; p < kPoints; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(kPoints - 1u);
                    glm::vec3 point = root + (normal * (kLength * along));
                    point.y -= kLength * 0.75f * along * along;
                    points.push_back(point);
                    // THIN, and tapering. Sub-pixel at this framing, which is
                    // what forces the one-pixel widening and the compensating
                    // alpha this test is reading back.
                    widths.push_back(0.006f * (1.0f - (0.5f * along)));
                }

                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { std::fmod(phi / (2.0f * 3.14159265f), 1.0f), t };
                input.GroupId = group;
                input.IsGuide = (s % 25u) == 0u;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }

            builder.SetName("CoverageChannelEvidenceGroom");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            return groom;
        }

        /// Write the coverage lane out as a picture. Asserting on numbers
        /// read back from a frame nobody looked at is how a test comes to
        /// defend a broken image: the counts below would be just as happy with
        /// coverage scattered over the wrong pixels as with a coat.
        ///
        /// Greyscale is deliberate — coverage is a scalar, and a false-colour
        /// ramp would invent structure at the boundaries between bands.
        static void WriteCoveragePng(const std::string& tag, const std::vector<f32>& texels)
        {
            std::vector<u8> image(static_cast<std::size_t>(kWidth) * kHeight * 4u, 0u);
            for (std::size_t px = 0u; px < static_cast<std::size_t>(kWidth) * kHeight; ++px)
            {
                const f32 coverage = texels[(px * 4u) + 2u];
                const u8 value = static_cast<u8>(
                    std::clamp(coverage, 0.0f, 1.0f) * 255.0f + 0.5f);
                image[(px * 4u) + 0u] = value;
                image[(px * 4u) + 1u] = value;
                image[(px * 4u) + 2u] = value;
                image[(px * 4u) + 3u] = 255u;
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("CoverageChannel_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                               4, image.data(), static_cast<int>(kWidth) * 4);
            EXPECT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        [[nodiscard]] bool ReadVelocityTarget(std::vector<f32>& out)
        {
            const auto sceneFB = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            if (!sceneFB)
                return false;
            const RHI::ResourceHandle textureID = sceneFB->GetColorAttachmentHandle(kVelocityAttachment);
            if (!textureID.IsValid())
                return false;
            ReadbackRgbaFloat(textureID, kWidth, kHeight, out);
            return out.size() == static_cast<std::size_t>(kWidth) * kHeight * 4u;
        }
    };

    TEST_F(CoverageChannelEvidenceTest, StrandsWriteFractionalCoverageIntoTheVelocityTargetsBlueChannel)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose(glm::vec3(0.0f, 0.55f, 2.6f), 0.0f, -0.15f);
        RunEditorFrames(camera, 3);

        std::vector<f32> texels;
        ASSERT_TRUE(ReadVelocityTarget(texels)) << "the scene framebuffer carries no velocity attachment";
        WriteCoveragePng("GL_Forward_Strands", texels);

        u32 untouched = 0u;       // coverage == 0 — nothing drew here
        u32 fullyCovered = 0u;    // coverage == 1 — an opaque surface
        u32 fractional = 0u;      // 0 < coverage < 1 — THE CLAIM
        u32 outOfRange = 0u;      // anything else is a lane carrying rubbish
        u32 marked = 0u;          // .a holds the stochastic mark: a StochasticAlpha strand drew here (#1552)
        u32 markedUntouched = 0u; // ...where nothing drew: a writer that leaves .a undefined
        u32 otherProfile = 0u;    // .a neither 0 nor the mark: no shader in this scene writes a profile
        f32 minFractional = 1.0f;
        f32 maxFractional = 0.0f;

        for (std::size_t i = 0u; i + 3u < texels.size(); i += 4u)
        {
            const f32 coverage = texels[i + 2u];
            const f32 profile = texels[i + 3u];

            if (!std::isfinite(coverage) || coverage < -kEpsilon || coverage > 1.0f + kEpsilon)
                ++outOfRange;
            else if (coverage <= kEpsilon)
                ++untouched;
            else if (coverage >= 1.0f - kEpsilon)
                ++fullyCovered;
            else
            {
                ++fractional;
                minFractional = std::min(minFractional, coverage);
                maxFractional = std::max(maxFractional, coverage);
            }

            if (std::isfinite(profile) && std::abs(profile - kStochasticCoverageMark) <= kEpsilon)
            {
                ++marked;
                markedUntouched += coverage <= kEpsilon ? 1u : 0u;
            }
            else if (!std::isfinite(profile) || std::abs(profile) > kEpsilon)
            {
                ++otherProfile;
            }
        }

        // Printed, not only asserted: these are the numbers that say what the
        // lane actually contains, and a future change that quietly flattens it
        // is far easier to diagnose from them than from a bare pass/fail.
        std::cout << "[coverage] untouched=" << untouched << " fullyCovered=" << fullyCovered
                  << " fractional=" << fractional << " outOfRange=" << outOfRange
                  << " range=[" << minFractional << ", " << maxFractional << "]\n";

        EXPECT_EQ(outOfRange, 0u) << "RT3's blue lane holds values outside [0,1] — a writer is not "
                                     "covering all four channels, so this is reading undefined memory.";
        EXPECT_GT(untouched, 0u) << "no pixel reads coverage 0; the target is not being cleared.";
        EXPECT_GT(fullyCovered, 0u) << "no pixel reads coverage 1; the opaque body did not write RT3.";

        // Separates the two ways this can fail. A groom that drew nothing at
        // all (every fragment discarded by an alpha cutoff, the trap
        // GroomStrandCommon.glsl documents) and a groom that drew opaque both
        // produce fractional == 0, and they need different fixes.
        const u32 coveredPixels = fullyCovered + fractional;
        EXPECT_GT(coveredPixels, 7500u)
            << "only " << coveredPixels << " pixels carry any coverage — about what the body sphere "
                                           "alone covers, so the strands did not draw. Check the composition mode's alpha cutoff "
                                           "against the widened alpha before suspecting the coverage lane.";

        // The claim.
        EXPECT_GT(fractional, 0u)
            << "no pixel carries fractional coverage. The strands are writing the opaque default, so "
               "the coverage channel is inert for the subject it exists for.";
        // RT3's alpha lane is the MATERIAL PROFILE, and no shader in this scene
        // writes one: it reads 0, except where a StochasticAlpha strand drew,
        // which marks its pixel (SurfaceCoverageMark.glsl, #1552) so TAA keeps
        // that coverage's feedback in motion. Anything else means a writer
        // left the lane undefined.
        std::cout << "[coverage] profile: marked=" << marked << " markedUntouched=" << markedUntouched
                  << " other=" << otherProfile << "\n";
        EXPECT_EQ(otherProfile, 0u) << "RT3's profile lane holds a value that is neither 0 nor the stochastic mark.";
        EXPECT_EQ(markedUntouched, 0u) << "the stochastic mark where nothing drew: a writer left .a undefined.";
        EXPECT_GT(marked, 0u) << "the StochasticAlpha strands did not mark their pixels.";
    }

    class TAACoverageMotionRampTest : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            // The graph below supplies deterministic surface and history textures.
        }
    };

    TEST_F(TAACoverageMotionRampTest, OnlyStochasticCoverageKeepsItsFeedbackInMotion)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct Band
        {
            f32 Coverage;
            f32 Profile;
            f32 MotionPixels;
            f32 Feedback;
        };
        // A stationary foliage control and an opaque moving control surround
        // the two branches of the exemption. Both fractional writers put 0.5
        // in RT3.b; only the stochastic writer puts its mark in RT3.a.
        constexpr std::array bands{
            Band{ 0.5f, 0.0f, 0.0f, 0.9f },
            Band{ 0.5f, 0.0f, 3.0f, 0.7f },
            Band{ 0.5f, 0.0f, 8.0f, 0.5f },
            Band{ 0.5f, -1.0f, 8.0f, 0.9f },
            Band{ 1.0f, 0.0f, 8.0f, 0.5f },
        };
        constexpr u32 bandWidth = 40u;
        constexpr u32 width = bandWidth * static_cast<u32>(bands.size());
        constexpr u32 height = 64u;
        std::vector<glm::vec4> current(width * height);
        std::vector<glm::vec4> velocity(width * height);
        std::vector<glm::vec4> history(width * height, glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
        std::vector<f32> depth(width * height, 0.5f);
        for (u32 y = 0u; y < height; ++y)
        {
            for (u32 x = 0u; x < width; ++x)
            {
                const f32 gray = y % 2u == 0u ? 0.25f : 0.75f;
                current[y * width + x] = glm::vec4(gray, gray, gray, 1.0f);
                const Band& band = bands[x / bandWidth];
                velocity[y * width + x] =
                    glm::vec4(band.MotionPixels / static_cast<f32>(width), 0.0f, band.Coverage, band.Profile);
            }
        }

        struct Inputs
        {
            Ref<Framebuffer> Current;
            Ref<Texture2D> Velocity;
            Ref<Texture2D> PreviousSurface;
            Ref<Texture2D> History;
            Ref<Texture2D> Depth;
            Ref<Framebuffer> Output;
        } inputs;
        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        auto taa = Ref<TAARenderPass>::Create();
        RunInFrame([&]()
                   {
            const auto makeTexture = [&](ImageFormat format, void* data, u32 bytes)
            {
                auto texture = Texture2D::Create({ .Width = width, .Height = height, .Format = format, .GenerateMips = false });
                if (texture)
                    texture->SetData(data, bytes);
                return texture;
            };
            constexpr u32 rgbaBytes = width * height * sizeof(glm::vec4);
            FramebufferSpecification currentSpec;
            currentSpec.Width = width;
            currentSpec.Height = height;
            currentSpec.Attachments = { FramebufferTextureFormat::RGBA32F };
            inputs.Current = Framebuffer::Create(currentSpec);
            ASSERT_TRUE(inputs.Current);
            RenderCommand::UploadTextureSubImage2D(inputs.Current->GetColorAttachmentHandle(0), width, height,
                                                   RHI::Format::RGBA32Float, current.data());
            inputs.Velocity = makeTexture(ImageFormat::RGBA32F, velocity.data(), rgbaBytes);
            inputs.PreviousSurface = makeTexture(ImageFormat::RGBA32F, velocity.data(), rgbaBytes);
            inputs.History = makeTexture(ImageFormat::RGBA32F, history.data(), rgbaBytes);
            inputs.Depth = makeTexture(ImageFormat::R32F, depth.data(), width * height * sizeof(f32));
            ASSERT_TRUE(inputs.Velocity && inputs.PreviousSurface && inputs.History && inputs.Depth);

            RGResourceDesc colorDesc;
            colorDesc.Kind = RGResourceHandle::Kind::Texture2D;
            colorDesc.Width = width;
            colorDesc.Height = height;
            colorDesc.Format = RGResourceFormat::RGBA32Float;
            auto depthDesc = colorDesc;
            depthDesc.Format = RGResourceFormat::R32Float;
            auto& board = graph.GetBlackboard();
            auto currentDesc = colorDesc;
            currentDesc.Kind = RGResourceHandle::Kind::Framebuffer;
            currentDesc.Attachments = { RGResourceFormat::RGBA32Float };
            const auto color = graph.ImportFramebuffer(ResourceNames::PostProcessColor, inputs.Current, currentDesc);
            ASSERT_TRUE(graph.CreateFramebufferAttachmentView(ResourceNames::PostProcessColorTexture, color, 0u).IsValid());
            board.Scene.SceneDepth = graph.ImportTextureHandle(ResourceNames::SceneDepth, inputs.Depth->GetRHIHandle(), depthDesc);
            board.Scene.SceneVelocity = graph.ImportTextureHandle(ResourceNames::SceneVelocity, inputs.Velocity->GetRHIHandle(), colorDesc);
            board.Temporal.TAASurfaceHistory = graph.ImportHistoryHandle(ResourceNames::TAASurfaceHistory, inputs.PreviousSurface->GetRHIHandle());
            board.Temporal.TAAHistory = graph.ImportHistoryHandle(ResourceNames::TAAHistory, inputs.History->GetRHIHandle());

            auto targetDesc = colorDesc;
            targetDesc.Kind = RGResourceHandle::Kind::Framebuffer;
            targetDesc.Format = RGResourceFormat::RGBA16Float;
            targetDesc.Attachments = { RGResourceFormat::RGBA16Float, RGResourceFormat::RGBA16Float };
            FramebufferSpecification spec;
            spec.Width = width;
            spec.Height = height;
            spec.Attachments = { FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::RGBA16F };
            inputs.Output = Framebuffer::Create(spec);
            ASSERT_TRUE(inputs.Output);
            board.Post.TAAColor = graph.DeclareTransientFramebuffer(ResourceNames::TAAColor, targetDesc, inputs.Output);
            taa->Init(spec);
            taa->SetEnabled(true);
            PostProcessSettings settings;
            settings.TAAFeedback = 0.9f;
            settings.TAASharpness = 0.0f;
            taa->SetSettings(settings);
            graph.AddNode(taa);
            graph.SetFinalPass("TAAPass");
            graph.BuildFrameGraph();
            ASSERT_TRUE(taa->GetPrimaryInputTextureHandle().IsValid()) << "TAA did not select its current color";
            ASSERT_TRUE(taa->GetPrimaryOutputFramebufferHandle().IsValid()) << "TAA did not declare its output";
            graph.Execute();
            for (const auto& failure : graph.GetResolveFailures())
                ADD_FAILURE() << failure.PassName.ToView() << ": " << failure.Reason.ToView(); });
        ASSERT_FALSE(HasFatalFailure());
        const auto output = taa->GetTarget();
        ASSERT_TRUE(output);
        std::vector<f32> resolved;
        ReadbackRgbaFloat(output->GetColorAttachmentHandle(TAARenderPass::kHistoryAttachment), width, height, resolved);
        ASSERT_EQ(resolved.size(), width * height * 4u);
        std::vector<u8> feedback(width * height);
        for (sizet i = 0; i < feedback.size(); ++i)
        {
            const f32 weight = 1.0f - 4.0f * std::abs(resolved[4u * i] - 0.5f);
            feedback[i] = std::isfinite(weight) ? static_cast<u8>(std::clamp(weight, 0.0f, 1.0f) * 255.0f) : 0u;
        }
        const fs::path evidenceDir = fs::path("assets") / "tests" / "visual";
        std::error_code error;
        fs::create_directories(evidenceDir, error);
        ASSERT_FALSE(error) << error.message();
        const auto evidence = evidenceDir / (TestRenderBackend::EvidenceName("TaaMotionRamp") + "Resolve.png");
        ASSERT_NE(::stbi_write_png(evidence.string().c_str(), width, height, 1, feedback.data(), width), 0);

        // The constant gray history lies inside the clip box of both stripe
        // phases for both kernels. The class masks reach beyond dilation,
        // reprojection and the 8x8 coverage comparison. The confidence stays
        // one there, so the resolved distance to gray is (1 - feedback) / 4.
        constexpr u32 margin = 14u;
        for (u32 band = 0u; band < bands.size(); ++band)
        {
            u32 qualified = 0u;
            u32 mismatches = 0u;
            f32 weightSum = 0.0f;
            for (u32 y = margin; y < height - margin; ++y)
            {
                for (u32 x = band * bandWidth + margin; x < (band + 1u) * bandWidth - margin; ++x)
                {
                    const f32 channel = resolved[4u * (y * width + x)];
                    const f32 weight = 1.0f - 4.0f * std::abs(channel - 0.5f);
                    mismatches += (!std::isfinite(weight) || std::abs(weight - bands[band].Feedback) > 0.01f) ? 1u : 0u;
                    weightSum += weight;
                    ++qualified;
                }
            }
            ASSERT_GE(qualified, 400u);
            std::cout << "[taa-motion-ramp] band=" << band << " samples=" << qualified
                      << " feedback=" << weightSum / static_cast<f32>(qualified) << '\n';
            EXPECT_EQ(mismatches, 0u) << "band " << band << " expected feedback " << bands[band].Feedback;
        }
    }
} // namespace OloEngine::Tests
