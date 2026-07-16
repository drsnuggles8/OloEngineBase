// OLO_TEST_LAYER: integration
//
// HW-vs-SW raster parity for the virtualized-geometry path, on a subject that can
// actually TELL THEM APART (issue #629).
//
// The pre-existing parity test (VirtualGeometryVisualEvidence.SoftwareRasterizerMatchesHardwareRaster)
// renders an UNTEXTURED red icosphere and compares red-pixel COVERAGE at a 10%
// tolerance. That metric cannot see a shading difference at all: it sails straight
// through an inverted normal map, which is exactly the bug that shipped —
// VirtualVisibilityResolve.glsl (the software rasterizer's material resolve) had
// hand-copied getNormalFromMap out of PBRCommon.glsl and DRIFTED from it:
//
//   * it sampled tangent-space z from the blue channel instead of reconstructing it
//     from xy — re-introducing issue #440 for every software-rasterized cluster (a
//     BC5/RGTC two-channel normal map has blue = 0 on the GPU, so z came out -1 and
//     the normal INVERTED), and
//   * it built the bitangent as +cross(N,T) where PBRCommon uses -cross(N,T) — the
//     opposite handedness, i.e. a flipped green channel.
//
// Both halves of the SAME frame therefore shaded the same material differently, and
// the software rasterizer is on by DEFAULT (Auto, 24px), so it owns most virtual
// pixels. The fix hoists ONE decode + ONE TBN into PBRCommon (decodeTangentNormal /
// applyNormalMapTBN) which both paths call, so they cannot drift again.
//
// This test therefore uses:
//   * a NORMAL-MAPPED, TEXTURED, TWO-SIDED subject — a dense grid sheet with a
//     uniform tangent-space normal whose blue channel is ZERO (the BC5 case) and
//     whose green channel is far from neutral (so a flipped bitangent shows up),
//   * a PER-CHANNEL RGB diff, not a coverage count,
//   * and BOTH software rasterizers: the single-pass 64-bit-atomic one and the
//     portable two-pass 2x32 fallback (SetForcePortableSwRaster). The old coverage
//     test was the only thing that ever ran the portable path; it has been deleted
//     and its unique value folded in here, under the bit-exact metric.
//
// Both maps are UNIFORM, so texture filtering / mip selection cannot contribute a
// difference: every remaining per-channel delta between the hardware frame and the
// software frame is a genuine shading divergence.
//
// It also covers the two-sidedness fix (kFlagTwoSided): an OPAQUE two-sided sheet
// viewed FROM BEHIND must survive both the cull's normal-cone backface rejection
// and the software rasterizer's negative-area cull.
//
// SKIPs cleanly with no GL 4.6 context (suite gate).

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
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
        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;

        // A dense grid sheet in the XY plane facing +Z, with real UVs.
        //
        // Flat and single-normal on purpose: the tangent frame then comes ENTIRELY from
        // the UV parameterisation, so a wrong handedness or a wrong tangent-space z is
        // the only thing that can move the shaded normal. 64x64 quads = 8192 triangles,
        // which the builder clusterizes into a real multi-level DAG.
        Ref<MeshSource> MakeGridSheetMeshSource(u32 quadsPerSide)
        {
            TArray<Vertex> vertices;
            TArray<u32> indices;
            u32 const verts = quadsPerSide + 1;
            vertices.Reserve(static_cast<i32>(verts * verts));
            for (u32 y = 0; y < verts; ++y)
            {
                for (u32 x = 0; x < verts; ++x)
                {
                    f32 const u = static_cast<f32>(x) / static_cast<f32>(quadsPerSide);
                    f32 const v = static_cast<f32>(y) / static_cast<f32>(quadsPerSide);
                    vertices.Add(Vertex(glm::vec3(u * 2.0f - 1.0f, v * 2.0f - 1.0f, 0.0f),
                                        glm::vec3(0.0f, 0.0f, 1.0f),
                                        glm::vec2(u, v)));
                }
            }
            indices.Reserve(static_cast<i32>(quadsPerSide * quadsPerSide * 6));
            for (u32 y = 0; y < quadsPerSide; ++y)
            {
                for (u32 x = 0; x < quadsPerSide; ++x)
                {
                    u32 const i0 = y * verts + x;
                    u32 const i1 = i0 + 1;
                    u32 const i2 = i0 + verts;
                    u32 const i3 = i2 + 1;
                    // CCW when seen from +Z (the hardware front-face convention)
                    for (u32 const idx : { i0, i1, i3, i0, i3, i2 })
                    {
                        indices.Add(idx);
                    }
                }
            }
            return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
        }

        // Uniform RGBA8 texture. Uniform is the point: it removes filtering/mip
        // selection as a possible source of HW-vs-SW difference, so the RGB diff
        // measures shading only.
        Ref<Texture2D> MakeUniformTexture(u8 r, u8 g, u8 b, bool srgb)
        {
            TextureSpecification spec;
            spec.Width = 8;
            spec.Height = 8;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;
            spec.SRGB = srgb;
            Ref<Texture2D> texture = Texture2D::Create(spec);
            std::array<u8, 8 * 8 * 4> pixels{};
            for (sizet i = 0; i < pixels.size(); i += 4)
            {
                pixels[i + 0] = r;
                pixels[i + 1] = g;
                pixels[i + 2] = b;
                pixels[i + 3] = 255;
            }
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
            return texture;
        }

        void WriteEvidencePng(const std::string& name, const std::vector<u8>& rgba, u32 width, u32 height)
        {
            std::vector<u8> flipped(rgba.size()); // GL readback is bottom-up
            sizet const rowBytes = static_cast<sizet>(width) * 4;
            for (u32 y = 0; y < height; ++y)
            {
                std::memcpy(flipped.data() + static_cast<sizet>(y) * rowBytes,
                            rgba.data() + static_cast<sizet>(height - 1 - y) * rowBytes, rowBytes);
            }
            std::filesystem::create_directories("assets/tests/visual");
            std::string const path = "assets/tests/visual/" + name;
            EXPECT_NE(::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                       flipped.data(), static_cast<int>(rowBytes)),
                      0)
                << "failed to write evidence PNG " << path;
        }

        struct ChannelDiff
        {
            u32 ComparedPixels = 0; // interior pixels both frames agree are sheet
            f64 MeanAbs[3] = { 0.0, 0.0, 0.0 };
            u32 MaxAbs[3] = { 0, 0, 0 };
            u32 OverToleranceCount = 0; // pixels where any channel differs by > kChannelTolerance
        };

        constexpr u32 kChannelTolerance = 4; // 8-bit levels. The interior is bit-identical in practice.

        // "This pixel shows the sheet." The sheet is the only MAGENTA thing in the scene
        // (bright magenta albedo on the lit front, a dim magenta on the shadowed back —
        // hence a hue test rather than a brightness test: the back face measures ~(46,9,40),
        // so any `r > 40 && b > 40` style threshold silently sees NOTHING from behind).
        bool IsSheetPixel(const std::vector<u8>& f, sizet i)
        {
            i32 const r = f[i];
            i32 const g = f[i + 1];
            i32 const b = f[i + 2];
            return r > 15 && b > 15 && r > g + 8 && b > g + 8;
        }

        u32 CountSheetPixels(const std::vector<u8>& f)
        {
            u32 count = 0;
            for (sizet i = 0; i + 3 < f.size(); i += 4)
            {
                count += IsSheetPixel(f, i) ? 1u : 0u;
            }
            return count;
        }

        // Per-channel RGB diff over the sheet's INTERIOR: pixels both frames call sheet, and
        // whose 8 neighbours all do too.
        //
        // The erosion is what makes this a SHADING metric instead of a coverage metric. The
        // two rasterizers legitimately disagree about coverage on the silhouette — different
        // fill rules, and in ForceSoftware mode the compute rasterizer also drops individual
        // triangles whose screen bounding box exceeds its kMaxTriangleBBox cap, nicking the
        // sheet's outline. Those boundary pixels are sheet-vs-background comparisons and swamp
        // the mean (they contributed the entire ~2.1/255 mean and the max of 87 that this test
        // first reported, while every interior pixel was already bit-identical). Coverage is
        // asserted separately below; this measures whether the two paths SHADE the same
        // surface the same way, which is what the drifted TBN broke.
        ChannelDiff DiffInterior(const std::vector<u8>& a, const std::vector<u8>& b, u32 width, u32 height)
        {
            ChannelDiff diff;
            u64 sums[3] = { 0, 0, 0 };
            auto interior = [&](u32 x, u32 y)
            {
                for (i32 dy = -1; dy <= 1; ++dy)
                {
                    for (i32 dx = -1; dx <= 1; ++dx)
                    {
                        sizet const n = ((static_cast<sizet>(y) + dy) * width + (static_cast<sizet>(x) + dx)) * 4;
                        if (!IsSheetPixel(a, n) || !IsSheetPixel(b, n))
                        {
                            return false;
                        }
                    }
                }
                return true;
            };

            for (u32 y = 1; y + 1 < height; ++y)
            {
                for (u32 x = 1; x + 1 < width; ++x)
                {
                    if (!interior(x, y))
                    {
                        continue;
                    }
                    sizet const i = (static_cast<sizet>(y) * width + x) * 4;
                    ++diff.ComparedPixels;
                    bool over = false;
                    for (u32 c = 0; c < 3; ++c)
                    {
                        u32 const d =
                            static_cast<u32>(std::abs(static_cast<i32>(a[i + c]) - static_cast<i32>(b[i + c])));
                        sums[c] += d;
                        diff.MaxAbs[c] = std::max(diff.MaxAbs[c], d);
                        over = over || (d > kChannelTolerance);
                    }
                    diff.OverToleranceCount += over ? 1u : 0u;
                }
            }
            for (u32 c = 0; c < 3; ++c)
            {
                diff.MeanAbs[c] = diff.ComparedPixels > 0
                                      ? static_cast<f64>(sums[c]) / static_cast<f64>(diff.ComparedPixels)
                                      : 0.0;
            }
            return diff;
        }
    } // namespace

    class VirtualGeometryRasterParity : public RendererAttachedTest
    {
      public:
        void BuildScene() override
        {
            if (!Project::GetActive() || !Project::GetAssetManager())
            {
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::path const projectDir = fs::temp_directory_path() / "OloEngineVirtualGeometryRasterParity";
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Parity.oloproj");
                    proj << "Project:\n"
                            "  Name: VirtualGeometryRasterParity\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Parity.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false);
                Project::SetAssetManager(assetManager);
            }

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            // Sun tilted off every axis so the shaded colour is genuinely sensitive to the
            // normal — a head-on light would wash out a small normal perturbation.
            {
                Entity sun = scene.CreateEntity("Sun");
                auto& dl = sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.45f, -0.55f, -0.70f));
                dl.m_Color = glm::vec3(1.0f, 0.98f, 0.95f);
                dl.m_Intensity = 4.0f;
            }

            // Ground plane (per docs/agent-rules/single-mesh-visual-test-lighting.md)
            {
                Entity ground = scene.CreateEntity("Ground");
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -2.0f, 0.0f };
                tc.Scale = { 20.0f, 0.1f, 20.0f };
                auto& mc = ground.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                {
                    mc.m_MeshSource = cube->GetMeshSource();
                }
                ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(
                    glm::vec4(0.32f, 0.34f, 0.30f, 1.0f));
            }

            // The subject: a two-sided, textured, normal-mapped virtual sheet.
            {
                AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(MakeGridSheetMeshSource(64));

                Entity sheet = scene.CreateEntity("VirtualSheet");
                auto& tc = sheet.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 0.0f, 0.0f };
                tc.Scale = { 1.0f, 1.0f, 1.0f }; // uniform — enables the cull's normal-cone test

                auto& vm = sheet.AddComponent<VirtualMeshComponent>();
                vm.m_MeshSource = handle;
                vm.m_ErrorThresholdPixels = 1.0f;

                auto& mat = sheet.AddComponent<MaterialComponent>();
                Material& material = mat.m_Material;
                material.SetBaseColorFactor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                material.SetRoughnessFactor(0.35f); // glossy enough that the normal really matters
                material.SetMetallicFactor(0.0f);

                // TEXTURED: a magenta albedo map (sRGB colour data), uniform so sampling
                // cannot contribute to the HW/SW diff.
                material.SetAlbedoMap(MakeUniformTexture(230, 40, 200, /*srgb*/ true));

                // NORMAL-MAPPED, and deliberately the pathological case:
                //   blue  = 0   -> a two-channel BC5/RGTC-style map. Reconstructing z gives
                //                  +0.585; SAMPLING the blue channel gives z = -1, i.e. the
                //                  normal points INTO the surface (issue #440).
                //   green = 40  -> tangent-space y = -0.686, a large off-neutral component, so
                //                  an inverted bitangent (the other half of the drift) also
                //                  moves the shaded normal a long way.
                material.SetNormalMap(MakeUniformTexture(220, 40, 0, /*srgb*/ false));
                material.SetNormalScale(1.0f);

                // OPAQUE + two-sided: opaque keeps it eligible for the software rasterizer
                // (alpha-masked parts are forced onto the hardware path), and two-sided is
                // what the cone cull + the SW backface cull have to honour.
                material.SetAlphaMode(AlphaMode::Opaque);
                material.SetFlag(MaterialFlag::TwoSided, true);

                m_SheetEntity = sheet;
            }
        }

        Entity m_SheetEntity;
        u32 m_CaptureWidth = 0;
        u32 m_CaptureHeight = 0;

        std::vector<u8> CaptureMode(VirtualSwRasterMode mode, const char* pngName, const glm::vec3& position, f32 yaw)
        {
            VirtualMeshRegistry::Get().SetSwRasterMode(mode);

            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, 0.0f);
            RunEditorFrames(camera, 3);

            std::vector<u8> rgba;
            u32 w = 0;
            u32 h = 0;
            if (!ReadbackComposite(rgba, w, h))
            {
                ADD_FAILURE() << "composite readback unavailable for " << pngName;
                return {};
            }
            m_CaptureWidth = w;
            m_CaptureHeight = h;
            WriteEvidencePng(std::string("VirtualGeometryParity_") + pngName + ".png", rgba, w, h);
            return rgba;
        }

        void TearDown() override
        {
            VirtualMeshRegistry::Get().SetSwRasterMode(VirtualSwRasterMode::Auto);
            VirtualMeshRegistry::Get().SetForcePortableSwRaster(false);
            RendererAttachedTest::TearDown();
        }
    };

    // C4: the software rasterizer's material resolve must shade a normal-mapped,
    // textured surface IDENTICALLY to the hardware raster — per channel, not by
    // coverage. Fails loudly on the pre-fix resolve shader (sampled tangent-space z
    // + inverted bitangent both move the shaded normal by tens of 8-bit levels).
    TEST_F(VirtualGeometryRasterParity, SoftwareRasterShadesNormalMappedSurfaceLikeHardware)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // Camera in FRONT of the sheet (+Z), looking at its front faces.
        glm::vec3 const eye{ 0.0f, 0.0f, 3.2f };
        std::vector<u8> const hardware = CaptureMode(VirtualSwRasterMode::Disabled, "NormalMap_HW", eye, 0.0f);
        std::vector<u8> const software = CaptureMode(VirtualSwRasterMode::ForceSoftware, "NormalMap_SW", eye, 0.0f);

        ASSERT_EQ(hardware.size(), software.size());
        u32 const hwSheet = CountSheetPixels(hardware);
        u32 const swSheet = CountSheetPixels(software);
        ASSERT_GT(hwSheet, 5000u) << "hardware baseline did not render the sheet";
        ASSERT_GT(swSheet, 5000u) << "software rasterizer did not render the sheet (visbuffer/resolve broken)";

        // Coverage agreement, kept deliberately loose: the compute rasterizer drops individual
        // triangles over its screen-bbox cap, which nicks the silhouette in ForceSoftware mode.
        // This is here only to catch "the SW path lost most of the surface".
        EXPECT_GT(static_cast<f64>(swSheet) / static_cast<f64>(hwSheet), 0.90)
            << "software raster covered only " << swSheet << " of hardware's " << hwSheet << " sheet pixels";

        // Shading parity on the interior. Measured: every interior pixel is BIT-IDENTICAL
        // between the two paths once they share PBRCommon's decodeTangentNormal +
        // applyNormalMapTBN, so the bar is set at "essentially exact" rather than a loose
        // tolerance. The pre-fix resolve shader (blue-channel z + inverted bitangent) moved
        // the shaded normal across the WHOLE surface, not just its edges.
        ChannelDiff const diff = DiffInterior(hardware, software, m_CaptureWidth, m_CaptureHeight);
        ASSERT_GT(diff.ComparedPixels, 5000u) << "not enough interior pixels to compare";

        for (u32 c = 0; c < 3; ++c)
        {
            EXPECT_LT(diff.MeanAbs[c], 0.5)
                << "channel " << c << ": software raster shades the normal-mapped sheet differently from hardware "
                << "(mean |delta| = " << diff.MeanAbs[c] << "/255, max = " << diff.MaxAbs[c] << " over "
                << diff.ComparedPixels << " interior pixels). The two paths must share PBRCommon's "
                << "decodeTangentNormal + applyNormalMapTBN — check for a re-hand-rolled TBN.";
        }
        EXPECT_EQ(diff.OverToleranceCount, 0u)
            << diff.OverToleranceCount << " of " << diff.ComparedPixels
            << " interior pixels differ by more than " << kChannelTolerance << " levels in some channel — that is a "
            << "shading divergence, not an edge-rule difference";

        // ── The PORTABLE two-pass 2x32 software rasterizer ──
        //
        // There are TWO software rasterizers: a single-pass one that needs 64-bit shader
        // atomics, and a portable two-pass 2x32 fallback for drivers without them. The
        // routing picks the first when available, so on this machine everything above only
        // tested that one. The deleted coverage test (VirtualGeometryVisualEvidence.
        // SoftwareRasterizerMatchesHardwareRaster) was the ONLY thing that ever exercised the
        // portable path — by red-pixel coverage at 10% tolerance, a metric that cannot see a
        // shading difference at all. Fold it in here, under the bit-exact metric: on
        // int64-capable hardware BOTH rasterizers get compared to hardware, per channel.
        //
        // The fixture's TearDown resets the flag (it used to reset a flag nothing ever set).
        if (RenderCommand::SupportsInt64ShaderAtomics())
        {
            VirtualMeshRegistry::Get().SetForcePortableSwRaster(true);
            std::vector<u8> const portable = CaptureMode(VirtualSwRasterMode::ForceSoftware, "NormalMap_SWPortable",
                                                         eye, 0.0f);
            VirtualMeshRegistry::Get().SetForcePortableSwRaster(false);

            ASSERT_EQ(hardware.size(), portable.size());
            u32 const portableSheet = CountSheetPixels(portable);
            ASSERT_GT(portableSheet, 5000u)
                << "the PORTABLE two-pass software rasterizer rendered nothing (the visbuffer's 2x32 depth/payload "
                   "pack or its resolve is broken) — this path is what runs on every driver without 64-bit shader "
                   "atomics";

            ChannelDiff const portableDiff = DiffInterior(hardware, portable, m_CaptureWidth, m_CaptureHeight);
            ASSERT_GT(portableDiff.ComparedPixels, 5000u) << "not enough interior pixels to compare";
            for (u32 c = 0; c < 3; ++c)
            {
                EXPECT_LT(portableDiff.MeanAbs[c], 0.5)
                    << "channel " << c << ": the PORTABLE (2x32) software rasterizer shades the normal-mapped sheet "
                    << "differently from hardware (mean |delta| = " << portableDiff.MeanAbs[c] << "/255, max = "
                    << portableDiff.MaxAbs[c] << " over " << portableDiff.ComparedPixels << " interior pixels)";
            }
            EXPECT_EQ(portableDiff.OverToleranceCount, 0u)
                << portableDiff.OverToleranceCount << " of " << portableDiff.ComparedPixels
                << " interior pixels differ by more than " << kChannelTolerance
                << " levels between hardware and the PORTABLE software rasterizer";
        }
        else
        {
            // Nothing skipped: with no int64 atomics the portable path IS the path measured above.
            OLO_CORE_INFO("VirtualGeometryRasterParity: no int64 shader atomics — the software frame above already "
                          "IS the portable 2x32 rasterizer");
        }
    }

    // C5: an OPAQUE, TWO-SIDED sheet seen FROM BEHIND. Two independent things used to
    // drop it, both virtual-path-only (the classic path has no cone cull and no SW raster):
    //   (a) VirtualClusterCull.comp's normal-cone rejection dropped clusters whose
    //       triangles all face away — every cluster of a flat sheet viewed from behind.
    //   (b) VirtualClusterRaster.comp hard-culled negative-area (back-facing) triangles,
    //       so the software path drew nothing at all.
    // Foliage was masked from both only because it is alpha-masked, hence hardware-only.
    TEST_F(VirtualGeometryRasterParity, TwoSidedOpaqueSheetIsVisibleFromBehindOnBothRasterPaths)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // Camera BEHIND the sheet (-Z), turned around to look back at it: every triangle
        // is back-facing.
        glm::vec3 const eye{ 0.0f, 0.0f, -3.2f };
        f32 const yaw = glm::radians(180.0f);

        std::vector<u8> const hardware = CaptureMode(VirtualSwRasterMode::Disabled, "TwoSidedBack_HW", eye, yaw);
        u32 const hwSheet = CountSheetPixels(hardware);
        EXPECT_GT(hwSheet, 5000u)
            << "two-sided sheet vanished from the HARDWARE path when viewed from behind — the cull's normal-cone "
            << "backface rejection dropped its clusters (kFlagTwoSided must skip the cone test)";

        std::vector<u8> const software = CaptureMode(VirtualSwRasterMode::ForceSoftware, "TwoSidedBack_SW", eye, yaw);
        u32 const swSheet = CountSheetPixels(software);
        EXPECT_GT(swSheet, 5000u)
            << "two-sided sheet vanished from the SOFTWARE path when viewed from behind — the compute rasterizer "
            << "culled its negative-signed-area triangles (kFlagTwoSided must keep them)";

        // And the back faces must agree between the two rasterizers too — same shared TBN,
        // and the resolve's ScreenBarycentrics must handle a NEGATIVE signed area (it used to
        // divide by max(area, 1e-12), which turned every back-face barycentric into ~1e12).
        ASSERT_EQ(hardware.size(), software.size());
        ChannelDiff const diff = DiffInterior(hardware, software, m_CaptureWidth, m_CaptureHeight);
        ASSERT_GT(diff.ComparedPixels, 5000u) << "not enough interior back-face pixels to compare";
        for (u32 c = 0; c < 3; ++c)
        {
            EXPECT_LT(diff.MeanAbs[c], 0.5)
                << "channel " << c << ": back-face shading diverges between the raster paths (mean |delta| = "
                << diff.MeanAbs[c] << "/255, max = " << diff.MaxAbs[c] << " over " << diff.ComparedPixels
                << " interior pixels)";
        }
        EXPECT_EQ(diff.OverToleranceCount, 0u);
    }
} // namespace OloEngine::Tests
