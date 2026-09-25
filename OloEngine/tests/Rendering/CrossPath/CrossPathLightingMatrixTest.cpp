// OLO_TEST_LAYER: integration
// =============================================================================
// CrossPathLightingMatrixTest.cpp — one fixture, every declared arm, separated
// lighting terms (issue #1347, acceptance criteria 3 and 5).
//
// 31 of the 73 bug-like issues filed 2026-08-28..09-25 named a backend or
// path divergence (#1457, #1470, #1472, #1417, ...). Each was found by a
// person looking at two paths side by side. This fixture does that for every
// row x every arm the support registry declares:
//
//   * ARMS come from RendererSupport::PathCoverageRows (CrossPathMatrix.h):
//     GL / Vulkan x Forward / Forward+ / Deferred, plus the Deferred MSAA,
//     spatial-upscale and temporal-upscale rows. Undeclared, Vulkan and
//     ray-query arms skip with the reason (see PlanArm).
//   * ROWS are scenes that separate one lighting term each: the term's source
//     is switched off and on and the linear HDR SceneColor is differenced, so
//     ON - OFF is that term alone on every path (CrossPathMatrix.h says why
//     this, and not a debug view, is the AOV).
//   * Each separated term is compared (a) per region against an ANALYTIC
//     expectation where one exists — the independent BSDF oracle of
//     Rendering/Oracles/IndependentBsdfOracle.h, evaluated per pixel with the
//     camera's real view direction — and (b) against the reference arm
//     (GL Forward native) rendered in the same test.
//   * INVARIANCE probes hold one term and switch an estimator that does not
//     own it (AO, SSGI): the term must not move. Positive controls switch one
//     that does: it must.
//
// ADDING A ROW: append a SceneRow to Rows() below with a Build function, one or
// more TermProbes (a SetSource that switches exactly one term's source, the
// ground regions to measure, and tolerances with their reasons) and, if the
// row needs a path-specific estimator, a Requires toggle. Every arm runs it.
// docs/testing.md §6.4 has the walkthrough and the table of which arms skip
// where.
//
// Evidence: every run writes CrossPathMatrix_<Row>_<Probe>_<Cell>.png — the
// separated term, tone-mapped x/(1+x) — under OloEditor/assets/tests/visual/,
// because a numeric tolerance can pass two identically broken arms.
//
// GL arms only in-process (a GL 4.6 context; the fixture skips cleanly without
// one). The Vulkan arms are run live by scripts/cross-path-matrix-live.py.
// =============================================================================

#include "OloEnginePCH.h"

