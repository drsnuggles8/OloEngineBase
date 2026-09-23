#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// UpscaleFramingEvidenceTest — issues #1397 and #1430.
//
// A non-native UpscaleMode must present the SAME framing as native, at lower
// reconstructed detail, and a groom coat must stay on the body it grows on.
//
// #1397 was the first half failing: on Vulkan the scene pass inherited the
// display-sized viewport of the previous frame's post chain, drew magnified
// into the reduced scene band, and the upscaler presented the band's corner.
// #1430 was the second: the groom pass set its viewport explicitly, so under
// the same frame its coats landed at the band's true scale, off their bodies.
// The mechanism is pinned on Vulkan by VulkanDrawPath.FramebufferBindResets
// TheViewportToItsTarget; this file pins the OUTCOME through the full GL
// pipeline, the only one a headless fixture can drive (every Vulkan cell is
// live-only and is evidenced in the PR body).
//
// The invariant, measured, not eyeballed. Three subjects with known world
// positions — a red cube upper-left, a blue cube lower-right, a green body at
// the centre wearing a groom coat — are segmented in every capture, and:
//
//   1. each subject's centroid in the presented image matches native within a
//      couple of pixels, and its pixel count stays near native's (a crop at
//      scale s multiplies every area by 1/s^2, so this is the FOV check);
//   2. the coat's centroid sits where it sits on the body at native — the coat
//      is measured as the A/B difference against the same frame with strand
//      rendering off, so the body underneath cannot be mistaken for it;
//   3. every mask has a pixel floor, so an empty frame cannot pass by having
//      no centroid to be wrong about.
//
// NEGATIVE CONTROL: the comparison is also run against the native frame
// cropped and magnified the way #1397 presented it. It must REJECT that frame,
// or the invariant could not have caught the bug it is here for.
//
// Cells, one PNG each: UpscaleFraming_GL_<Path>_<Mode>[_FSR2].png for
// {Forward, ForwardPlus, Deferred} x {Native, Quality, Performance} with the
// spatial upscaler, the FSR2 technique at Quality/Performance on each path,
// FSR2 requested under Deferred MSAA 4 (which falls back to spatial), a second
// viewport size (16:9 against the issue's 1024x683), and a dynamic-resolution
// arm for the groom pass's own viewport.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Upscaling/TemporalUpscaler.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // The issue's editor viewport: not 16:9, because an off-by-aspect
        // error hides at 16:9.
        constexpr u32 kWidth = 1024;
        constexpr u32 kHeight = 683;
        constexpr f32 kCaptureTime = 2.0f;

        // FSR2 needs a few jittered frames before its history means anything;
        // the spatial path needs one plus the graph's warm-up.
        constexpr u32 kSpatialFrames = 3;
        constexpr u32 kTemporalFrames = 12;

        // A strand pixel differs from the strands-off frame by more than this
        // in some channel. Well above dithering and FSR2 jitter on the solid
        // subjects, well below the coat's contrast against the green body.
        constexpr int kCoatDelta = 28;

        // Tolerances, in presented pixels. A subject ~100 px across rendered at
        // half resolution and reconstructed moves its centroid by under a pixel
        // (measured <= 0.7). The coat moves more, legitimately: strands thinner
        // than a pixel get fractional alpha and the opaque tier discards them,
        // so at half resolution the thin tips on the coat's lower rim drop out
        // and its centroid rises -- 5.9 px at FSR2 Performance, measured. That
        // is lost detail, not framing; a #1397 crop moves it by 280 px or more.
        constexpr f64 kSolidCentroidTolerance = 3.0;
        constexpr f64 kCoatCentroidTolerance = 10.0;

        // A crop at render scale s multiplies every area by 1/s^2: 2.25x at
        // Quality, 4x at Performance. Reconstruction moves a solid silhouette's
        // area by a few percent.
        constexpr f64 kSolidAreaRatioMin = 0.80;
        constexpr f64 kSolidAreaRatioMax = 1.25;

        // Floors: every subject must be visibly THERE.
        constexpr u32 kSolidPixelFloor = 1500;
        constexpr u32 kCoatPixelFloor = 3000;

        struct Frame
        {
            u32 Width = 0;
            u32 Height = 0;
            std::vector<u8> Rgba; // top-down

            [[nodiscard]] const u8* At(u32 x, u32 y) const
            {
                return Rgba.data() + ((static_cast<sizet>(y) * Width) + x) * 4u;
            }
        };

        struct Blob
        {
            u32 Count = 0;
            f64 X = 0.0;
            f64 Y = 0.0;
        };

        template<typename Predicate>
        [[nodiscard]] Blob Measure(const Frame& frame, Predicate&& isSubject)
        {
            Blob blob;
            f64 sumX = 0.0;
            f64 sumY = 0.0;
            for (u32 y = 0; y < frame.Height; ++y)
            {
                for (u32 x = 0; x < frame.Width; ++x)
                {
                    if (isSubject(frame.At(x, y)))
                    {
                        ++blob.Count;
                        sumX += static_cast<f64>(x);
                        sumY += static_cast<f64>(y);
                    }
                }
            }
            if (blob.Count > 0u)
            {
                blob.X = sumX / static_cast<f64>(blob.Count);
                blob.Y = sumY / static_cast<f64>(blob.Count);
            }
            return blob;
        }

        [[nodiscard]] bool IsRed(const u8* p)
        {
            return p[0] > 90 && p[0] > 2 * p[1] && p[0] > 2 * p[2];
        }

        [[nodiscard]] bool IsBlue(const u8* p)
        {
            return p[2] > 90 && p[2] * 10 > p[0] * 16 && p[2] * 10 > p[1] * 13;
        }

        [[nodiscard]] bool IsGreen(const u8* p)
        {
            return p[1] > 60 && p[1] * 10 > p[0] * 16 && p[1] * 10 > p[2] * 16;
        }

        [[nodiscard]] Blob MeasureCoat(const Frame& with, const Frame& without)
        {
            Blob blob;
            if (with.Width != without.Width || with.Height != without.Height)
            {
                return blob;
            }
            f64 sumX = 0.0;
            f64 sumY = 0.0;
            for (u32 y = 0; y < with.Height; ++y)
            {
                for (u32 x = 0; x < with.Width; ++x)
                {
                    const u8* a = with.At(x, y);
                    const u8* b = without.At(x, y);
                    bool differs = false;
                    for (int c = 0; c < 3; ++c)
                    {
                        differs = differs || std::abs(static_cast<int>(a[c]) - static_cast<int>(b[c])) > kCoatDelta;
                    }
                    if (differs)
                    {
                        ++blob.Count;
                        sumX += static_cast<f64>(x);
                        sumY += static_cast<f64>(y);
                    }
                }
            }
            if (blob.Count > 0u)
            {
                blob.X = sumX / static_cast<f64>(blob.Count);
                blob.Y = sumY / static_cast<f64>(blob.Count);
            }
            return blob;
        }

        // What one configuration presents: the three solid subjects from the
        // strands-ON frame (the coat does not reach the cubes, and the body's
        // silhouette is taken from the strands-OFF frame where nothing covers
        // it), plus the coat as the A/B difference.
        struct Framing
        {
            Blob Red;
            Blob Blue;
            Blob Body;
            Blob Coat;
        };

        [[nodiscard]] Framing MeasureFraming(const Frame& with, const Frame& without)
        {
            Framing framing;
            framing.Red = Measure(with, IsRed);
            framing.Blue = Measure(with, IsBlue);
            framing.Body = Measure(without, IsGreen);
            framing.Coat = MeasureCoat(with, without);
            return framing;
        }

        [[nodiscard]] f64 Distance(const Blob& a, const Blob& b)
        {
            return std::hypot(a.X - b.X, a.Y - b.Y);
        }

        // Every way `got` fails to present `native`'s framing, one line each.
        // Empty means the framing matches. Written as a report rather than as
        // EXPECTs so the negative control can assert that it is NOT empty.
        [[nodiscard]] std::string CompareFraming(const Framing& native, const Framing& got, bool checkCoat)
        {
            std::ostringstream failures;
            const auto solid = [&failures](const char* name, const Blob& ref, const Blob& blob)
            {
                if (blob.Count < kSolidPixelFloor)
                {
                    failures << name << ": only " << blob.Count << " px (floor " << kSolidPixelFloor << ")\n";
                    return;
                }
                const f64 ratio = static_cast<f64>(blob.Count) / static_cast<f64>(std::max(ref.Count, 1u));
                if (ratio < kSolidAreaRatioMin || ratio > kSolidAreaRatioMax)
                {
                    failures << name << ": area " << blob.Count << " px vs native " << ref.Count << " (ratio " << ratio
                             << ") - the field of view changed\n";
                }
                const f64 moved = Distance(ref, blob);
                if (moved > kSolidCentroidTolerance)
                {
                    failures << name << ": centroid (" << blob.X << ", " << blob.Y << ") vs native (" << ref.X << ", "
                             << ref.Y << "), moved " << moved << " px\n";
                }
            };
            solid("red cube", native.Red, got.Red);
            solid("blue cube", native.Blue, got.Blue);
            solid("body", native.Body, got.Body);

            if (checkCoat)
            {
                if (got.Coat.Count < kCoatPixelFloor)
                {
                    failures << "coat: only " << got.Coat.Count << " px (floor " << kCoatPixelFloor << ")\n";
                }
                else
                {
                    // Registration: the coat's offset from the body it grows on,
                    // against the same offset at native. Framing and registration
                    // are separate failures and are reported separately.
                    const f64 dx = (got.Coat.X - got.Body.X) - (native.Coat.X - native.Body.X);
                    const f64 dy = (got.Coat.Y - got.Body.Y) - (native.Coat.Y - native.Body.Y);
                    const f64 misregistration = std::hypot(dx, dy);
                    if (misregistration > kCoatCentroidTolerance)
                    {
                        failures << "coat: off its body by " << misregistration << " px relative to native (coat ("
                                 << got.Coat.X << ", " << got.Coat.Y << "), body (" << got.Body.X << ", " << got.Body.Y
                                 << "))\n";
                    }
                }
            }
            return failures.str();
        }

        // One line per cell for the log and the PR's verification matrix: the
        // numbers behind a pass, so a reader can see how much margin it had.
        void PrintFraming(const std::string& cell, const Framing& native, const Framing& got)
        {
            const auto blob = [](const Blob& ref, const Blob& b)
            {
                std::ostringstream out;
                out << b.Count << "px@(" << static_cast<int>(std::lround(b.X)) << "," << static_cast<int>(std::lround(b.Y))
                    << ") d=" << Distance(ref, b);
                return out.str();
            };
            std::cout << "[UpscaleFraming] " << cell << ": red " << blob(native.Red, got.Red) << "; blue "
                      << blob(native.Blue, got.Blue) << "; body " << blob(native.Body, got.Body) << "; coat "
                      << blob(native.Coat, got.Coat) << '\n';
        }

        // The #1397 presentation: the band's top-left scale*W x scale*H texels
        // stretched over the whole viewport.
        [[nodiscard]] Frame CropLikeIssue1397(const Frame& native, f32 scale)
        {
            Frame cropped;
            cropped.Width = native.Width;
            cropped.Height = native.Height;
            cropped.Rgba.resize(native.Rgba.size());
            for (u32 y = 0; y < native.Height; ++y)
            {
                for (u32 x = 0; x < native.Width; ++x)
                {
                    const auto sx = static_cast<u32>(static_cast<f32>(x) * scale);
                    const auto sy = static_cast<u32>(static_cast<f32>(y) * scale);
                    std::memcpy(cropped.Rgba.data() + ((static_cast<sizet>(y) * native.Width) + x) * 4u,
                                native.At(sx, sy), 4u);
                }
            }
            return cropped;
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

        [[nodiscard]] const char* ModeName(UpscaleMode mode)
        {
            switch (mode)
            {
                case UpscaleMode::Off:
                    return "Native";
                case UpscaleMode::Quality:
                    return "Quality";
                case UpscaleMode::Balanced:
                    return "Balanced";
                case UpscaleMode::Performance:
                    return "Performance";
                case UpscaleMode::UltraPerformance:
                    return "UltraPerformance";
            }
            return "Unknown";
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

        // Renderer3D::SetRenderScale is process-wide and outside the settings
        // the fixture snapshots; RendererStateCheck flags a leak of it.
        struct ScopedRenderScale
        {
            explicit ScopedRenderScale(f32 scale)
                : m_Saved(Renderer3D::GetRenderScale())
            {
                Renderer3D::SetRenderScale(scale);
            }
            ~ScopedRenderScale()
            {
                Renderer3D::SetRenderScale(m_Saved);
            }
            ScopedRenderScale(const ScopedRenderScale&) = delete;
            ScopedRenderScale& operator=(const ScopedRenderScale&) = delete;

          private:
            f32 m_Saved;
        };
    } // namespace

    class UpscaleFramingEvidenceTest : public RendererAttachedTest
    {
      public:
        Entity m_GroomEntity;

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
                            "  Name: UpscaleFramingEvidence\n"
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
            rendererSettings.EditorDebugDrawsEnabled = true;
            rendererSettings.ShowComponentGizmos = false;
            rendererSettings.ShowGrid = false;
            rendererSettings.ShowWorldAxisHelper = false;

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 6.0f, 6.0f);
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.2f, -0.45f, -1.0f));
                dl.m_Color = glm::vec3(1.0f);
                dl.m_Intensity = 3.0f;
            }

            const auto addMesh = [&scene](const char* name, MeshPrimitive primitive, const glm::vec3& at,
                                          const glm::vec3& scale, const glm::vec4& color)
            {
                Entity entity = scene.CreateEntity(name);
                auto& tc = entity.GetComponent<TransformComponent>();
                tc.Translation = at;
                tc.Scale = scale;
                auto& mc = entity.AddComponent<MeshComponent>();
                mc.m_Primitive = primitive;
                const Ref<Mesh> mesh =
                    primitive == MeshPrimitive::Sphere ? MeshPrimitives::CreateSphere() : MeshPrimitives::CreateCube();
                if (mesh)
                {
                    mc.m_MeshSource = mesh->GetMeshSource();
                }
                auto& mat = entity.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(color);
            };

            // Off-centre on purpose: a crop anchored at a corner moves an
            // off-centre subject a long way, and a centred one hardly at all.
            addMesh("RedMarker", MeshPrimitive::Cube, { -2.5f, 1.35f, 0.0f }, glm::vec3(0.8f),
                    glm::vec4(0.85f, 0.05f, 0.04f, 1.0f));
            addMesh("BlueMarker", MeshPrimitive::Cube, { 2.5f, -1.2f, 0.0f }, glm::vec3(0.8f),
                    glm::vec4(0.04f, 0.10f, 0.90f, 1.0f));
            // THE BODY the coat grows on. Green, so its silhouette is segmented
            // from the strands-off frame without the coat's colour in the mix.
            addMesh("Body", MeshPrimitive::Sphere, glm::vec3(0.0f), glm::vec3(0.98f),
                    glm::vec4(0.06f, 0.55f, 0.08f, 1.0f));

            Ref<GroomAsset> groom = BuildCoat();
            ASSERT_TRUE(groom);
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(groom);
            ASSERT_NE(static_cast<u64>(handle), 0u);

            m_GroomEntity = scene.CreateEntity("Coat");
            auto& coat = m_GroomEntity.AddComponent<GroomComponent>();
            coat.m_Groom = handle;
            coat.m_ShowPreview = false;
            coat.m_RenderStrands = true;
            coat.m_MaxRenderStrands = 3000;
            // Deterministic coverage: a stochastic tier re-jitters every frame
            // and its centroid is noise.
            coat.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            coat.m_StrandColor = glm::vec3(0.95f, 0.85f, 0.75f);
        }

        // An upper-hemisphere coat on the unit body, strands a pixel or two wide
        // at this framing so the coat has a silhouette to register.
        static Ref<GroomAsset> BuildCoat()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

            constexpr u32 kStrands = 3000;
            constexpr u32 kPoints = 6;
            constexpr f32 kRadius = 1.0f;
            constexpr f32 kLength = 0.7f;
            constexpr f32 kGoldenAngle = 2.39996323f;

            for (u32 s = 0; s < kStrands; ++s)
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
                for (u32 p = 0; p < kPoints; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(kPoints - 1);
                    glm::vec3 point = root + (normal * (kLength * along));
                    point.y -= kLength * 0.5f * along * along;
                    points.push_back(point);
                    widths.push_back(0.012f * (1.0f - (0.7f * along)));
                }

                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { std::fmod(phi / (2.0f * 3.14159265f), 1.0f), t };
                input.GroupId = group;
                input.IsGuide = (s % 25u) == 0;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }

            builder.SetName("UpscaleFramingCoat");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
            {
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            }
            return groom;
        }

        [[nodiscard]] static bool TemporalUpscalerUsable()
        {
            const Ref<TemporalUpscaler> upscaler = TemporalUpscaler::Create();
            return upscaler && upscaler->IsAvailable();
        }

        // One capture at the current settings, top-down. `saveAs` empty skips
        // the PNG (the strands-off control frames are measured, not kept).
        void Capture(u32 width, u32 height, u32 frames, const std::string& saveAs, Frame& out)
        {
            EditorCamera camera(45.0f, static_cast<f32>(width) / static_cast<f32>(height), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
            camera.SetPose(glm::vec3(0.0f, 0.0f, 7.0f), 0.0f, 0.0f);

            RunEditorFrames(camera, frames);

            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(ReadbackComposite(out.Rgba, w, h)) << "no composite for '" << saveAs << "'";
            ASSERT_EQ(w, width);
            ASSERT_EQ(h, height);
            out.Width = w;
            out.Height = h;

            const sizet rowBytes = static_cast<sizet>(w) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < h / 2u; ++y)
            {
                u8* top = out.Rgba.data() + (static_cast<sizet>(y) * rowBytes);
                u8* bottom = out.Rgba.data() + (static_cast<sizet>(h - 1u - y) * rowBytes);
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, tmp.data(), rowBytes);
            }

            if (!saveAs.empty())
            {
                const fs::path dir = fs::path("assets") / "tests" / "visual";
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "failed to create '" << dir.string() << "': " << ec.message();
                const std::string path = (dir / (saveAs + ".png")).string();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                                                   out.Rgba.data(), static_cast<int>(rowBytes));
                ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
            }
        }

        // Strands off, then on, under the SAME settings and camera.
        [[nodiscard]] Framing CaptureFraming(u32 width, u32 height, u32 frames, const std::string& saveAs,
                                             Frame* keepWith = nullptr)
        {
            auto& coat = m_GroomEntity.GetComponent<GroomComponent>();
            Frame without;
            Frame with;
            coat.m_RenderStrands = false;
            Capture(width, height, frames, "", without);
            coat.m_RenderStrands = true;
            Capture(width, height, frames, saveAs, with);
            if (keepWith)
            {
                *keepWith = with;
            }
            return MeasureFraming(with, without);
        }
    };

    // ── The whole matrix a headless fixture can reach ──────────────────────

    TEST_F(UpscaleFramingEvidenceTest, UpscaleKeepsNativeFramingOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);

        const bool temporalUsable = TemporalUpscalerUsable();
        auto& settings = Renderer3D::GetRendererSettings();
        auto& pp = Renderer3D::GetPostProcessSettings();

        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            settings.Path = path;
            Renderer3D::ApplyRendererSettings();
            const std::string cell = std::string("UpscaleFraming_GL_") + PathName(path);

            pp.Upscale = UpscaleMode::Off;
            pp.Technique = UpscalerTechnique::Spatial;
            const Framing native = CaptureFraming(kWidth, kHeight, kSpatialFrames, cell + "_Native");
            if (HasFatalFailure())
            {
                return;
            }
            // Native itself must show every subject, or nothing below means anything.
            EXPECT_EQ(CompareFraming(native, native, true), "") << cell << " native";

            for (const UpscaleMode mode : { UpscaleMode::Quality, UpscaleMode::Performance })
            {
                pp.Upscale = mode;
                pp.Technique = UpscalerTechnique::Spatial;
                const Framing spatial = CaptureFraming(kWidth, kHeight, kSpatialFrames, cell + "_" + ModeName(mode));
                if (HasFatalFailure())
                {
                    return;
                }
                PrintFraming(cell + "_" + ModeName(mode), native, spatial);
                EXPECT_EQ(CompareFraming(native, spatial, true), "") << cell << " FSR1 " << ModeName(mode);

                if (temporalUsable)
                {
                    pp.Technique = UpscalerTechnique::Temporal;
                    const Framing temporal =
                        CaptureFraming(kWidth, kHeight, kTemporalFrames, cell + "_" + ModeName(mode) + "_FSR2");
                    if (HasFatalFailure())
                    {
                        return;
                    }
                    PrintFraming(cell + "_" + ModeName(mode) + "_FSR2", native, temporal);
                    EXPECT_EQ(CompareFraming(native, temporal, true), "") << cell << " FSR2 " << ModeName(mode);
                }
            }
        }
        if (!temporalUsable)
        {
            std::cout << "[UpscaleFraming] FSR2 unavailable on this build - temporal cells not captured\n";
        }
    }

    // FSR2 requested with MSAA resolved falls back to the spatial upscaler. The
    // fallback is a different pass chain, so its framing is its own cell.
    TEST_F(UpscaleFramingEvidenceTest, MsaaTemporalFallbackKeepsNativeFraming)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);

        auto& settings = Renderer3D::GetRendererSettings();
        auto& pp = Renderer3D::GetPostProcessSettings();
        settings.Path = RenderingPath::Deferred;
        settings.Deferred.MSAASampleCount = 4u;
        Renderer3D::ApplyRendererSettings();

        pp.Upscale = UpscaleMode::Off;
        pp.Technique = UpscalerTechnique::Spatial;
        const Framing native = CaptureFraming(kWidth, kHeight, kSpatialFrames, "UpscaleFraming_GL_DeferredMSAA4_Native");
        if (HasFatalFailure())
        {
            return;
        }

        pp.Upscale = UpscaleMode::Quality;
        pp.Technique = UpscalerTechnique::Temporal;
        const Framing fallback =
            CaptureFraming(kWidth, kHeight, kTemporalFrames, "UpscaleFraming_GL_DeferredMSAA4_Quality_FSR2Fallback");
        if (HasFatalFailure())
        {
            return;
        }
        PrintFraming("DeferredMSAA4_Quality_FSR2Fallback", native, fallback);
        EXPECT_EQ(CompareFraming(native, fallback, true), "") << "Deferred MSAA 4, FSR2 requested at Quality";
    }

    // The second viewport size, 16:9, so the matrix is not one aspect ratio.
    TEST_F(UpscaleFramingEvidenceTest, UpscaleKeepsNativeFramingAtASecondViewportSize)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);

        constexpr u32 kWideWidth = 1280;
        constexpr u32 kWideHeight = 720;
        ResizeRenderTarget(kWideWidth, kWideHeight);
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward; // the cell the PNG names
        Renderer3D::ApplyRendererSettings();

        auto& pp = Renderer3D::GetPostProcessSettings();
        pp.Technique = UpscalerTechnique::Spatial;
        pp.Upscale = UpscaleMode::Off;
        const Framing native = CaptureFraming(kWideWidth, kWideHeight, kSpatialFrames, "UpscaleFraming_GL_Forward_1280x720_Native");
        if (HasFatalFailure())
        {
            return;
        }
        pp.Upscale = UpscaleMode::Performance;
        const Framing upscaled =
            CaptureFraming(kWideWidth, kWideHeight, kSpatialFrames, "UpscaleFraming_GL_Forward_1280x720_Performance");
        if (HasFatalFailure())
        {
            return;
        }
        PrintFraming("Forward_1280x720_Performance", native, upscaled);
        EXPECT_EQ(CompareFraming(native, upscaled, true), "") << "1280x720 FSR1 Performance";
    }

    // The NEGATIVE CONTROL: the invariant must reject the frame #1397 presented.
    // If it accepted a 1/scale crop, every passing cell above would be vacuous.
    TEST_F(UpscaleFramingEvidenceTest, FramingInvariantRejectsTheIssue1397Crop)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);

        auto& pp = Renderer3D::GetPostProcessSettings();
        pp.Upscale = UpscaleMode::Off;

        auto& coat = m_GroomEntity.GetComponent<GroomComponent>();
        Frame without;
        Frame with;
        coat.m_RenderStrands = false;
        Capture(kWidth, kHeight, kSpatialFrames, "", without);
        coat.m_RenderStrands = true;
        Capture(kWidth, kHeight, kSpatialFrames, "", with);
        if (HasFatalFailure())
        {
            return;
        }
        const Framing native = MeasureFraming(with, without);
        ASSERT_EQ(CompareFraming(native, native, true), "");

        for (const f32 scale : { UpscaleModeToRenderScale(UpscaleMode::Quality),
                                 UpscaleModeToRenderScale(UpscaleMode::Performance) })
        {
            const Framing cropped = MeasureFraming(CropLikeIssue1397(with, scale), CropLikeIssue1397(without, scale));
            PrintFraming("SyntheticCrop_" + std::to_string(scale), native, cropped);
            const std::string report = CompareFraming(native, cropped, true);
            EXPECT_NE(report, "") << "the framing invariant ACCEPTED a " << scale
                                  << "-scale crop - it cannot detect #1397";
        }
    }

    // #1430's pass-local half: the groom pass must draw at the viewport the
    // scene target is BOUND at, not at the target's full size. A dynamic
    // render scale installs exactly that difference on the scene framebuffer,
    // with no upscaler involved, so the body draws into the sub-rectangle and a
    // coat drawn at the full spec would land at twice its size, off the body.
    TEST_F(UpscaleFramingEvidenceTest, GroomCoatFollowsTheBoundRenderViewport)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);

        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward; // the cell the PNG names
        Renderer3D::ApplyRendererSettings();
        auto& pp = Renderer3D::GetPostProcessSettings();
        pp.Upscale = UpscaleMode::Off;

        const Framing native = CaptureFraming(kWidth, kHeight, kSpatialFrames, "");
        if (HasFatalFailure())
        {
            return;
        }
        ASSERT_GE(native.Coat.Count, kCoatPixelFloor);
        ASSERT_GE(native.Body.Count, kSolidPixelFloor);

        const ScopedRenderScale renderScale(0.5f);
        const Framing scaled = CaptureFraming(kWidth, kHeight, kSpatialFrames, "UpscaleFraming_GL_Forward_RenderScale50");
        if (HasFatalFailure())
        {
            return;
        }

        // The render scale shrinks the whole image into a sub-rectangle, so
        // compare the coat's offset from the body in BODY units: both scale
        // together when they agree about the viewport, and the offset doubles
        // relative to the body when they do not.
        ASSERT_GE(scaled.Body.Count, kSolidPixelFloor / 4u) << "the body did not draw under the render scale";
        ASSERT_GE(scaled.Coat.Count, kCoatPixelFloor / 4u) << "the coat did not draw under the render scale";
        const f64 nativeBodyRadius = std::sqrt(static_cast<f64>(native.Body.Count));
        const f64 scaledBodyRadius = std::sqrt(static_cast<f64>(scaled.Body.Count));
        const f64 nativeOffsetX = (native.Coat.X - native.Body.X) / nativeBodyRadius;
        const f64 nativeOffsetY = (native.Coat.Y - native.Body.Y) / nativeBodyRadius;
        const f64 scaledOffsetX = (scaled.Coat.X - scaled.Body.X) / scaledBodyRadius;
        const f64 scaledOffsetY = (scaled.Coat.Y - scaled.Body.Y) / scaledBodyRadius;
        const f64 drift = std::hypot(scaledOffsetX - nativeOffsetX, scaledOffsetY - nativeOffsetY);
        std::cout << "[UpscaleFraming] RenderScale50: coat-to-body offset drift " << drift << " body radii (coat "
                  << scaled.Coat.Count << "px, body " << scaled.Body.Count << "px)\n";
        EXPECT_LT(drift, 0.12) << "the coat is off its body under a 0.5 render scale: coat (" << scaled.Coat.X << ", "
                               << scaled.Coat.Y << ") vs body (" << scaled.Body.X << ", " << scaled.Body.Y
                               << "); at native coat (" << native.Coat.X << ", " << native.Coat.Y << ") vs body ("
                               << native.Body.X << ", " << native.Body.Y << ")";
    }
} // namespace OloEngine::Tests
