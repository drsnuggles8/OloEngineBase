// OLO_TEST_LAYER: L8
// =============================================================================
// DynamicRenderScaleEvidenceTest.cpp — the dynamic render scale on every
// raster path (issue #1537).
//
// Renderer3D::SetRenderScale keeps every display-sized target and draws the
// frame into its [0, render) corner. Deferred used to write its G-Buffer at
// full size and read every screen-space input through [0, 1] UVs, so the
// corner held a quarter of a full-size frame. This renders one scene on
// {Forward, Forward+, Deferred, Deferred MSAA 4} at scales {1.0, 0.75, 0.5}
// and measures, in the rendered corner, three subjects against the same
// path's scale-1.0 frame: a red cube and a blue cube (geometry framing), and
// the warm pool a point light throws on the floor (the light-cluster lookup,
// which maps a fragment to its cluster through the rendered extent). Each
// must sit at native * scale with native * scale^2 of the area.
//
// Evidence: OloEditor/assets/tests/visual/DynamicRenderScale_<Backend>_<Path>_Scale<NN>.png,
// the rendered corner, top row first. Runs in the Vulkan process too
// (RunsInVulkanProcess), so each cell has a GL and a Vulkan artefact.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestRenderBackend.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 400;
        constexpr f32 kCaptureTime = 4.0f;
        constexpr u32 kSettleFrames = 3;
        constexpr u32 kPixelFloor = 60;

        struct ScaleCase
        {
            RenderingPath Path;
            u32 DeferredSamples;
            const char* Name;
        };

        struct Blob
        {
            u32 Count = 0;
            f64 X = 0.0;
            f64 Y = 0.0;
        };

        // The rendered corner, top row first.
        struct Frame
        {
            u32 Width = 0;
            u32 Height = 0;
            std::vector<u8> Rgba;
        };

        [[nodiscard]] bool IsRed(const u8* p)
        {
            return p[0] > 70 && p[0] > 2 * p[1] && p[0] > 2 * p[2];
        }
        [[nodiscard]] bool IsBlue(const u8* p)
        {
            return p[2] > 70 && p[2] > 2 * p[0] && p[2] > p[1] + 30;
        }
        // The point light's warm pool on the grey floor, (198, 176, 133) at its
        // centre: bright, warm, green close to red. The red cube fails the
        // green test, the blue cube and the grey floor the warmth tests.
        [[nodiscard]] bool IsWarmPool(const u8* p)
        {
            return p[0] > 140 && p[0] > p[2] + 30 && p[1] > p[2] + 15 && p[1] * 10 > p[0] * 7;
        }

        template<typename Pred>
        [[nodiscard]] Blob Measure(const Frame& f, Pred pred)
        {
            Blob b;
            f64 sx = 0.0;
            f64 sy = 0.0;
            for (u32 y = 0; y < f.Height; ++y)
            {
                for (u32 x = 0; x < f.Width; ++x)
                {
                    if (pred(&f.Rgba[(static_cast<sizet>(y) * f.Width + x) * 4u]))
                    {
                        ++b.Count;
                        sx += x;
                        sy += y;
                    }
                }
            }
            if (b.Count > 0)
            {
                b.X = sx / b.Count;
                b.Y = sy / b.Count;
            }
            return b;
        }

        // A subject at native * scale: near the scaled centroid and inside a
        // band around the scaled area (UpscaleFramingEvidenceTest's rule).
        [[nodiscard]] std::string CompareScaled(const char* name, const Blob& native, const Blob& got, f64 scale)
        {
            std::string failures;
            const f64 expectedCount = static_cast<f64>(native.Count) * scale * scale;
            const f64 ratio = static_cast<f64>(got.Count) / std::max(expectedCount, 1.0);
            if (got.Count == 0u || ratio < 0.70 || ratio > 1.35)
            {
                failures += std::string(name) + ": " + std::to_string(got.Count) + " px, expected about " +
                            std::to_string(expectedCount) + "\n";
            }
            const f64 expectedX = ((native.X + 0.5) * scale) - 0.5;
            const f64 expectedY = ((native.Y + 0.5) * scale) - 0.5;
            const f64 moved = std::hypot(got.X - expectedX, got.Y - expectedY);
            if (moved > 1.5 + (1.0 / scale))
            {
                failures += std::string(name) + ": centroid (" + std::to_string(got.X) + ", " + std::to_string(got.Y) +
                            ") vs expected (" + std::to_string(expectedX) + ", " + std::to_string(expectedY) +
                            "), off by " + std::to_string(moved) + " px\n";
            }
            return failures;
        }

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

    class DynamicRenderScaleEvidenceTest : public RendererAttachedTest, public ::testing::WithParamInterface<ScaleCase>
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();
            scene.SetLightGizmosVisible(false);
            scene.SetGridVisible(false);
            scene.SetWorldAxisHelperVisible(false);

            const auto addMesh = [&scene](const char* name, const glm::vec3& at, const glm::vec3& scale,
                                          const glm::vec3& color)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = at;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (const Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(color, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.9f);
            };

            addMesh("Floor", { 0.0f, -0.55f, 0.0f }, { 14.0f, 0.1f, 10.0f }, glm::vec3(0.35f));
            // Off-centre on purpose: a frame drawn at the wrong scale moves an
            // off-centre subject a long way.
            addMesh("RedCube", { -2.6f, 0.0f, -1.2f }, glm::vec3(1.0f), { 0.85f, 0.05f, 0.04f });
            addMesh("BlueCube", { 2.4f, 0.0f, 1.3f }, glm::vec3(1.0f), { 0.04f, 0.10f, 0.90f });

            {
                Entity sun = scene.CreateEntity("Sun");
                auto& dl = sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f));
                dl.m_Color = glm::vec3(1.0f);
                dl.m_Intensity = 1.2f;
            }
            {
                // The warm pool: a short-range point light low over an empty
                // stretch of floor, off-centre the other way.
                Entity lamp = scene.CreateEntity("Lamp");
                lamp.GetComponent<TransformComponent>().Translation = { 1.2f, 0.3f, -2.0f };
                auto& pl = lamp.AddComponent<PointLightComponent>();
                pl.m_Color = { 1.0f, 0.55f, 0.15f };
                pl.m_Intensity = 25.0f;
                pl.m_Range = 2.5f;
            }
        }

        [[nodiscard]] static EditorCamera Camera()
        {
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose({ 0.0f, 6.5f, 7.5f }, 0.0f, 0.72f);
            return camera;
        }

        // Renders at the current scale and returns the rendered corner of the
        // composited frame, top row first. The corner is texel (0, 0) on both
        // backends, the bottom-left of the image on GL and the top-left on
        // Vulkan (RHI::RenderTargetRowsAreBottomUp); the readback returns GL
        // row order on both, so on Vulkan the corner is its last rows.
        void Capture(f32 scale, Frame& out)
        {
            const EditorCamera camera = Camera();
            RunEditorFrames(camera, kSettleFrames);
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            ASSERT_TRUE(fb) << "No composited framebuffer";
            ASSERT_EQ(fb->GetSpecification().Width, kWidth) << "the dynamic scale must not reallocate the target";
            std::vector<u8> full;
            ReadbackRgba8(fb->GetColorAttachmentHandle(0), kWidth, kHeight, full);
            ASSERT_EQ(full.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            out.Width = static_cast<u32>(std::floor(static_cast<f32>(kWidth) * scale));
            out.Height = static_cast<u32>(std::floor(static_cast<f32>(kHeight) * scale));
            out.Rgba.assign(static_cast<sizet>(out.Width) * out.Height * 4u, 0u);
            const u32 firstRow = RHI::RenderTargetRowsAreBottomUp() ? 0u : kHeight - out.Height;
            for (u32 row = 0; row < out.Height; ++row)
            {
                const u8* src = full.data() + (static_cast<sizet>(firstRow + row) * kWidth * 4u);
                u8* dst = out.Rgba.data() + (static_cast<sizet>(out.Height - 1u - row) * out.Width * 4u);
                std::memcpy(dst, src, static_cast<sizet>(out.Width) * 4u);
            }
        }

        static void Save(const std::string& cell, const Frame& f)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / (cell + ".png")).string();
            EXPECT_NE(::stbi_write_png(path.c_str(), static_cast<int>(f.Width), static_cast<int>(f.Height), 4,
                                       f.Rgba.data(), static_cast<int>(f.Width) * 4),
                      0)
                << path;
        }
    };

    TEST_P(DynamicRenderScaleEvidenceTest, EveryPathKeepsItsFramingAndLightingAtSubScale)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScaleCase& param = GetParam();
        if (param.DeferredSamples > 1u && param.DeferredSamples > Renderer3D::GetMaxMSAASamples())
            GTEST_SKIP() << "The device cannot render " << param.DeferredSamples << " samples";

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
        } const mockTime(kCaptureTime);

        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = param.Path;
        settings.Deferred.MSAASampleCount = param.DeferredSamples;
        Renderer3D::ApplyRendererSettings();

        const std::string cellBase = TestRenderBackend::EvidenceName("DynamicRenderScale") + param.Name;

        Frame native;
        {
            const ScopedRenderScale scale(1.0f);
            Capture(1.0f, native);
            if (HasFatalFailure())
                return;
        }
        Save(cellBase + "_Scale100", native);
        const Blob nativeRed = Measure(native, IsRed);
        const Blob nativeBlue = Measure(native, IsBlue);
        const Blob nativePool = Measure(native, IsWarmPool);
        std::cout << "[DynamicRenderScale] " << cellBase << "_Scale100: red " << nativeRed.Count << "px@(" << nativeRed.X
                  << "," << nativeRed.Y << "), blue " << nativeBlue.Count << "px@(" << nativeBlue.X << "," << nativeBlue.Y
                  << "), pool " << nativePool.Count << "px@(" << nativePool.X << "," << nativePool.Y << ")\n";
        ASSERT_GE(nativeRed.Count, kPixelFloor) << "native red cube";
        ASSERT_GE(nativeBlue.Count, kPixelFloor) << "native blue cube";
        ASSERT_GE(nativePool.Count, kPixelFloor) << "native point-light pool";

        for (const f32 s : { 0.75f, 0.5f })
        {
            const std::string cell = cellBase + "_Scale" + std::to_string(static_cast<int>(std::lround(s * 100.0f)));
            SCOPED_TRACE(cell);
            Frame got;
            {
                const ScopedRenderScale scale(s);
                Capture(s, got);
                if (HasFatalFailure())
                    return;
            }
            Save(cell, got);
            const Blob red = Measure(got, IsRed);
            const Blob blue = Measure(got, IsBlue);
            const Blob pool = Measure(got, IsWarmPool);
            std::cout << "[DynamicRenderScale] " << cell << " (" << got.Width << "x" << got.Height << "): red "
                      << red.Count << "px@(" << red.X << "," << red.Y << "), blue " << blue.Count << "px@(" << blue.X
                      << "," << blue.Y << "), pool " << pool.Count << "px@(" << pool.X << "," << pool.Y << ")\n";
            const f64 scale = static_cast<f64>(got.Width) / static_cast<f64>(kWidth);
            EXPECT_EQ(CompareScaled("red cube", nativeRed, red, scale), "");
            EXPECT_EQ(CompareScaled("blue cube", nativeBlue, blue, scale), "");
            EXPECT_EQ(CompareScaled("point-light pool", nativePool, pool, scale), "");
        }
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, DynamicRenderScaleEvidenceTest,
                             ::testing::Values(ScaleCase{ RenderingPath::Forward, 1u, "Forward" },
                                               ScaleCase{ RenderingPath::ForwardPlus, 1u, "ForwardPlus" },
                                               ScaleCase{ RenderingPath::Deferred, 1u, "Deferred" },
                                               ScaleCase{ RenderingPath::Deferred, 4u, "Deferred4x" }),
                             [](const auto& info)
                             { return info.param.Name; });
} // namespace OloEngine::Tests