#include "Rendering/CrossPath/CrossPathMatrix.h"
#include "Rendering/Oracles/IndependentBsdfOracle.h"
#include "Rendering/PropertyTests/RenderPropertyTest.h"
#include "Rendering/PropertyTests/RendererAttachedTest.h"
#include "TestOptions.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace OloEngine::Tests::CrossPath
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 384;
        constexpr u32 kHeight = 256;
        constexpr f32 kCaptureTime = 4.0f;
        constexpr u32 kFramesPerCapture = 8;
        constexpr u32 kFramesPerTemporalCapture = 32; // FSR2 history re-accumulates after the reset in Capture

        // The camera: straight down from kEyeHeight, vertical FOV kFovY. Every
        // pixel's ground point follows from these, which is what lets a region
        // be declared in world units and measured at any render resolution.
        constexpr f64 kEyeHeight = 8.0;
        constexpr f64 kFovY = 60.0 * std::numbers::pi / 180.0;
        const glm::dvec3 kEye{ 0.0, kEyeHeight, 0.0 };

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

        // ---- linear readback ------------------------------------------------

        struct LinearImage
        {
            std::string_view Target;   // the render-graph resource that was read
            std::string ReadAfterPass; // the pass it was read after (in-frame), for the live replay
            u32 Width = 0;
            u32 Height = 0;
            std::vector<f32> Rgba; // rows top-down
        };

        [[nodiscard]] glm::dvec3 Texel(const LinearImage& image, u32 x, u32 y)
        {
            const sizet i = (static_cast<sizet>(y) * image.Width + x) * 4u;
            return { image.Rgba[i], image.Rgba[i + 1], image.Rgba[i + 2] };
        }

        // The ground point a pixel of a `width` x `height` SceneColor sees.
        // Camera pitch -90 degrees: view +x -> world +x, view +y -> world -z.
        [[nodiscard]] glm::dvec3 GroundPoint(u32 x, u32 y, u32 width, u32 height)
        {
            const f64 aspect = static_cast<f64>(kWidth) / static_cast<f64>(kHeight);
            const f64 tanHalf = std::tan(0.5 * kFovY);
            const f64 ndcX = 2.0 * (static_cast<f64>(x) + 0.5) / static_cast<f64>(width) - 1.0;
            const f64 ndcY = 1.0 - 2.0 * (static_cast<f64>(y) + 0.5) / static_cast<f64>(height);
            return { ndcX * tanHalf * aspect * kEyeHeight, 0.0, -ndcY * tanHalf * kEyeHeight };
        }

        [[nodiscard]] bool Inside(const GroundRegion& region, const glm::dvec3& p)
        {
            return p.x >= region.Min.x && p.x <= region.Max.x && p.z >= region.Min.y && p.z <= region.Max.y;
        }

        struct RegionStats
        {
            u64 Pixels = 0;
            glm::dvec3 Mean{ 0.0 };
            glm::dvec3 AnalyticMean{ 0.0 };
            u64 AnalyticPixels = 0;
            bool NonFinite = false;
        };

        [[nodiscard]] RegionStats Measure(const LinearImage& on, const LinearImage& off, const GroundRegion& region)
        {
            RegionStats stats;
            for (u32 y = 0; y < on.Height; ++y)
            {
                for (u32 x = 0; x < on.Width; ++x)
                {
                    const glm::dvec3 p = GroundPoint(x, y, on.Width, on.Height);
                    if (!Inside(region, p))
                        continue;
                    const glm::dvec3 term = Texel(on, x, y) - Texel(off, x, y);
                    if (!std::isfinite(term.x) || !std::isfinite(term.y) || !std::isfinite(term.z))
                    {
                        stats.NonFinite = true;
                        continue;
                    }
                    stats.Mean += term;
                    ++stats.Pixels;
                    if (region.Analytic)
                    {
                        if (const auto expected = region.Analytic(p, kEye))
                        {
                            stats.AnalyticMean += *expected;
                            ++stats.AnalyticPixels;
                        }
                    }
                }
            }
            if (stats.Pixels > 0)
                stats.Mean /= static_cast<f64>(stats.Pixels);
            if (stats.AnalyticPixels > 0)
                stats.AnalyticMean /= static_cast<f64>(stats.AnalyticPixels);
            return stats;
        }

        [[nodiscard]] bool Within(const glm::dvec3& measured, const glm::dvec3& expected, const Tolerance& tol,
                                  f64& worstRelative)
        {
            bool ok = true;
            for (int c = 0; c < 3; ++c)
            {
                const f64 err = std::abs(measured[c] - expected[c]);
                worstRelative = std::max(worstRelative, err / std::max(std::abs(expected[c]), 1.0e-9));
                ok = ok && err <= tol.Relative * std::abs(expected[c]) + tol.Absolute;
            }
            return ok;
        }

        [[nodiscard]] std::string Str(const glm::dvec3& v)
        {
            std::ostringstream s;
            s.precision(5);
            s << "(" << v.x << ", " << v.y << ", " << v.z << ")";
            return s.str();
        }

        // The separated term as evidence: x / (1 + x), then a 2.2 gamma.
        void WriteTermPng(const std::string& name, const LinearImage& on, const LinearImage& off)
        {
            std::vector<u8> rgba(static_cast<sizet>(on.Width) * on.Height * 4u, 255u);
            for (sizet i = 0; i < static_cast<sizet>(on.Width) * on.Height; ++i)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const f64 v = std::max(0.0, static_cast<f64>(on.Rgba[i * 4u + c] - off.Rgba[i * 4u + c]));
                    const f64 mapped = std::pow(v / (1.0 + v), 1.0 / 2.2);
                    rgba[i * 4u + c] = static_cast<u8>(std::clamp(mapped * 255.0 + 0.5, 0.0, 255.0));
                }
            }
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            fs::create_directories(dir);
            ::stbi_write_png((dir / (name + ".png")).string().c_str(), static_cast<int>(on.Width),
                             static_cast<int>(on.Height), 4, rgba.data(), static_cast<int>(on.Width) * 4);
        }

        // ---- export for the live Vulkan replay ---------------------------------

        // Up to 16 pixels on a 4 x 4 lattice inside a region, at the native
        // capture size. The live replay probes exactly these pixels, so the GL
        // reference it is compared with is the mean over the same pixels.
        [[nodiscard]] std::vector<glm::uvec2> SamplePixels(const GroundRegion& region)
        {
            std::vector<glm::uvec2> inside;
            for (u32 y = 0; y < kHeight; ++y)
                for (u32 x = 0; x < kWidth; ++x)
                    if (Inside(region, GroundPoint(x, y, kWidth, kHeight)))
                        inside.emplace_back(x, y);
            if (inside.empty())
                return inside;
            glm::uvec2 lo = inside.front();
            glm::uvec2 hi = inside.front();
            for (const glm::uvec2& p : inside)
            {
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
            std::vector<glm::uvec2> lattice;
            for (u32 j = 0; j < 4u; ++j)
                for (u32 i = 0; i < 4u; ++i)
                    lattice.emplace_back(lo.x + (hi.x - lo.x) * (2u * i + 1u) / 8u,
                                         lo.y + (hi.y - lo.y) * (2u * j + 1u) / 8u);
            return lattice;
        }

        [[nodiscard]] nlohmann::json ToJson(const glm::dvec3& v)
        {
            return nlohmann::json::array({ v.x, v.y, v.z });
        }

        [[nodiscard]] nlohmann::json ToJson(const Tolerance& t)
        {
            return { { "relative", t.Relative }, { "absolute", t.Absolute }, { "reason", std::string(t.Reason) } };
        }

        // Write <dir>/<Row>_<Probe>_{Off,On}.olo — the scene, with the renderer's
        // post-process and snow settings copied into it, since some sources
        // (GTAO, SSGI, snow) are renderer settings — and append the probe to
        // <dir>/<Row>.json. The render path is not in the scene: the replay
        // sets it per arm.
        // A row that needs a ray-query estimator (ReSTIR DI) is exported from GL
        // Deferred with the estimator OFF — the raster loop is its GL reference
        // — and its scenes carry the estimator ON, for the live Vulkan cell.
        void ExportProbe(Ref<Scene> sceneRef, const SceneRow& row, const TermProbe& probe,
                         const LinearImage& on, const LinearImage& off)
        {
            const bool liveEstimator = row.Requires.Estimator != LightingEstimator::None &&
                                       NeedsRayQueries(row.Requires.Estimator) && row.Requires.Set;
            Scene& scene = *sceneRef;
            const fs::path dir = Tests::Options().CrossPathExportDir;
            fs::create_directories(dir);
            const std::string stem = std::string(row.Name) + "_" + std::string(probe.Name);
            for (const bool state : { false, true })
            {
                probe.SetSource(scene, state);
                if (liveEstimator)
                    row.Requires.Set(true);
                scene.GetPostProcessSettings() = Renderer3D::GetPostProcessSettings();
                scene.GetSnowSettings() = Renderer3D::GetSnowSettings();
                if (liveEstimator)
                    row.Requires.Set(false);
                SceneSerializer(sceneRef).Serialize(dir / (stem + (state ? "_On.olo" : "_Off.olo")));
            }

            nlohmann::json regions = nlohmann::json::array();
            for (const GroundRegion& region : probe.Regions)
            {
                nlohmann::json pixels = nlohmann::json::array();
                glm::dvec3 gl(0.0);
                glm::dvec3 model(0.0);
                u32 modelCount = 0;
                const std::vector<glm::uvec2> lattice = SamplePixels(region);
                for (const glm::uvec2& p : lattice)
                {
                    pixels.push_back({ p.x, p.y });
                    gl += Texel(on, p.x, p.y) - Texel(off, p.x, p.y);
                    if (region.Analytic)
                    {
                        if (const auto e = region.Analytic(GroundPoint(p.x, p.y, kWidth, kHeight), kEye))
                        {
                            model += *e;
                            ++modelCount;
                        }
                    }
                }
                nlohmann::json entry = { { "name", std::string(region.Name) },
                                         { "pixels", pixels },
                                         { "glReference", ToJson(gl / static_cast<f64>(std::max<sizet>(lattice.size(), 1u))) } };
                entry["analytic"] = modelCount > 0 ? ToJson(model / static_cast<f64>(modelCount)) : nlohmann::json();
                regions.push_back(std::move(entry));
            }

            const fs::path manifestPath = dir / (std::string(row.Name) + ".json");
            nlohmann::json manifest;
            if (std::ifstream in(manifestPath); in)
                manifest = nlohmann::json::parse(in, nullptr, false);
            if (!manifest.is_object())
                manifest = nlohmann::json::object();
            manifest["row"] = std::string(row.Name);
            manifest["motivation"] = std::string(row.Motivation);
            manifest["width"] = kWidth;
            manifest["height"] = kHeight;
            manifest["requires"] = std::string(row.Requires.Name);
            // Only a ray-query row's estimator is forced on in the live replay;
            // an SSGI row's probe switches SSGI itself.
            manifest["estimatorField"] = liveEstimator ? std::string(row.Requires.McpField) : std::string();
            manifest["probes"][std::string(probe.Name)] = {
                { "term", std::string(ToString(probe.Term)) },
                // The render-graph resource the GL reference read: the live
                // replay probes the same one (the tone map's input, not
                // necessarily SceneColor).
                // The render-graph resource each state was read from, and the
                // pass it was read after: the live replay probes the same ones.
                // They differ when the source IS an estimator (SSGI off has no
                // SSGIColor; the OFF frame's scene band ends at SceneColor).
                { "target", std::string(on.Target) },
                { "targetPass", on.ReadAfterPass },
                { "targetOff", std::string(off.Target) },
                { "targetPassOff", off.ReadAfterPass },
                { "off", stem + "_Off.olo" },
                { "on", stem + "_On.olo" },
                { "analyticTolerance", ToJson(probe.Analytic) },
                { "crossArmTolerance", ToJson(probe.CrossArm) },
                { "regions", regions },
            };
            std::ofstream(manifestPath) << manifest.dump(2) << "\n";
        }

        // ---- scene building -------------------------------------------------

        struct TileSpec
        {
            const char* Name;
            glm::dvec2 Centre; // (x, z) on the ground
            f64 Size = 3.0;
            glm::vec3 Albedo{ 0.8f };
            f32 Metallic = 0.0f;
            f32 Roughness = 1.0f;
            PBRModel Model = PBRModel::Legacy;
            glm::vec3 Emissive{ 0.0f };
        };

        Entity AddTile(Scene& scene, const TileSpec& tile)
        {
            Entity e = scene.CreateEntity(tile.Name);
            auto& tc = e.GetComponent<TransformComponent>();
            tc.Translation = { static_cast<f32>(tile.Centre.x), 0.0f, static_cast<f32>(tile.Centre.y) };
            tc.Scale = { static_cast<f32>(tile.Size), 1.0f, static_cast<f32>(tile.Size) };
            auto& mc = e.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Plane;
            if (Ref<Mesh> mesh = MeshPrimitives::CreatePlane())
                mc.m_MeshSource = mesh->GetMeshSource();
            auto& mat = e.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(tile.Albedo, 1.0f));
            mat.m_Material.SetMetallicFactor(tile.Metallic);
            mat.m_Material.SetRoughnessFactor(tile.Roughness);
            mat.m_Material.SetPBRModel(tile.Model);
            mat.m_Material.SetEmissiveFactor(glm::vec4(tile.Emissive, 1.0f));
            return e;
        }

        Entity AddBox(Scene& scene, const char* name, const glm::vec3& position, const glm::vec3& scale,
                      const glm::vec3& albedo, const glm::vec3& emissive = glm::vec3(0.0f))
        {
            Entity e = scene.CreateEntity(name);
            auto& tc = e.GetComponent<TransformComponent>();
            tc.Translation = position;
            tc.Scale = scale;
            auto& mc = e.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                mc.m_MeshSource = mesh->GetMeshSource();
            auto& mat = e.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            mat.m_Material.SetMetallicFactor(0.0f);
            mat.m_Material.SetRoughnessFactor(0.9f);
            mat.m_Material.SetEmissiveFactor(glm::vec4(emissive, 1.0f));
            return e;
        }

        // The measured interior of a tile: 70 % of its extent, so neither the
        // tile's silhouette nor MSAA edge resolve is inside the region.
        [[nodiscard]] GroundRegion Interior(const TileSpec& tile, AnalyticFn analytic = {})
        {
            const f64 h = 0.35 * tile.Size;
            return { tile.Name, tile.Centre - glm::dvec2(h), tile.Centre + glm::dvec2(h), std::move(analytic) };
        }

        template<typename Component>
        Component* FindComponent(Scene& scene, std::string_view name)
        {
            auto view = scene.GetAllEntitiesWith<TagComponent, Component>();
            for (auto entity : view)
            {
                if (view.template get<TagComponent>(entity).Tag == name)
                    return &view.template get<Component>(entity);
            }
            return nullptr;
        }

        Material* FindMaterial(Scene& scene, std::string_view name)
        {
            auto* mc = FindComponent<MaterialComponent>(scene, name);
            return mc ? &mc->m_Material : nullptr;
        }

        // ---- analytic expectations (the independent oracle) ---------------

        // Outgoing radiance of one directional light off a ground tile, in the
        // oracle's local frame (n = +y in the world becomes +z locally):
        //   L = f(v, l) * E_perp * (n . l),  E_perp = colour * intensity.
        // f is the tile's versioned closure from IndependentBsdfOracle.h.
        // The single-scattering energy E(mu) of one alpha, integrated once by
        // the oracle's quadrature on a mu grid and interpolated linearly (E is
        // smooth in mu; the interpolation error at 129 nodes is < 1e-5, far
        // below the analytic tolerance). Per pixel it would cost a full
        // hemisphere quadrature, minutes per frame in a Debug build.
        struct EnergyCurve
        {
            std::vector<f64> E; // E((i) / (N - 1))
            f64 EAvg = 1.0;

            [[nodiscard]] f64 At(f64 mu) const
            {
                const f64 x = std::clamp(mu, 0.0, 1.0) * static_cast<f64>(E.size() - 1u);
                const auto i = std::min(static_cast<sizet>(x), E.size() - 2u);
                const f64 f = x - static_cast<f64>(i);
                return E[i] * (1.0 - f) + E[i + 1u] * f;
            }
        };

        [[nodiscard]] std::shared_ptr<const EnergyCurve> MakeEnergyCurve(f64 alpha)
        {
            auto curve = std::make_shared<EnergyCurve>();
            constexpr u32 kNodes = 129;
            curve->E.resize(kNodes);
            // mu = 0 exactly is the tangent view, where E -> 0; start a hair above.
            for (u32 i = 0; i < kNodes; ++i)
                curve->E[i] = Oracle::GgxDirectionalAlbedo(std::max(1.0e-3, static_cast<f64>(i) / (kNodes - 1u)),
                                                           alpha, 256, 64)
                                  .Value;
            // E_avg = 2 int E(mu) mu dmu, by the trapezoid rule on the same nodes.
            f64 sum = 0.0;
            for (u32 i = 0; i + 1u < kNodes; ++i)
            {
                const f64 m0 = static_cast<f64>(i) / (kNodes - 1u);
                const f64 m1 = static_cast<f64>(i + 1u) / (kNodes - 1u);
                sum += 0.5 * (curve->E[i] * m0 + curve->E[i + 1u] * m1) * (m1 - m0);
            }
            curve->EAvg = 2.0 * sum;
            return curve;
        }

        [[nodiscard]] AnalyticFn DirectionalLightAnalytic(const TileSpec& tile, const glm::dvec3& towardLight,
                                                          const glm::dvec3& radiance)
        {
            // Built on first use, not here: the row table is constructed at
            // static-initialisation time in every test process.
            struct LazyEnergy
            {
                std::once_flag Once;
                std::shared_ptr<const EnergyCurve> Curve;
            };
            auto lazy = std::make_shared<LazyEnergy>();
            return [tile, towardLight, radiance, lazy](const glm::dvec3& p,
                                                       const glm::dvec3& eye) -> std::optional<glm::dvec3>
            {
                std::shared_ptr<const EnergyCurve> energy;
                if (tile.Model == PBRModel::ClosureV2)
                {
                    std::call_once(lazy->Once, [&]
                                   { lazy->Curve = MakeEnergyCurve(Oracle::ClosureV2Alpha(tile.Roughness)); });
                    energy = lazy->Curve;
                }
                auto toLocal = [](const glm::dvec3& w)
                { return glm::dvec3(w.x, -w.z, w.y); };
                const glm::dvec3 v = toLocal(glm::normalize(eye - p));
                const glm::dvec3 l = toLocal(glm::normalize(towardLight));
                if (l.z <= 0.0 || v.z <= 0.0)
                    return glm::dvec3(0.0);
                const glm::dvec3 albedo(tile.Albedo);
                glm::dvec3 f;
                if (energy)
                {
                    const Oracle::ClosureV2Energies energies{ energy->At(v.z), energy->At(l.z), energy->EAvg };
                    f = Oracle::ClosureV2Brdf(v, l, albedo, tile.Metallic, tile.Roughness, energies);
                }
                else
                {
                    f = Oracle::LegacyBrdf(v, l, albedo, tile.Metallic, tile.Roughness);
                }
                return f * radiance * l.z;
            };
        }

        [[nodiscard]] AnalyticFn ConstantAnalytic(const glm::dvec3& value)
        {
            return [value](const glm::dvec3&, const glm::dvec3&) -> std::optional<glm::dvec3>
            { return value; };
        }

        // ---- rows -------------------------------------------------------------

        // Six tiles, three per row of the frame: Legacy on the top row, ClosureV2
        // on the bottom; white-rough (mostly diffuse), black dielectric (pure
        // specular: no diffuse term at all), coloured metal.
        const std::vector<TileSpec>& MaterialTiles()
        {
            static const std::vector<TileSpec> tiles = {
                { "LegacyWhiteRough", { -3.6, -1.9 }, 3.0, glm::vec3(0.8f), 0.0f, 1.0f, PBRModel::Legacy },
                { "LegacyBlackGloss", { 0.0, -1.9 }, 3.0, glm::vec3(0.0f), 0.0f, 0.35f, PBRModel::Legacy },
                { "LegacyGoldMetal", { 3.6, -1.9 }, 3.0, { 1.0f, 0.78f, 0.34f }, 1.0f, 0.5f, PBRModel::Legacy },
                { "V2WhiteRough", { -3.6, 1.9 }, 3.0, glm::vec3(0.8f), 0.0f, 0.6f, PBRModel::ClosureV2 },
                { "V2BlackGloss", { 0.0, 1.9 }, 3.0, glm::vec3(0.0f), 0.0f, 0.3f, PBRModel::ClosureV2 },
                { "V2CopperMetal", { 3.6, 1.9 }, 3.0, { 0.95f, 0.64f, 0.54f }, 1.0f, 0.4f, PBRModel::ClosureV2 },
            };
            return tiles;
        }

        // The sun travels along kSunDirection; the ground sees it at
        // acos(n . l) = 50 degrees. Intensity 40 puts the lit tiles well above
        // 1.0: the HDR case, through an RGBA16F SceneColor.
        const glm::vec3 kSunDirection = glm::normalize(glm::vec3(-0.55f, -0.766f, -0.33f));
        constexpr f32 kSunIntensity = 40.0f;

        void AddSun(Scene& scene, f32 intensity)
        {
            Entity sun = scene.CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = kSunDirection;
            dl.m_Color = glm::vec3(1.0f);
            dl.m_Intensity = intensity;
            dl.m_CastShadows = false;
        }

        void SetSun(Scene& scene, bool on)
        {
            if (auto* dl = FindComponent<DirectionalLightComponent>(scene, "Sun"))
                dl->m_Intensity = on ? kSunIntensity : 0.0f;
        }

        // GTAO runs only when it is the ACTIVE technique (GTAORenderPass checks
        // ActiveAOTechnique; the default is SSAO), so GTAOEnabled alone
        // switches nothing. The override flag is what makes the serializer
        // write the choice into an exported scene.
        void SetGtao(bool on)
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.ActiveAOTechnique = AOTechnique::GTAO;
            pp.m_AOTechniqueOverride = true;
            pp.GTAOEnabled = on;
            pp.SSAOEnabled = false;
        }

        EstimatorToggle GtaoToggle()
        {
            return { "GTAO", SetGtao, LightingEstimator::None, {} };
        }

        EstimatorToggle SsgiToggle()
        {
            return { "SSGI", [](bool on)
                     { Renderer3D::GetPostProcessSettings().SSGIEnabled = on; },
                     LightingEstimator::SSGI, [](LightingFrameConfiguration& f)
                     { f.SSGIRequested = true; },
                     "SSGIEnabled" };
        }

        EstimatorToggle ReSTIRDIToggle()
        {
            return { "ReSTIR DI", [](bool on)
                     { Renderer3D::GetPostProcessSettings().ReSTIRDI.Enabled = on; },
                     LightingEstimator::ReSTIRDI, [](LightingFrameConfiguration& f)
                     { f.ReSTIRDIActive = true; },
                     "ReSTIRDIEnabled" };
        }

        std::vector<SceneRow> BuildRows()
        {
            std::vector<SceneRow> rows;

            // -- 1. Direct light, both closures, analytic ------------------------
            {
                SceneRow row;
                row.Name = "DirectionalLight";
                row.Motivation = "#1336 direct term; the HDR case";
                row.Build = [](Scene& scene)
                {
                    for (const TileSpec& tile : MaterialTiles())
                        AddTile(scene, tile);
                    AddSun(scene, kSunIntensity);
                };
                TermProbe probe;
                probe.Name = "Direct";
                probe.Term = LightingTerm::DirectDiffuse;
                probe.SetSource = SetSun;
                const glm::dvec3 towardLight = -glm::dvec3(kSunDirection);
                for (const TileSpec& tile : MaterialTiles())
                    probe.Regions.push_back(
                        Interior(tile, DirectionalLightAnalytic(tile, towardLight, glm::dvec3(kSunIntensity))));
                probe.Analytic = { 0.03, 2.0e-3,
                                   "RGBA16F SceneColor (2^-11 relative) plus f32 shading against the f64 oracle; the "
                                   "ClosureV2 multi-scatter lobe reads a 16x16 bilinear energy table the oracle "
                                   "re-integrates (BsdfIdentityOracleTest bounds that table)" };
                probe.CrossArm = { 0.01, 1.0e-3, "every arm shades the same closure; 1 % covers fp16 rounding "
                                                 "of the G-Buffer's packed normal and roughness on Deferred" };
                probe.TemporalCrossArm = Tolerance{ 0.02, 1.0e-3, "FSR2's reconstruction of a converged static "
                                                                  "frame moves 0.8-1.3 % (measured)" };
                row.Probes.push_back(std::move(probe));
                row.Invariances.push_back({ "Direct", GtaoToggle(), false, { 0.005, 1.0e-3, "AO is visibility for the ambient term only (#1452): "
                                                                                            "the direct term may not move" } });
                rows.push_back(std::move(row));
            }

            // -- 2. Emission, analytic, and untouched by AO and SSGI ------------
            {
                SceneRow row;
                row.Name = "Emission";
                row.Motivation = "#1336: emission is added once, and no visibility term applies to it";
                static const std::vector<TileSpec> tiles = {
                    { "EmitDim", { -3.6, 0.0 }, 3.0, glm::vec3(0.0f), 0.0f, 0.8f, PBRModel::Legacy, glm::vec3(0.2f) },
                    { "EmitWarm", { 0.0, 0.0 }, 3.0, glm::vec3(0.0f), 0.0f, 0.8f, PBRModel::Legacy, { 4.0f, 1.0f, 0.25f } },
                    { "EmitHdr", { 3.6, 0.0 }, 3.0, glm::vec3(0.0f), 0.0f, 0.8f, PBRModel::ClosureV2, glm::vec3(20.0f) },
                };
                row.Build = [](Scene& scene)
                {
                    for (const TileSpec& tile : tiles)
                        AddTile(scene, tile);
                    // Occluders, so AO has something to find next to the emitters.
                    AddBox(scene, "Occluder", { 1.8f, 0.75f, 0.0f }, { 0.4f, 1.5f, 3.0f }, glm::vec3(0.0f));
                };
                TermProbe probe;
                probe.Name = "Emission";
                probe.Term = LightingTerm::Emission;
                probe.SetSource = [](Scene& scene, bool on)
                {
                    for (const TileSpec& tile : tiles)
                        if (Material* m = FindMaterial(scene, tile.Name))
                            m->SetEmissiveFactor(glm::vec4(on ? tile.Emissive : glm::vec3(0.0f), 1.0f));
                };
                for (const TileSpec& tile : tiles)
                    probe.Regions.push_back(Interior(tile, ConstantAnalytic(glm::dvec3(tile.Emissive))));
                probe.Analytic = { 2.0e-3, 1.0e-4, "emission is the material's radiance, written once: only "
                                                   "RGBA16F rounding (2^-11) separates it from the authored value" };
                probe.CrossArm = { 2.0e-3, 1.0e-4, "the same authored radiance on every arm" };
                row.Probes.push_back(std::move(probe));
                row.Invariances.push_back(
                    { "Emission", GtaoToggle(), false, { 2.0e-3, 1.0e-4, "AO multiplies the ambient term only; emission has no visibility (#1336)" } });
                row.Invariances.push_back(
                    { "Emission", SsgiToggle(), false, { 2.0e-3, 1.0e-4, "SSGI replaces the ladder's indirect diffuse; emission is not its term" } });
                rows.push_back(std::move(row));
            }

            // -- 3. The ambient ladder's flat rung (indirect diffuse) ----------
            {
                SceneRow row;
                row.Name = "AmbientFlatFill";
                row.Motivation = "#1336 ambient ladder, bottom rung: no lightmap, probe or IBL in the scene";
                static const std::vector<TileSpec> tiles = {
                    { "AmbientWhite", { -3.6, 0.0 }, 3.0, glm::vec3(0.9f), 0.0f, 1.0f, PBRModel::Legacy },
                    { "AmbientRed", { 0.0, 0.0 }, 3.0, { 0.9f, 0.1f, 0.05f }, 0.0f, 0.7f, PBRModel::Legacy },
                    { "AmbientV2", { 3.6, 0.0 }, 3.0, glm::vec3(0.5f), 0.0f, 0.5f, PBRModel::ClosureV2 },
                };
                row.Build = [](Scene& scene)
                {
                    for (const TileSpec& tile : tiles)
                    {
                        TileSpec black = tile;
                        black.Albedo = glm::vec3(0.0f);
                        AddTile(scene, black);
                    }
                };
                // Switching the ALBEDO (dielectric, so F0 and the specular
                // ambient do not move) separates the albedo-proportional
                // ambient diffuse from everything else.
                TermProbe probe;
                probe.Name = "IndirectDiffuse";
                probe.Term = LightingTerm::IndirectDiffuse;
                probe.SetSource = [](Scene& scene, bool on)
                {
                    for (const TileSpec& tile : tiles)
                        if (Material* m = FindMaterial(scene, tile.Name))
                            m->SetBaseColorFactor(glm::vec4(on ? tile.Albedo : glm::vec3(0.0f), 1.0f));
                };
                for (const TileSpec& tile : tiles)
                    probe.Regions.push_back(Interior(tile));
                probe.CrossArm = { 0.01, 2.0e-4, "the flat rung is one constant times albedo on every path" };
                probe.MinimumSignal = 5.0e-3;
                row.Probes.push_back(std::move(probe));
                rows.push_back(std::move(row));
            }

            // -- 4. One point light: Forward's loop vs the tiles (#1457) --------
            {
                SceneRow row;
                row.Name = "PointLight";
                row.Motivation = "#1457: Forward walks the light array, Forward+ and Deferred the tiles";
                row.Build = [](Scene& scene)
                {
                    for (const TileSpec& tile : MaterialTiles())
                        AddTile(scene, tile);
                    Entity e = scene.CreateEntity("Point");
                    e.GetComponent<TransformComponent>().Translation = { -0.8f, 1.6f, 0.4f };
                    auto& pl = e.AddComponent<PointLightComponent>();
                    pl.m_Color = { 1.0f, 0.25f, 0.2f };
                    pl.m_Intensity = 12.0f;
                    pl.m_Range = 9.0f;
                    pl.m_Attenuation = 1.0f;
                    pl.m_CastShadows = false;
                };
                TermProbe probe;
                probe.Name = "Direct";
                probe.Term = LightingTerm::DirectDiffuse;
                probe.SetSource = [](Scene& scene, bool on)
                {
                    if (auto* pl = FindComponent<PointLightComponent>(scene, "Point"))
                        pl->m_Intensity = on ? 12.0f : 0.0f;
                };
                for (const TileSpec& tile : MaterialTiles())
                    probe.Regions.push_back(Interior(tile));
                probe.CrossArm = { 0.01, 1.0e-3, "one light evaluated by the same closure; the tile lists must "
                                                 "contain it wherever it reaches (#1457)" };
                // The far black-gloss tile reflects the light at ~7e-4.
                probe.MinimumSignal = 1.0e-4;
                row.Probes.push_back(std::move(probe));
                rows.push_back(std::move(row));
            }

            // -- 5. Snow on every path (#1451) ------------------------------------
            {
                SceneRow row;
                row.Name = "SnowLayer";
                row.Motivation = "#1451: the same snow on all three paths";
                row.Build = [](Scene& scene)
                {
                    for (const TileSpec& tile : MaterialTiles())
                        AddTile(scene, tile);
                    AddSun(scene, 4.0f);
                };
                TermProbe probe;
                probe.Name = "Snow";
                probe.Term = LightingTerm::DirectDiffuse;
                probe.SetSource = [](Scene&, bool on)
                {
                    SnowSettings& snow = Renderer3D::GetSnowSettings();
                    snow = SnowSettings{};
                    snow.Enabled = on;
                    // The ground is at y = 0: full coverage from y = -1 up.
                    snow.HeightStart = -2.0f;
                    snow.HeightFull = -1.0f;
                    snow.SSSBlurEnabled = false;
                };
                for (const TileSpec& tile : MaterialTiles())
                    probe.Regions.push_back(Interior(tile));
                probe.CrossArm = { 0.02, 2.0e-3, "snow replaces the surface closure identically on every path; "
                                                 "2 % covers Deferred's G-Buffer quantisation of the snow albedo" };
                probe.TemporalCrossArm = Tolerance{
                    0.06, 2.0e-3,
                    "the snow term is the difference of two frames each ~6x larger, and FSR2's reconstruction "
                    "moves ~1 % of the full signal (0.8-1.3 % measured on the direct row): ~5 % of this "
                    "difference, measured 4.4-4.7 % after the history reset"
                };
                row.Probes.push_back(std::move(probe));
                rows.push_back(std::move(row));
            }

            // -- 6. Screen-space AO on the ambient term (#1452) -----------------
            {
                SceneRow row;
                row.Name = "ScreenSpaceAO";
                row.Motivation = "#1452: AO multiplies the ambient term on every path";
                static const std::vector<TileSpec> tiles = {
                    { "AOFloorLeft", { -2.5, 0.0 }, 5.0, glm::vec3(0.9f), 0.0f, 1.0f, PBRModel::Legacy },
                    { "AOFloorRight", { 2.5, 0.0 }, 5.0, glm::vec3(0.9f), 0.0f, 1.0f, PBRModel::Legacy },
                };
                row.Build = [](Scene& scene)
                {
                    for (const TileSpec& tile : tiles)
                        AddTile(scene, tile);
                    AddBox(scene, "Wall", { 0.0f, 1.0f, 0.0f }, { 0.5f, 2.0f, 6.0f }, glm::vec3(0.9f));
                };
                TermProbe probe;
                probe.Name = "AOOnAmbient";
                probe.Term = LightingTerm::IndirectDiffuse;
                probe.SetSource = [](Scene&, bool on)
                { SetGtao(on); };
                // Strips beside the wall, where the AO darkens the ambient.
                probe.Regions.push_back({ "LeftOfWall", { -0.7, -1.5 }, { -0.3, 1.5 }, {} });
                probe.Regions.push_back({ "RightOfWall", { 0.3, -1.5 }, { 0.7, 1.5 }, {} });
                probe.CrossArm = { 0.15, 5.0e-4, "the same GTAO buffer on every path since #1452; 15 % covers "
                                                 "the forward paths building it from the prepass normals and "
                                                 "Deferred from the G-Buffer's packed ones" };
                // AO darkens a flat-fill ambient of ~0.027: the term is small.
                probe.MinimumSignal = 1.0e-4;
                row.Probes.push_back(std::move(probe));
                rows.push_back(std::move(row));
            }

            // -- 7. SSGI: Deferred's indirect-diffuse estimator ------------------
            {
                SceneRow row;
                row.Name = "SSGIBounce";
                row.Motivation = "#1336: SSGI answers for indirect diffuse over the directions it resolves";
                row.Requires = SsgiToggle();
                static const std::vector<TileSpec> tiles = {
                    { "SSGIFloor", { 0.0, 0.0 }, 8.0, glm::vec3(0.8f), 0.0f, 1.0f, PBRModel::Legacy },
                };
                row.Build = [](Scene& scene)
                {
                    for (const TileSpec& tile : tiles)
                        AddTile(scene, tile);
                    AddBox(scene, "HotWall", { 0.0f, 1.0f, -1.0f }, { 4.0f, 2.0f, 0.3f }, glm::vec3(0.0f),
                           { 3.0f, 0.6f, 0.2f });
                };
                TermProbe probe;
                probe.Name = "SSGI";
                probe.Term = LightingTerm::IndirectDiffuse;
                probe.SetSource = [](Scene&, bool on)
                { Renderer3D::GetPostProcessSettings().SSGIEnabled = on; };
                probe.Regions.push_back({ "FloorBesideWall", { -1.5, -0.6 }, { 1.5, 0.4 }, {} });
                probe.CrossArm = { 0.10, 1.0e-3, "a screen-space estimate: MSAA and the upscalers change its "
                                                 "input resolution" };
                row.Probes.push_back(std::move(probe));
                rows.push_back(std::move(row));
            }

            // -- 8. ReSTIR DI: the hybrid tier (Vulkan + ray queries only) ------
            {
                SceneRow row;
                row.Name = "ReSTIRDI";
                row.Motivation = "#1140: ReSTIR DI owns the punctual lights when live";
                row.Requires = ReSTIRDIToggle();
                // The point light is what ReSTIR DI owns (punctual lights); the
                // separated term must match what the raster loop gives.
                // 64 point lights: ReSTIR DI stands down ("fallback") unless the
                // scene has more emitters than its per-pixel candidate budget
                // (32) plus a margin, since resampling would then only
                // re-enumerate what the clustered loop already walks.
                row.Build = [](Scene& scene)
                {
                    for (const TileSpec& tile : MaterialTiles())
                        AddTile(scene, tile);
                    for (int i = 0; i < 64; ++i)
                    {
                        const f32 x = -5.6f + 1.6f * static_cast<f32>(i % 8);
                        const f32 z = -3.5f + 1.0f * static_cast<f32>(i / 8);
                        Entity e = scene.CreateEntity("Point" + std::to_string(i));
                        e.GetComponent<TransformComponent>().Translation = { x, 1.2f, z };
                        auto& pl = e.AddComponent<PointLightComponent>();
                        pl.m_Color = { 1.0f, 0.9f, 0.8f };
                        pl.m_Intensity = 0.6f;
                        pl.m_Range = 4.0f;
                        pl.m_CastShadows = false;
                    }
                };
                TermProbe probe;
                probe.Name = "Direct";
                probe.Term = LightingTerm::DirectDiffuse;
                probe.SetSource = [](Scene& scene, bool on)
                {
                    auto view = scene.GetAllEntitiesWith<PointLightComponent>();
                    for (auto entity : view)
                        view.template get<PointLightComponent>(entity).m_Intensity = on ? 0.6f : 0.0f;
                };
                for (const TileSpec& tile : MaterialTiles())
                    probe.Regions.push_back(Interior(tile));
                probe.CrossArm = { 0.05, 2.0e-3, "a resampled estimate of the same light: 5 % covers the "
                                                 "reservoir's per-pixel noise averaged over a tile" };
                row.Probes.push_back(std::move(probe));
                rows.push_back(std::move(row));
            }

            // -- 9. A near-mirror highlight of an HDR sun ------------------------
            {
                SceneRow row;
                row.Name = "MirrorSunHighlight";
                row.Motivation = "#1347: the unclamped v2 NDF peak times an HDR sun exceeds the RGBA16F range";
                // The ground point that reflects the sun into the eye: the eye
                // looks along -v where v = reflect(toward-light) about +y.
                static const TileSpec mirror = []
                {
                    const glm::dvec3 l = -glm::dvec3(kSunDirection);
                    const glm::dvec3 v(-l.x, l.y, -l.z);
                    const f64 t = kEyeHeight / v.y;
                    TileSpec tile{ "MirrorMetal", { -v.x * t, -v.z * t }, 1.4, { 0.95f, 0.93f, 0.88f }, 1.0f, 0.0f, PBRModel::ClosureV2 };
                    return tile;
                }();
                row.Build = [](Scene& scene)
                {
                    AddTile(scene, mirror);
                    AddSun(scene, kSunIntensity);
                };
                TermProbe probe;
                probe.Name = "Direct";
                probe.Term = LightingTerm::DirectSpecular;
                probe.SetSource = SetSun;
                probe.Regions.push_back(Interior(mirror));
                probe.CrossArm = { 0.05, 1.0e-2, "a sub-pixel lobe: every arm point-samples the same peak, but "
                                                 "MSAA and upscaling resample it" };
                probe.TemporalCrossArm = Tolerance{
                    0.6, 1.0e-2,
                    "FSR2 accumulates colour through a reversible tone map, so a sub-pixel HDR peak averages low "
                    "(measured 41-47 % under the reference); the finite-value check still holds on this arm"
                };
                row.Probes.push_back(std::move(probe));
                rows.push_back(std::move(row));
            }

            return rows;
        }

        const std::vector<SceneRow>& Rows()
        {
            static const std::vector<SceneRow> rows = BuildRows();
            return rows;
        }

        const std::vector<Arm>& Arms()
        {
            static const std::vector<Arm> arms = EnumerateArms();
            return arms;
        }

        [[nodiscard]] const Arm& ArmById(std::string_view id)
        {
            for (const Arm& arm : Arms())
                if (arm.Id == id)
                    return arm;
            return Arms().front();
        }
    } // namespace

    using MatrixParam = std::tuple<sizet, sizet>; // (row index, arm index)

    class CrossPathLightingMatrix : public RendererAttachedTest, public ::testing::WithParamInterface<MatrixParam>
    {
      protected:
        [[nodiscard]] const SceneRow& Row() const
        {
            return Rows()[std::get<0>(GetParam())];
        }
        [[nodiscard]] const Arm& ThisArm() const
        {
            return Arms()[std::get<1>(GetParam())];
        }

        void BuildScene() override
        {
            m_SavedSnow = Renderer3D::GetSnowSettings();
            Renderer3D::GetSnowSettings() = SnowSettings{};
            EnableRendering(kWidth, kHeight);

            Scene& scene = GetScene();
            Entity camera = scene.CreateEntity("Camera");
            auto& tc = camera.GetComponent<TransformComponent>();
            tc.Translation = glm::vec3(kEye);
            tc.SetRotationEuler({ -0.5f * std::numbers::pi_v<f32>, 0.0f, 0.0f });
            auto& cc = camera.AddComponent<CameraComponent>();
            cc.Primary = true;
            cc.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
            cc.Camera.SetPerspectiveVerticalFOV(static_cast<f32>(kFovY));
            cc.Camera.SetPerspectiveNearClip(0.05f);
            cc.Camera.SetPerspectiveFarClip(100.0f);
            cc.Camera.SetViewportSize(kWidth, kHeight);

            Row().Build(scene);
        }

        void TearDown() override
        {
            Renderer3D::GetSnowSettings() = m_SavedSnow;
            RendererAttachedTest::TearDown();
        }

        // Configure the renderer for an arm: the path, and the Deferred
        // sampling / reconstruction the arm's support row declares. Everything
        // that would add a term of its own is off.
        void ApplyArm(const Arm& arm)
        {
            auto& rs = Renderer3D::GetRendererSettings();
            rs.Path = arm.Request.Path;
            rs.ForwardPlusAutoSwitch = false; // Forward stays Forward
            rs.ShowGrid = false;
            rs.Deferred.MSAASampleCount = arm.Request.Samples;

            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.BloomEnabled = false;
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
            pp.ActiveAOTechnique = AOTechnique::GTAO;
            pp.m_AOTechniqueOverride = true;
            pp.SSAOEnabled = false;
            pp.GTAOEnabled = false;
            pp.SSGIEnabled = false;
            pp.Upscale = arm.Request.Upscale == RendererSupport::Reconstruction::Native ? UpscaleMode::Off
                                                                                        : UpscaleMode::Quality;
            pp.Technique = arm.Request.Upscale == RendererSupport::Reconstruction::Temporal ? UpscalerTechnique::Temporal
                                                                                            : UpscalerTechnique::Spatial;
            // FSR2's RCAS runs on HDR before the tone map, i.e. on what this
            // fixture reads; sharpening is not a lighting term.
            pp.FSR2SharpeningEnabled = false;
            Renderer3D::ApplyRendererSettings();
        }

        [[nodiscard]] u32 FramesFor(const Arm& arm) const
        {
            return arm.Request.Upscale == RendererSupport::Reconstruction::Temporal ? kFramesPerTemporalCapture
                                                                                    : kFramesPerCapture;
        }

        // Render and read the linear HDR colour at the END OF THE SCENE BAND:
        // the latest of the HDR composites that run before the tone map, in
        // chain order. Not SceneColor alone: SSGI composites into its own
        // SSGIColor downstream of it (SSR into SSRColor, contact shadows into
        // ContactShadowColor). Not PostProcessColor: that alias is repointed
        // down the whole chain, onto the post-tone-map sharpen output on the
        // spatial arm. On an upscaled arm it is the display-resolution EASU /
        // FSR2 output, both HDR and before the tone map, so a temporal arm is
        // read after FSR2 has resolved its jitter instead of from one jittered
        // frame. Every other post effect is off (ApplyArm).
        //
        // READ INSIDE THE FRAME, not after it. These are transient render-graph
        // targets whose memory is reused by later passes: read after the frame,
        // EASUColor on the spatial arm held the tone-mapped image (a 7.6 term
        // read as 0.76, an authored emission of 4 read as Reinhard + gamma
        // 0.903). A post-pass hook reads each candidate right after every pass
        // that writes it, so the last write before reuse is what is measured.
        [[nodiscard]] LinearImage Capture(const Arm& arm)
        {
            static constexpr std::string_view kSceneBandEnds[] = {
                ResourceNames::FSR2Color,
                ResourceNames::EASUColor,
                ResourceNames::SSRColor,
                ResourceNames::SSGIColor,
                ResourceNames::ContactShadowColor,
                ResourceNames::AOApplyColor,
                ResourceNames::SceneColor,
            };
            struct Session
            {
                bool Pinned = false;
                std::map<std::string, std::vector<std::string_view>> CandidatesByWriter;
                std::map<std::string_view, LinearImage> Read;
            };

            // FSR2 carries its history across the OFF -> ON switch of a
            // term's source (a lighting change on a static pose is nothing its
            // motion vectors describe), and a small change converges slowly:
            // measured 14 % of the snow term still missing after 24 frames and
            // 7 % after 96. The fixture measures each state converged, so it
            // drops the history the way production does on a resolution change
            // (FSR2RenderPass::SetupFramebuffer), then re-accumulates.
            if (arm.Request.Upscale == RendererSupport::Reconstruction::Temporal)
            {
                ResizeRenderTarget(kWidth + 16u, kHeight);
                RunFrames(1);
                ResizeRenderTarget(kWidth, kHeight);
            }
            const u32 frames = FramesFor(arm);
            if (frames > 1u)
                RunFrames(frames - 1u);

            RenderGraph* graph = const_cast<RenderGraph*>(RenderGraphDebugRuntime::GetActiveGraph().Raw());
            if (!graph)
            {
                ADD_FAILURE() << "no active render graph on " << arm.Id;
                return {};
            }
            auto session = std::make_shared<Session>();
            constexpr std::string_view kHookKey = "CrossPathLightingMatrix";
            graph->AddPostPassHook(
                kHookKey,
                [session](std::string_view pass, RenderGraph& g)
                {
                    if (!session->Pinned)
                    {
                        session->Pinned = true;
                        for (const auto& lifetime : g.GetResourceLifetimes())
                        {
                            const std::string_view resource = lifetime.ResourceName.ToView();
                            for (const std::string_view candidate : kSceneBandEnds)
                            {
                                // `candidate` itself and its WriteNewVersion renames ("name@Pass").
                                const bool family = resource == candidate ||
                                                    (resource.size() > candidate.size() && resource.starts_with(candidate) &&
                                                     resource[candidate.size()] == '@');
                                if (family && lifetime.FirstWritePassIndex != std::numeric_limits<u32>::max())
                                    session->CandidatesByWriter[lifetime.FirstWritePass.ToStdString()].push_back(candidate);
                            }
                        }
                    }
                    const auto it = session->CandidatesByWriter.find(std::string(pass));
                    if (it == session->CandidatesByWriter.end())
                        return;
                    for (const std::string_view candidate : it->second)
                    {
                        const u32 texture =
                            g.ResolveTexture(g.GetTextureHandle(std::string(candidate) + "Texture"));
                        if (texture == 0u)
                            continue;
                        GLint width = 0;
                        GLint height = 0;
                        GLint samples = 0;
                        ::glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &width);
                        ::glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &height);
                        ::glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_SAMPLES, &samples);
                        if (width <= 0 || height <= 0 || samples > 0)
                            continue;
                        LinearImage& image = session->Read[candidate];
                        image.Target = candidate;
                        image.ReadAfterPass = std::string(pass);
                        image.Width = static_cast<u32>(width);
                        image.Height = static_cast<u32>(height);
                        std::vector<f32> bottomUp;
                        ReadbackRgbaFloat(texture, image.Width, image.Height, bottomUp);
                        image.Rgba.resize(bottomUp.size());
                        const sizet row = static_cast<sizet>(image.Width) * 4u;
                        for (u32 y = 0; y < image.Height; ++y)
                            std::copy_n(bottomUp.data() + static_cast<sizet>(image.Height - 1u - y) * row, row,
                                        image.Rgba.data() + static_cast<sizet>(y) * row);
                    }
                });
            RunFrames(1);
            graph->RemovePostPassHook(kHookKey);

            for (const std::string_view candidate : kSceneBandEnds)
            {
                if (const auto it = session->Read.find(candidate); it != session->Read.end())
                    return it->second;
            }
            ADD_FAILURE() << "no scene-band colour target was written on " << arm.Id;
            return {};
        }

        struct SeparatedTerm
        {
            LinearImage On;
            LinearImage Off;
        };

        [[nodiscard]] SeparatedTerm Separate(const Arm& arm, const TermProbe& probe)
        {
            SeparatedTerm term;
            probe.SetSource(GetScene(), false);
            term.Off = Capture(arm);
            probe.SetSource(GetScene(), true);
            term.On = Capture(arm);
            return term;
        }

        [[nodiscard]] std::optional<std::string> TemporalSkip(const Arm& arm) const
        {
            if (arm.Request.Upscale == RendererSupport::Reconstruction::Temporal &&
                !Renderer3D::IsTemporalUpscaleActive())
                return std::string(arm.Id) + ": FSR2 did not engage on this device (Renderer3D::"
                                             "IsTemporalUpscaleActive() is false after the capture), so this "
                                             "arm would silently be the spatial one";
            return std::nullopt;
        }

        SnowSettings m_SavedSnow{};
    };

    TEST_P(CrossPathLightingMatrix, SeparatedTermsAgreeAcrossArmsAndWithTheModel)
    {
        const SceneRow& row = Row();
        const Arm& arm = ThisArm();

        const ArmPlan plan = PlanArm(arm);
        if (!plan.Runs)
            GTEST_SKIP() << plan.SkipReason;
        if (row.Requires.Estimator != LightingEstimator::None && NeedsRayQueries(row.Requires.Estimator))
        {
            if (!Tests::Options().CrossPathExportDir.empty() && arm.Id == "gl-deferred-native" &&
                RenderPropertyFixture::IsGpuAvailable())
            {
                ScopedMockTime exportTime(kCaptureTime);
                for (const TermProbe& probe : row.Probes)
                {
                    ApplyArm(arm);
                    const SeparatedTerm term = Separate(arm, probe);
                    if (!term.On.Rgba.empty() && !term.Off.Rgba.empty())
                        ExportProbe(GetSceneRef(), row, probe, term.On, term.Off);
                }
            }
            GTEST_SKIP() << row.Name << " needs " << row.Requires.Name
                         << ", which runs on a hardware ray-tracing Vulkan device only; " << arm.Id
                         << " is GL. Live cell: scripts/cross-path-matrix-live.py";
        }
        if (!row.Probes.empty())
        {
            if (const auto why = OwnershipSkip(arm, row.Requires, row.Probes.front().Term))
                GTEST_SKIP() << row.Name << " on " << arm.Id << ": " << *why;
        }
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);

        // The comparison arm: GL Forward native, unless the row needs an
        // estimator Forward does not host — then the first runnable arm that
        // hosts it.
        const Arm* reference = &ArmById(kReferenceArmId);
        if (row.Requires.Estimator != LightingEstimator::None)
        {
            for (const Arm& candidate : Arms())
            {
                if (PlanArm(candidate).Runs && !OwnershipSkip(candidate, row.Requires, row.Probes.front().Term))
                {
                    reference = &candidate;
                    break;
                }
            }
        }
        const bool isReference = reference->Id == arm.Id;

        for (const TermProbe& probe : row.Probes)
        {
            SCOPED_TRACE(std::string(row.Name) + "/" + std::string(probe.Name) + " on " + std::string(arm.Id));

            ApplyArm(arm);
            if (row.Requires.Set)
                row.Requires.Set(true);
            const SeparatedTerm term = Separate(arm, probe);
            ASSERT_FALSE(HasFatalFailure());
            if (term.On.Rgba.empty() || term.Off.Rgba.empty())
                return; // Capture reported why
            if (const auto why = TemporalSkip(arm))
                GTEST_SKIP() << *why;
            WriteTermPng("CrossPathMatrix_" + std::string(row.Name) + "_" + std::string(probe.Name) + "_" +
                             CellName(arm),
                         term.On, term.Off);

            SeparatedTerm referenceTerm;
            if (!isReference)
            {
                ApplyArm(*reference);
                if (row.Requires.Set)
                    row.Requires.Set(true);
                referenceTerm = Separate(*reference, probe);
                ASSERT_FALSE(HasFatalFailure());
            }

            for (const GroundRegion& region : probe.Regions)
            {
                const RegionStats stats = Measure(term.On, term.Off, region);
                std::cout << "[matrix] " << row.Name << "/" << probe.Name << " " << arm.Id << " " << region.Name
                          << ": term " << Str(stats.Mean) << " over " << stats.Pixels << " px of "
                          << term.On.Target;
                if (stats.AnalyticPixels > 0)
                    std::cout << ", model " << Str(stats.AnalyticMean);
                std::cout << "\n";

                ASSERT_GT(stats.Pixels, 50u) << region.Name << ": too few pixels in the region — the camera "
                                             << "mapping or the render resolution is off";
                EXPECT_FALSE(stats.NonFinite) << region.Name << ": a non-finite texel in the separated term";

                if (stats.AnalyticPixels > 0)
                {
                    f64 worst = 0.0;
                    EXPECT_TRUE(Within(stats.Mean, stats.AnalyticMean, probe.Analytic, worst))
                        << region.Name << ": separated " << ToString(probe.Term) << " " << Str(stats.Mean)
                        << " vs the independent model " << Str(stats.AnalyticMean) << " (worst channel "
                        << worst * 100.0 << " %; bound " << probe.Analytic.Relative * 100.0 << " % + "
                        << probe.Analytic.Absolute << ": " << probe.Analytic.Reason << ")";
                }

                if (isReference)
                {
                    if (&region == &probe.Regions.back() && !Tests::Options().CrossPathExportDir.empty())
                        ExportProbe(GetSceneRef(), row, probe, term.On, term.Off);
                    // Magnitude: a term may legitimately be negative (snow over a
                    // brighter surface, AO darkening the ambient).
                    const f64 peak =
                        std::max({ std::abs(stats.Mean.x), std::abs(stats.Mean.y), std::abs(stats.Mean.z) });
                    EXPECT_GT(peak, probe.MinimumSignal)
                        << region.Name << ": the reference arm's separated term is " << Str(stats.Mean)
                        << " — a row that draws nothing cannot show two arms agree";
                }
                else
                {
                    const RegionStats ref = Measure(referenceTerm.On, referenceTerm.Off, region);
                    const Tolerance& crossArm =
                        (probe.TemporalCrossArm && arm.Request.Upscale == RendererSupport::Reconstruction::Temporal)
                            ? *probe.TemporalCrossArm
                            : probe.CrossArm;
                    f64 worst = 0.0;
                    EXPECT_TRUE(Within(stats.Mean, ref.Mean, crossArm, worst))
                        << region.Name << ": " << arm.Id << " separates " << ToString(probe.Term) << " as "
                        << Str(stats.Mean) << ", the reference arm " << reference->Id << " as " << Str(ref.Mean)
                        << " (worst channel " << worst * 100.0 << " %; bound " << crossArm.Relative * 100.0
                        << " %: " << crossArm.Reason << ")";
                }
            }
        }

        // Invariance and positive controls: hold one term, switch an estimator.
        for (const InvarianceProbe& inv : row.Invariances)
        {
            const TermProbe* held = nullptr;
            for (const TermProbe& p : row.Probes)
                if (p.Name == inv.TermProbeName)
                    held = &p;
            ASSERT_NE(held, nullptr) << "invariance probe names no term probe of the row";
            if (const auto why = OwnershipSkip(arm, inv.Toggle, LightingTerm::IndirectDiffuse);
                why && inv.Toggle.Estimator != LightingEstimator::None)
            {
                std::cout << "[matrix] " << row.Name << " " << arm.Id << ": invariance under " << inv.Toggle.Name
                          << " not run — " << *why << "\n";
                continue;
            }
            SCOPED_TRACE(std::string(row.Name) + "/" + std::string(held->Name) + " under " +
                         std::string(inv.Toggle.Name) + " on " + std::string(arm.Id));

            ApplyArm(arm);
            inv.Toggle.Set(false);
            const SeparatedTerm without = Separate(arm, *held);
            ApplyArm(arm);
            inv.Toggle.Set(true);
            const SeparatedTerm with = Separate(arm, *held);
            inv.Toggle.Set(false);
            ASSERT_FALSE(HasFatalFailure());
            if (with.On.Rgba.empty() || without.On.Rgba.empty())
                return;

            for (const GroundRegion& region : held->Regions)
            {
                const RegionStats a = Measure(without.On, without.Off, region);
                const RegionStats b = Measure(with.On, with.Off, region);
                f64 worst = 0.0;
                const bool same = Within(b.Mean, a.Mean, inv.Allowed, worst);
                std::cout << "[matrix] " << row.Name << "/" << held->Name << " under " << inv.Toggle.Name << " "
                          << arm.Id << " " << region.Name << ": " << Str(a.Mean) << " -> " << Str(b.Mean) << "\n";
                if (inv.ExpectChange)
                    EXPECT_FALSE(same) << region.Name << ": " << inv.Toggle.Name << " did not change "
                                       << ToString(held->Term) << " — the positive control did not engage";
                else
                    EXPECT_TRUE(same) << region.Name << ": " << inv.Toggle.Name << " moved "
                                      << ToString(held->Term) << " from " << Str(a.Mean) << " to " << Str(b.Mean)
                                      << " (worst channel " << worst * 100.0 << " %): " << inv.Allowed.Reason;
            }
        }
    }

    namespace
    {
        [[nodiscard]] std::string ParamName(const ::testing::TestParamInfo<MatrixParam>& info)
        {
            std::string name = std::string(Rows()[std::get<0>(info.param)].Name) + "_" +
                               std::string(Arms()[std::get<1>(info.param)].Id);
            for (char& c : name)
                if (!std::isalnum(static_cast<unsigned char>(c)))
                    c = '_';
            return name;
        }
    } // namespace

    INSTANTIATE_TEST_SUITE_P(AllRowsAllArms, CrossPathLightingMatrix,
                             ::testing::Combine(::testing::Range<sizet>(0, Rows().size()),
                                                ::testing::Range<sizet>(0, Arms().size())),
                             ParamName);

    // The declared matrix and the enumerated arms are the same list — the
    // fixture never runs a hand-written subset.
    TEST(CrossPathMatrixArms, EveryDeclaredPathRowIsAnArm)
    {
        EXPECT_EQ(Arms().size(), RendererSupport::PathCoverageRows.size());
        u32 glNative = 0;
        for (const Arm& arm : Arms())
        {
            if (arm.Request.Api == RendererSupport::Backend::OpenGL &&
                arm.Request.Upscale == RendererSupport::Reconstruction::Native && arm.Request.Samples == 1u &&
                arm.Request.LightingTechnique == RendererSupport::Technique::Raster)
            {
                ++glNative;
                EXPECT_TRUE(PlanArm(arm).Runs) << arm.Id << " must run in-process";
            }
            if (arm.Request.Api == RendererSupport::Backend::Vulkan)
            {
                const ArmPlan plan = PlanArm(arm);
                EXPECT_FALSE(plan.Runs);
                EXPECT_FALSE(plan.SkipReason.empty());
            }
        }
        EXPECT_EQ(glNative, 3u) << "GL x {Forward, Forward+, Deferred} native must all be arms";
    }
} // namespace OloEngine::Tests::CrossPath
