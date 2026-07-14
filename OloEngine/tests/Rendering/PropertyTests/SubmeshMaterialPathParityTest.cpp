// OLO_TEST_LAYER: integration
//
// THREE PATHS, ONE PICTURE (issue #629).
//
// The same multi-material mesh can reach the screen three ways — ModelComponent,
// MeshComponent, VirtualMeshComponent — and all three are supposed to render the same
// frame. None of them agreed:
//
//   * MeshComponent  resolved MaterialComponent -> engine default and NEVER consulted the
//                    submesh's imported material, so a multi-material glTF shaded every
//                    submesh with one flat grey material.
//   * ModelComponent resolved imported -> default and ignored an authored MaterialComponent.
//   * VirtualMeshComponent was the only path doing the full
//                    override -> imported -> default precedence.
//   * On top of that, Model::CreateCombinedMeshSource wrote Submesh::m_MaterialIndex = the
//                    submesh's own index, which only addresses the material array the WARM
//                    (.omesh cache) load rebuilds — never the DEDUPLICATED array the COLD
//                    import builds. So a first import and a second import of the same file
//                    rendered differently.
//
// Every one of those is invisible to a unit test of any single path, and all of them are
// caught by rendering the SAME MeshSource down all three paths and diffing the frames.
// The asset is the deccer-cubes "Colored" variant: a cube cluster whose materials are pure
// per-material baseColorFactors (red / green / blue / …) and no textures — so "did the
// per-submesh material survive?" is a question about the frame's COLOURS, which is exactly
// what the bug destroyed (everything went engine-default grey).
//
// SKIPs cleanly with no GL 4.6 context (suite gate).

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;

        // Parked far above the frame — the fixture builds all three renderables once and
        // moves the one under test to the origin, so every path is rendered by the exact
        // same scene, camera and lighting.
        constexpr glm::vec3 kParked{ 0.0f, 1000.0f, 0.0f };
        constexpr glm::vec3 kStage{ 0.0f, 0.0f, 0.0f };

        std::filesystem::path ColoredCubesPath()
        {
            return std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "assets/models/DeccerCubes/SM_Deccer_Cubes_Colored.glb";
        }

        // Per-hue pixel census. The cubes carry saturated per-material base colours; the
        // ground and the engine-default material are neutral grey. A frame that lost its
        // per-submesh materials has near-zero saturated pixels — which is the whole bug.
        struct ColourCensus
        {
            u32 Saturated = 0; // any clearly-non-grey pixel
            u32 RedDominant = 0;
            u32 GreenDominant = 0;
            u32 BlueDominant = 0;
        };

        ColourCensus Census(const std::vector<u8>& rgba)
        {
            ColourCensus census;
            for (sizet i = 0; i + 3 < rgba.size(); i += 4)
            {
                const i32 r = rgba[i];
                const i32 g = rgba[i + 1];
                const i32 b = rgba[i + 2];
                const i32 maxC = std::max({ r, g, b });
                const i32 minC = std::min({ r, g, b });
                if (maxC < 25 || (maxC - minC) < 40)
                {
                    continue; // background, ground, or an engine-default grey surface
                }

                ++census.Saturated;
                if (r == maxC)
                {
                    ++census.RedDominant;
                }
                else if (g == maxC)
                {
                    ++census.GreenDominant;
                }
                else
                {
                    ++census.BlueDominant;
                }
            }
            return census;
        }

        void WriteEvidencePng(const std::string& name, const std::vector<u8>& rgba, u32 width, u32 height)
        {
            std::vector<u8> flipped(rgba.size()); // GL readback is bottom-up
            const sizet rowBytes = static_cast<sizet>(width) * 4;
            for (u32 y = 0; y < height; ++y)
            {
                std::memcpy(flipped.data() + (static_cast<sizet>(y) * rowBytes),
                            rgba.data() + (static_cast<sizet>(height - 1 - y) * rowBytes), rowBytes);
            }

            std::filesystem::create_directories("assets/tests/visual");
            const std::string path = "assets/tests/visual/" + name;
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                               flipped.data(), static_cast<int>(rowBytes));
            EXPECT_NE(wrote, 0) << "failed to write evidence PNG " << path;
        }

        // Relative difference between two counts, guarded against a zero denominator.
        f32 RelativeDifference(u32 a, u32 b)
        {
            const auto larger = static_cast<f32>(std::max(a, b));
            if (larger <= 0.0f)
            {
                return 0.0f;
            }
            return std::abs(static_cast<f32>(a) - static_cast<f32>(b)) / larger;
        }

        // ── THE FIXTURE THE OLD ONE SHOULD HAVE BEEN ─────────────────────────────────────
        //
        // SM_Deccer_Cubes_Colored.glb is untextured, opaque, single-sided, carries no alpha
        // mode and no normal map. EVERY material property that actually broke Sponza is
        // ABSENT from it — the sRGB/linear albedo identity (needs a texture), the cutout that
        // makes a submesh VANISH when its albedo fails to load (needs alphaMode MASK), the
        // backface cull that eats half a leaf card (needs doubleSided), the drifted TBN (needs
        // a normal map), and the shadow gate that projected solid quads (needs an alpha-masked
        // material). A parity test on that asset can only see baseColorFactor.
        //
        // So author a glTF that has all four, one per submesh. It is generated rather than
        // committed because the properties, not the pixels, are the fixture — and because a
        // generated file cannot drift from what this test believes it contains.
        //
        //   submesh 0  TEXTURED       checkerboard albedo (sRGB)
        //   submesh 1  ALPHA-MASKED   albedo whose left half has alpha = 0, MASK, cutoff 0.5
        //   submesh 2  TWO-SIDED      doubleSided, and wound BACK-TO-FRONT so it is only
        //                             visible at all if the backface cull honours the flag
        //   submesh 3  NORMAL-MAPPED  a strongly off-neutral tangent-space normal
        struct AuthoredFixture
        {
            std::filesystem::path GltfPath;
            bool Ok = false;
        };

        // Four upright quads in a row, x = -3, -1, +1, +3.
        constexpr f32 kQuadHalfWidth = 0.8f;
        constexpr f32 kQuadBottom = 0.6f;
        constexpr f32 kQuadTop = 2.2f;
        constexpr u32 kSubmeshCount = 4;

        f32 QuadCenterX(u32 quad)
        {
            return -3.0f + 2.0f * static_cast<f32>(quad);
        }

        void AppendFloats(std::vector<u8>& buffer, std::initializer_list<f32> values)
        {
            for (f32 const value : values)
            {
                const auto* bytes = reinterpret_cast<const u8*>(&value);
                buffer.insert(buffer.end(), bytes, bytes + sizeof(f32));
            }
        }

        AuthoredFixture AuthorMaterialFixture()
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path dir = fs::temp_directory_path() / "OloEngineSubmeshMaterialFixture";
            fs::create_directories(dir, ec);
            if (ec)
            {
                return {};
            }

            // ---- textures ----
            constexpr int kTexSize = 16;
            {
                std::vector<u8> checker(kTexSize * kTexSize * 4);
                for (int y = 0; y < kTexSize; ++y)
                {
                    for (int x = 0; x < kTexSize; ++x)
                    {
                        const bool on = ((x / 4) + (y / 4)) % 2 == 0;
                        const sizet i = (static_cast<sizet>(y) * kTexSize + x) * 4;
                        checker[i + 0] = on ? 230 : 40;
                        checker[i + 1] = on ? 180 : 40;
                        checker[i + 2] = on ? 60 : 40;
                        checker[i + 3] = 255;
                    }
                }
                if (::stbi_write_png((dir / "checker.png").string().c_str(), kTexSize, kTexSize, 4, checker.data(),
                                     kTexSize * 4) == 0)
                {
                    return {};
                }
            }
            {
                // Alpha-masked albedo: the LEFT half of the quad is cut away entirely.
                std::vector<u8> mask(kTexSize * kTexSize * 4);
                for (int y = 0; y < kTexSize; ++y)
                {
                    for (int x = 0; x < kTexSize; ++x)
                    {
                        const sizet i = (static_cast<sizet>(y) * kTexSize + x) * 4;
                        mask[i + 0] = 60;
                        mask[i + 1] = 210;
                        mask[i + 2] = 90;
                        mask[i + 3] = (x < kTexSize / 2) ? 0 : 255;
                    }
                }
                if (::stbi_write_png((dir / "leaf.png").string().c_str(), kTexSize, kTexSize, 4, mask.data(),
                                     kTexSize * 4) == 0)
                {
                    return {};
                }
            }
            {
                // Uniform tangent-space normal, far from neutral, with blue = 0 — the BC5/RGTC
                // case (#440): reconstructing z gives +0.585, SAMPLING the blue channel gives -1.
                std::vector<u8> normal(kTexSize * kTexSize * 4);
                for (sizet i = 0; i < normal.size(); i += 4)
                {
                    normal[i + 0] = 220;
                    normal[i + 1] = 40;
                    normal[i + 2] = 0;
                    normal[i + 3] = 255;
                }
                if (::stbi_write_png((dir / "normal.png").string().c_str(), kTexSize, kTexSize, 4, normal.data(),
                                     kTexSize * 4) == 0)
                {
                    return {};
                }
            }

            // ---- geometry: one 144-byte block per quad (pos 48 | nrm 48 | uv 32 | idx 12 | pad 4) ----
            std::vector<u8> geometry;
            for (u32 q = 0; q < kSubmeshCount; ++q)
            {
                const f32 xc = QuadCenterX(q);
                AppendFloats(geometry, { xc - kQuadHalfWidth, kQuadBottom, 0.0f });
                AppendFloats(geometry, { xc + kQuadHalfWidth, kQuadBottom, 0.0f });
                AppendFloats(geometry, { xc + kQuadHalfWidth, kQuadTop, 0.0f });
                AppendFloats(geometry, { xc - kQuadHalfWidth, kQuadTop, 0.0f });
                for (u32 v = 0; v < 4; ++v)
                {
                    // Normals stay +Z even on the two-sided quad: the winding alone makes it
                    // back-facing, so the test isolates the CULL, not the lighting.
                    AppendFloats(geometry, { 0.0f, 0.0f, 1.0f });
                }
                AppendFloats(geometry, { 0.0f, 1.0f });
                AppendFloats(geometry, { 1.0f, 1.0f });
                AppendFloats(geometry, { 1.0f, 0.0f });
                AppendFloats(geometry, { 0.0f, 0.0f });

                // Quad 2 is wound backwards: it faces AWAY from the camera and only survives if
                // its doubleSided flag reaches the rasterizer's cull state on every path.
                const std::array<u16, 6> front = { 0, 1, 2, 2, 3, 0 };
                const std::array<u16, 6> back = { 0, 2, 1, 2, 0, 3 };
                for (u16 const index : (q == 2 ? back : front))
                {
                    const auto* bytes = reinterpret_cast<const u8*>(&index);
                    geometry.insert(geometry.end(), bytes, bytes + sizeof(u16));
                }
                geometry.insert(geometry.end(), 4, 0u); // pad to a 4-byte boundary
            }
            {
                std::ofstream bin(dir / "quads.bin", std::ios::binary);
                bin.write(reinterpret_cast<const char*>(geometry.data()), static_cast<std::streamsize>(geometry.size()));
            }

            // ---- the glTF ----
            std::ostringstream json;
            json << "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                    "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[";
            for (u32 q = 0; q < kSubmeshCount; ++q)
            {
                json << (q > 0 ? "," : "") << "{\"attributes\":{\"POSITION\":" << (q * 4) << ",\"NORMAL\":"
                     << (q * 4 + 1) << ",\"TEXCOORD_0\":" << (q * 4 + 2) << "},\"indices\":" << (q * 4 + 3)
                     << ",\"material\":" << q << "}";
            }
            json << "]}],\"buffers\":[{\"uri\":\"quads.bin\",\"byteLength\":" << geometry.size() << "}],"
                                                                                                    "\"bufferViews\":[";
            for (u32 q = 0; q < kSubmeshCount; ++q)
            {
                const u32 base = q * 144;
                json << (q > 0 ? "," : "") << "{\"buffer\":0,\"byteOffset\":" << base << ",\"byteLength\":48},"
                     << "{\"buffer\":0,\"byteOffset\":" << (base + 48) << ",\"byteLength\":48},"
                     << "{\"buffer\":0,\"byteOffset\":" << (base + 96) << ",\"byteLength\":32},"
                     << "{\"buffer\":0,\"byteOffset\":" << (base + 128) << ",\"byteLength\":12}";
            }
            json << "],\"accessors\":[";
            for (u32 q = 0; q < kSubmeshCount; ++q)
            {
                const f32 xc = QuadCenterX(q);
                json << (q > 0 ? "," : "") << "{\"bufferView\":" << (q * 4)
                     << ",\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[" << (xc - kQuadHalfWidth)
                     << "," << kQuadBottom << ",0],\"max\":[" << (xc + kQuadHalfWidth) << "," << kQuadTop << ",0]},"
                     << "{\"bufferView\":" << (q * 4 + 1) << ",\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
                     << "{\"bufferView\":" << (q * 4 + 2) << ",\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
                     << "{\"bufferView\":" << (q * 4 + 3) << ",\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}";
            }
            json << "],\"images\":[{\"uri\":\"checker.png\"},{\"uri\":\"leaf.png\"},{\"uri\":\"normal.png\"}],"
                    "\"samplers\":[{}],"
                    "\"textures\":[{\"source\":0,\"sampler\":0},{\"source\":1,\"sampler\":0},"
                    "{\"source\":2,\"sampler\":0}],"
                    "\"materials\":["
                    // 0: TEXTURED
                    "{\"name\":\"Textured\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
                    "\"metallicFactor\":0.0,\"roughnessFactor\":0.6}},"
                    // 1: ALPHA-MASKED
                    "{\"name\":\"AlphaMasked\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1},"
                    "\"metallicFactor\":0.0,\"roughnessFactor\":0.8},\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5},"
                    // 2: TWO-SIDED
                    "{\"name\":\"TwoSided\",\"doubleSided\":true,\"pbrMetallicRoughness\":"
                    "{\"baseColorFactor\":[0.1,0.85,0.9,1.0],\"metallicFactor\":0.0,\"roughnessFactor\":0.5}},"
                    // 3: NORMAL-MAPPED
                    "{\"name\":\"NormalMapped\",\"normalTexture\":{\"index\":2},\"pbrMetallicRoughness\":"
                    "{\"baseColorFactor\":[0.85,0.30,0.85,1.0],\"metallicFactor\":0.0,\"roughnessFactor\":0.3}}"
                    "]}";

            const fs::path gltf = dir / "MaterialParity.gltf";
            {
                std::ofstream out(gltf);
                out << json.str();
            }
            return { gltf, true };
        }

        // ── The metric: a PER-PIXEL, PER-CHANNEL diff over FLAT interiors ────────────────
        //
        // The old metric was a per-hue pixel CENSUS at a 25% relative tolerance — a coverage
        // metric, the exact species this whole batch of work condemned: it cannot see a wrong
        // texture, an inverted normal, a missing cutout or a wrong shade, only a wrong AMOUNT
        // of roughly-that-colour.
        //
        // "Interior" here means: every pixel whose 3x3 neighbourhood is FLAT in BOTH frames
        // (max-min channel range <= kFlatRange). That erodes silhouettes, shadow edges and
        // checker edges — where two rasterizers legitimately disagree by a subpixel — while
        // keeping the interiors of the quads, of the ground, and of the shadow cores, which is
        // where a shading, texturing or material-resolution difference lives.
        struct ChannelDiff
        {
            u32 ComparedPixels = 0;
            f64 MeanAbs[3] = { 0.0, 0.0, 0.0 };
            u32 MaxAbs[3] = { 0, 0, 0 };
            u32 OverToleranceCount = 0;
        };

        constexpr u32 kFlatRange = 10;        // a 3x3 neighbourhood this flat is an interior
        constexpr u32 kChannelTolerance = 12; // 8-bit levels

        bool IsFlat(const std::vector<u8>& f, u32 x, u32 y, u32 width)
        {
            for (u32 c = 0; c < 3; ++c)
            {
                i32 lo = 255;
                i32 hi = 0;
                for (i32 dy = -1; dy <= 1; ++dy)
                {
                    for (i32 dx = -1; dx <= 1; ++dx)
                    {
                        const sizet n = ((static_cast<sizet>(y) + dy) * width + (static_cast<sizet>(x) + dx)) * 4 + c;
                        lo = std::min(lo, static_cast<i32>(f[n]));
                        hi = std::max(hi, static_cast<i32>(f[n]));
                    }
                }
                if (static_cast<u32>(hi - lo) > kFlatRange)
                {
                    return false;
                }
            }
            return true;
        }

        ChannelDiff DiffInterior(const std::vector<u8>& a, const std::vector<u8>& b, u32 width, u32 height)
        {
            ChannelDiff diff;
            u64 sums[3] = { 0, 0, 0 };
            for (u32 y = 1; y + 1 < height; ++y)
            {
                for (u32 x = 1; x + 1 < width; ++x)
                {
                    if (!IsFlat(a, x, y, width) || !IsFlat(b, x, y, width))
                    {
                        continue;
                    }
                    const sizet i = (static_cast<sizet>(y) * width + x) * 4;
                    ++diff.ComparedPixels;
                    bool over = false;
                    for (u32 c = 0; c < 3; ++c)
                    {
                        const auto d =
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

        // ── Per-submesh identification: DIFFERENCE FROM THE EMPTY SCENE ─────────────────
        //
        // "Is submesh q on screen, and how much of it?" is answered by projecting that quad's
        // own world rectangle to pixels and counting how many of those pixels CHANGED relative
        // to a frame of the same scene with nothing staged. No hue thresholds, no assumptions
        // about how a material happens to be lit — a submesh that a path DROPPED (the two-sided
        // one backface-culled, the alpha-masked one vanishing because its albedo failed to
        // load) leaves its rectangle identical to the empty scene.
        //
        // It also measures the CUTOUT: the alpha-masked quad's albedo has alpha = 0 over its
        // left half, so it must fill only about half its rectangle. That is a per-submesh
        // coverage statement, which is exactly what a whole-frame hue census cannot make.
        constexpr std::array<const char*, kSubmeshCount> kSubmeshNames = { "textured", "alpha-masked", "two-sided",
                                                                           "normal-mapped" };
        // ── Shadows ─────────────────────────────────────────────────────────────────────
        //
        // The sun points (0, -0.8, +0.6), so a quad's shadow lands on the ground directly IN
        // FRONT of it (between the row and the camera): same x, z from +(0.6/0.8 * 0.6) = 0.45
        // (under its bottom edge) to +(2.2/0.8 * 0.6) = 1.65 (under its top edge). Project that
        // world rectangle with the very camera the capture used and count how much went dark.
        //
        // The reference is measured, not assumed: the same-sized patch of ground beside the
        // row (x = +5) is lit by definition, so "shadowed" means "clearly darker than that".
        EditorCamera MakeCaptureCamera()
        {
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 2.6f, 7.5f), 0.0f, glm::radians(12.0f));
            return camera;
        }

        struct ScreenRect
        {
            u32 X0 = 0, X1 = 0, Y0 = 0, Y1 = 0;
            bool Valid = false;
        };

        // World-space axis-aligned patch on the ground (y = 0) -> pixel bounding box.
        // The GL readback is bottom-up, so NDC y maps straight to the row index.
        ScreenRect ProjectGroundPatch(f32 x0, f32 x1, f32 z0, f32 z1)
        {
            const glm::mat4 viewProjection = MakeCaptureCamera().GetViewProjection();
            f32 minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
            for (const glm::vec3 corner : { glm::vec3(x0, 0.0f, z0), glm::vec3(x1, 0.0f, z0), glm::vec3(x0, 0.0f, z1),
                                            glm::vec3(x1, 0.0f, z1) })
            {
                const glm::vec4 clip = viewProjection * glm::vec4(corner, 1.0f);
                if (clip.w <= 0.0f)
                {
                    return {};
                }
                const f32 px = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<f32>(kWidth);
                const f32 py = (clip.y / clip.w * 0.5f + 0.5f) * static_cast<f32>(kHeight);
                minX = std::min(minX, px);
                maxX = std::max(maxX, px);
                minY = std::min(minY, py);
                maxY = std::max(maxY, py);
            }
            ScreenRect rect;
            rect.X0 = static_cast<u32>(std::clamp(minX, 0.0f, static_cast<f32>(kWidth - 1)));
            rect.X1 = static_cast<u32>(std::clamp(maxX, 0.0f, static_cast<f32>(kWidth - 1)));
            rect.Y0 = static_cast<u32>(std::clamp(minY, 0.0f, static_cast<f32>(kHeight - 1)));
            rect.Y1 = static_cast<u32>(std::clamp(maxY, 0.0f, static_cast<f32>(kHeight - 1)));
            rect.Valid = rect.X1 > rect.X0 && rect.Y1 > rect.Y0;
            return rect;
        }

        // The quad's own world rectangle (it stands in the z = 0 plane) -> pixel bounding box.
        ScreenRect ProjectQuad(u32 quad)
        {
            const glm::mat4 viewProjection = MakeCaptureCamera().GetViewProjection();
            const f32 xc = QuadCenterX(quad);
            f32 minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
            for (const glm::vec3 corner : { glm::vec3(xc - kQuadHalfWidth, kQuadBottom, 0.0f),
                                            glm::vec3(xc + kQuadHalfWidth, kQuadBottom, 0.0f),
                                            glm::vec3(xc - kQuadHalfWidth, kQuadTop, 0.0f),
                                            glm::vec3(xc + kQuadHalfWidth, kQuadTop, 0.0f) })
            {
                const glm::vec4 clip = viewProjection * glm::vec4(corner, 1.0f);
                if (clip.w <= 0.0f)
                {
                    return {};
                }
                const f32 px = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<f32>(kWidth);
                const f32 py = (clip.y / clip.w * 0.5f + 0.5f) * static_cast<f32>(kHeight);
                minX = std::min(minX, px);
                maxX = std::max(maxX, px);
                minY = std::min(minY, py);
                maxY = std::max(maxY, py);
            }
            ScreenRect rect;
            rect.X0 = static_cast<u32>(std::clamp(minX, 0.0f, static_cast<f32>(kWidth - 1)));
            rect.X1 = static_cast<u32>(std::clamp(maxX, 0.0f, static_cast<f32>(kWidth - 1)));
            rect.Y0 = static_cast<u32>(std::clamp(minY, 0.0f, static_cast<f32>(kHeight - 1)));
            rect.Y1 = static_cast<u32>(std::clamp(maxY, 0.0f, static_cast<f32>(kHeight - 1)));
            rect.Valid = rect.X1 > rect.X0 && rect.Y1 > rect.Y0;
            return rect;
        }

        // Pixels of quad q's rectangle that differ from the empty-scene frame.
        u32 QuadPixels(const std::vector<u8>& frame, const std::vector<u8>& empty, u32 quad)
        {
            const ScreenRect rect = ProjectQuad(quad);
            if (!rect.Valid || frame.size() != empty.size())
            {
                return 0;
            }
            u32 count = 0;
            for (u32 y = rect.Y0; y <= rect.Y1; ++y)
            {
                for (u32 x = rect.X0; x <= rect.X1; ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4;
                    i32 delta = 0;
                    for (u32 c = 0; c < 3; ++c)
                    {
                        delta = std::max(delta, std::abs(static_cast<i32>(frame[i + c]) - static_cast<i32>(empty[i + c])));
                    }
                    count += (delta > 20) ? 1u : 0u;
                }
            }
            return count;
        }

        bool IsGreyPixel(i32 r, i32 g, i32 b)
        {
            const i32 hi = std::max({ r, g, b });
            const i32 lo = std::min({ r, g, b });
            return (hi - lo) < 30;
        }

        f32 MeanGroundLuminance(const std::vector<u8>& frame, const ScreenRect& rect)
        {
            f64 sum = 0.0;
            u32 count = 0;
            for (u32 y = rect.Y0; y <= rect.Y1; ++y)
            {
                for (u32 x = rect.X0; x <= rect.X1; ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4;
                    const i32 r = frame[i];
                    const i32 g = frame[i + 1];
                    const i32 b = frame[i + 2];
                    if (!IsGreyPixel(r, g, b))
                    {
                        continue;
                    }
                    sum += (r + g + b) / 3.0;
                    ++count;
                }
            }
            return count > 0 ? static_cast<f32>(sum / count) : 0.0f;
        }

        // Grey ground pixels in the quad's shadow footprint that are clearly darker than the
        // lit reference patch beside the row.
        u32 ShadowPixelsUnderQuad(const std::vector<u8>& frame, u32 quad)
        {
            const f32 xc = QuadCenterX(quad);
            const ScreenRect reference = ProjectGroundPatch(4.6f, 6.2f, 0.45f, 1.65f);
            const ScreenRect footprint = ProjectGroundPatch(xc - kQuadHalfWidth, xc + kQuadHalfWidth, 0.45f, 1.65f);
            if (!reference.Valid || !footprint.Valid)
            {
                return 0;
            }
            const f32 lit = MeanGroundLuminance(frame, reference);
            if (lit <= 1.0f)
            {
                return 0; // no lit ground to compare against
            }
            const auto shadowThreshold = static_cast<i32>(lit * 0.65f);

            u32 count = 0;
            for (u32 y = footprint.Y0; y <= footprint.Y1; ++y)
            {
                for (u32 x = footprint.X0; x <= footprint.X1; ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4;
                    const i32 r = frame[i];
                    const i32 g = frame[i + 1];
                    const i32 b = frame[i + 2];
                    if (IsGreyPixel(r, g, b) && (r + g + b) / 3 < shadowThreshold)
                    {
                        ++count;
                    }
                }
            }
            return count;
        }
    } // namespace

    class SubmeshMaterialPathParity : public RendererAttachedTest
    {
      public:
        void BuildScene() override
        {
            // AssetManager::AddMemoryOnlyAsset needs an active project + asset manager
            // (same temp-project bootstrap as VirtualGeometryVisualEvidenceTest).
            if (!Project::GetActive() || !Project::GetAssetManager())
            {
                namespace fs = std::filesystem;
                std::error_code ec;
                const fs::path projectDir = fs::temp_directory_path() / "OloEngineSubmeshMaterialParity";
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Parity.oloproj");
                    proj << "Project:\n"
                            "  Name: SubmeshMaterialParity\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Parity.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false); // no file watcher in tests
                Project::SetAssetManager(assetManager);
            }

            // The AUTHORED fixture: four quads, four materials — textured / alpha-masked /
            // two-sided / normal-mapped. See AuthorMaterialFixture().
            const AuthoredFixture fixture = AuthorMaterialFixture();
            ASSERT_TRUE(fixture.Ok) << "could not author the glTF material fixture";
            ASSERT_TRUE(std::filesystem::exists(fixture.GltfPath));

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            // One import, three consumers: the Model (ModelComponent), the combined
            // MeshSource it produces (MeshComponent), and that same MeshSource registered
            // as an asset (VirtualMeshComponent). Same geometry, same imported materials —
            // any difference in the frames is a difference in material RESOLUTION.
            MeshCache::InvalidateCache(fixture.GltfPath); // always a COLD import: the .gltf is regenerated
            m_Model = Ref<Model>::Create(fixture.GltfPath.string());
            ASSERT_EQ(m_Model->GetMeshCount(), kSubmeshCount)
                << "the authored glTF must import as " << kSubmeshCount << " submeshes (one per material)";
            ASSERT_EQ(m_Model->GetMaterialCount(), kSubmeshCount) << "one material per submesh";

            // The fixture is only worth anything if the importer actually brought the four
            // properties across. Assert that BEFORE using it as an oracle — a silently
            // downgraded fixture (no texture, no cutoff, no doubleSided) is how the old test
            // ended up proving nothing.
            {
                const auto& materials = m_Model->GetMaterials();
                ASSERT_TRUE(materials[0] && materials[0]->GetAlbedoMap())
                    << "submesh 0 lost its ALBEDO TEXTURE on import — the fixture cannot see a texturing bug";
                ASSERT_TRUE(materials[0]->GetAlbedoMap()->GetSpecification().SRGB)
                    << "the albedo came in LINEAR — a baseColorTexture is colour data and must be sRGB";
                ASSERT_TRUE(materials[1]);
                ASSERT_EQ(materials[1]->GetAlphaMode(), AlphaMode::Mask)
                    << "submesh 1 lost its alphaMode=MASK — the fixture cannot see the cutout or the shadow gate";
                EXPECT_NEAR(materials[1]->GetAlphaCutoff(), 0.5f, 1e-4f);
                ASSERT_TRUE(materials[1]->GetAlbedoMap());
                ASSERT_TRUE(materials[2] && materials[2]->GetFlag(MaterialFlag::TwoSided))
                    << "submesh 2 lost doubleSided — it is wound backwards, so a path that backface-culls it will "
                       "simply drop it, which is what the fixture is FOR";
                ASSERT_TRUE(materials[3] && materials[3]->GetNormalMap())
                    << "submesh 3 lost its NORMAL MAP — the fixture cannot see a drifted TBN";
            }

            m_Combined = m_Model->CreateCombinedMeshSource();
            ASSERT_TRUE(m_Combined);
            m_Combined->Build();
            ASSERT_EQ(m_Combined->GetSubmeshes().Num(), static_cast<i32>(kSubmeshCount));

            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset(m_Combined);

            // ── Lighting, and why it is arranged this way ────────────────────────────────
            //
            // The SUN is BEHIND the quads (it travels toward +Z), for a measured reason: the
            // shadow pass culls the faces that FACE the light. A zero-thickness quad lit from
            // the front therefore casts NOTHING — verified: with the sun on the camera side,
            // only the reverse-wound two-sided quad cast a shadow at all, and every "does this
            // submesh cast a shadow?" assertion below would have been vacuous. With the sun
            // behind, every quad presents its far face to the light and casts a shadow forward,
            // onto the ground BETWEEN the row and the camera, in full view.
            {
                Entity sun = scene.CreateEntity("Sun");
                auto& dl = sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.0f, -0.80f, 0.60f));
                dl.m_Color = glm::vec3(1.0f, 0.98f, 0.95f);
                dl.m_Intensity = 4.0f;
                dl.m_CastShadows = true;
            }

            // A FILL light on the camera side, casting no shadows: the sun is behind the quads,
            // so without this their camera-facing sides would be nearly black and every
            // per-material hue — which is how each submesh is identified — would be unreadable.
            {
                Entity fill = scene.CreateEntity("Fill");
                fill.GetComponent<TransformComponent>().Translation = { 0.0f, 3.0f, 6.0f };
                auto& pl = fill.AddComponent<PointLightComponent>();
                pl.m_Color = glm::vec3(1.0f, 1.0f, 1.0f);
                pl.m_Intensity = 60.0f;
                pl.m_Range = 40.0f;
                pl.m_CastShadows = false;
            }

            // Ground: the shadow receiver. Its top surface sits at y = 0, the quads start at
            // y = 0.6, so every opaque quad throws a shadow onto it.
            {
                Entity ground = scene.CreateEntity("Ground");
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -0.25f, -2.0f };
                tc.Scale = { 40.0f, 0.5f, 24.0f };
                auto& mc = ground.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> cube = MeshPrimitives::CreateCube(); cube)
                {
                    mc.m_MeshSource = cube->GetMeshSource();
                }
                auto& mat = ground.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.55f, 0.55f, 0.55f, 1.0f));
                mat.m_Material.SetRoughnessFactor(0.9f);
            }

            // The quads are authored at their final world size — no framing scale.
            constexpr f32 kModelScale = 1.0f;

            m_ModelEntity = scene.CreateEntity("ModelPath");
            {
                auto& tc = m_ModelEntity.GetComponent<TransformComponent>();
                tc.Translation = kParked;
                tc.Scale = glm::vec3(kModelScale);
                auto& mc = m_ModelEntity.AddComponent<ModelComponent>();
                mc.m_Model = m_Model;
                mc.m_Visible = true;
            }

            m_MeshEntity = scene.CreateEntity("MeshPath");
            {
                auto& tc = m_MeshEntity.GetComponent<TransformComponent>();
                tc.Translation = kParked;
                tc.Scale = glm::vec3(kModelScale);
                auto& mc = m_MeshEntity.AddComponent<MeshComponent>();
                mc.m_MeshSource = m_Combined;
            }

            m_VirtualEntity = scene.CreateEntity("VirtualPath");
            {
                auto& tc = m_VirtualEntity.GetComponent<TransformComponent>();
                tc.Translation = kParked;
                tc.Scale = glm::vec3(kModelScale);
                auto& vm = m_VirtualEntity.AddComponent<VirtualMeshComponent>();
                vm.m_MeshSource = handle;
                vm.m_ErrorThresholdPixels = 0.05f; // finest cut: LOD 0, the source triangles
            }
        }

        // Stages exactly one renderable at the origin and renders it. Passing a default-
        // constructed Entity stages NOTHING — that is the empty-scene reference frame every
        // per-submesh measurement is differenced against.
        std::vector<u8> CapturePath(const char* name, Entity staged)
        {
            for (Entity e : { m_ModelEntity, m_MeshEntity, m_VirtualEntity })
            {
                e.GetComponent<TransformComponent>().Translation = (e == staged) ? kStage : kParked;
            }

            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 2.6f, 7.5f), 0.0f, glm::radians(12.0f));
            RunEditorFrames(camera, 4);

            std::vector<u8> rgba;
            u32 w = 0;
            u32 h = 0;
            if (!ReadbackComposite(rgba, w, h))
            {
                ADD_FAILURE() << "composite readback unavailable for " << name;
                return {};
            }
            WriteEvidencePng(std::string("SubmeshMaterialParity_") + name + ".png", rgba, w, h);
            return rgba;
        }

        Ref<Model> m_Model;
        Ref<MeshSource> m_Combined;
        Entity m_ModelEntity;
        Entity m_MeshEntity;
        Entity m_VirtualEntity;
    };

    // The three paths must produce the SAME PICTURE of a mesh whose submeshes are textured,
    // alpha-masked, two-sided and normal-mapped — measured per pixel, per channel.
    TEST_F(SubmeshMaterialPathParity, ClassicAndVirtualPathsShadeEverySubmeshWithItsImportedMaterial)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // VirtualGeometryPass is Deferred-only; the classic paths render there too, so
        // Deferred is the one path where all three are directly comparable.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        const std::vector<u8> empty = CapturePath("Empty", Entity{}); // nothing staged: the reference
        const std::vector<u8> model = CapturePath("Model", m_ModelEntity);
        const std::vector<u8> mesh = CapturePath("Mesh", m_MeshEntity);
        const std::vector<u8> virt = CapturePath("Virtual", m_VirtualEntity);
        ASSERT_EQ(model.size(), empty.size());
        ASSERT_EQ(model.size(), mesh.size());
        ASSERT_EQ(model.size(), virt.size());
        ASSERT_FALSE(model.empty());

        // 1. Every path must still show the imported materials at all. (The old census, kept
        //    only as a smoke check — it is NOT the metric.)
        const ColourCensus modelCensus = Census(model);
        const ColourCensus meshCensus = Census(mesh);
        const ColourCensus virtCensus = Census(virt);
        EXPECT_GT(modelCensus.Saturated, 500u) << "ModelComponent lost the imported materials";
        EXPECT_GT(meshCensus.Saturated, 500u)
            << "MeshComponent rendered no coloured pixels — it is shading every submesh with ONE material again "
               "(the imported material is being ignored)";
        EXPECT_GT(virtCensus.Saturated, 500u) << "VirtualMeshComponent lost the imported materials";

        // 2. THE METRIC: a per-pixel, per-channel diff on the flat interiors. This sees a
        //    wrong texture, a wrong colour space, an inverted normal, a missing cutout and a
        //    missing two-sided quad — none of which a coverage census can see.
        for (const auto& [name, frame] : std::array<std::pair<const char*, const std::vector<u8>*>, 2>{
                 { { "MeshComponent", &mesh }, { "VirtualMeshComponent", &virt } } })
        {
            const ChannelDiff diff = DiffInterior(model, *frame, kWidth, kHeight);
            ASSERT_GT(diff.ComparedPixels, 20000u)
                << name << ": too few flat interior pixels to compare — the frames are empty or all edges";

            for (u32 c = 0; c < 3; ++c)
            {
                EXPECT_LT(diff.MeanAbs[c], 2.0)
                    << name << " vs ModelComponent, channel " << c << ": the two paths SHADE the same mesh "
                    << "differently (mean |delta| = " << diff.MeanAbs[c] << "/255, max = " << diff.MaxAbs[c]
                    << " over " << diff.ComparedPixels << " flat interior pixels). Every submission path must "
                    << "resolve the material through SubmeshMaterialResolve.h and shade it through PBRCommon.";
            }
            EXPECT_LT(diff.OverToleranceCount, diff.ComparedPixels / 100u)
                << name << ": " << diff.OverToleranceCount << " of " << diff.ComparedPixels
                << " flat interior pixels differ from the ModelComponent frame by more than " << kChannelTolerance
                << " levels in some channel — that is a shading/material divergence, not an edge rule";
        }

        // 3. Per-submesh, per-path presence. Each quad owns a vertical STRIP of the frame
        //    (they are evenly spaced in x and the camera is centred), so a submesh that a path
        //    dropped — the two-sided one being backface-culled, the alpha-masked one vanishing
        //    because its albedo failed to load — shows up as an empty strip.
        for (const auto& [name, frame] : std::array<std::pair<const char*, const std::vector<u8>*>, 3>{
                 { { "Model", &model }, { "Mesh", &mesh }, { "Virtual", &virt } } })
        {
            for (u32 q = 0; q < kSubmeshCount; ++q)
            {
                EXPECT_GT(QuadPixels(*frame, empty, q), 300u)
                    << name << ": submesh " << q << " (" << kSubmeshNames[q] << ") is MISSING from the frame. "
                    << (q == 1   ? "An alpha-masked submesh whose albedo failed to load fails the cutoff and the "
                                   "whole submesh vanishes. "
                        : q == 2 ? "This quad is wound backwards: if its doubleSided flag does not reach "
                                   "the rasterizer's cull state, it is culled away entirely. "
                                 : "");
            }

            // The alpha-masked quad must be HALF cut away — that is the cutout working. A path
            // that ignored alphaMode would render the whole quad.
            const u32 masked = QuadPixels(*frame, empty, 1);
            const u32 textured = QuadPixels(*frame, empty, 0);
            EXPECT_LT(masked, textured * 3u / 4u)
                << name << ": the ALPHA-MASKED quad covers " << masked << " pixels and the opaque textured one "
                << textured << " — the mask cuts away the left half of its albedo, so it must cover clearly LESS. "
                               "It does not: the cutout is not being applied (alphaMode/alphaCutoff did not reach the shader).";
        }
    }

    // NOTHING in any picture test measured SHADOWS before this — and the shadow gate is where
    // C8 shipped: the "does this cast a shadow?" question was asked of the ENTITY's
    // MaterialComponent, so an entity WITHOUT one (which is every entity here, and every
    // entity in a plain multi-material import) cast shadows unconditionally. Alpha-masked
    // foliage projected SOLID QUAD silhouettes: the shadow-depth shader never samples the
    // albedo alpha, so a leaf card shadows like a plank. The gate now asks the RESOLVED
    // per-submesh material (MaterialCastsShadows), on every path.
    TEST_F(SubmeshMaterialPathParity, AnAlphaMaskedSubmeshCastsNoSolidShadowOnAnyPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        for (const auto& [name, entity] : std::array<std::pair<const char*, Entity>, 3>{
                 { { "Model", m_ModelEntity }, { "Mesh", m_MeshEntity }, { "Virtual", m_VirtualEntity } } })
        {
            const std::vector<u8> frame = CapturePath((std::string("Shadow") + name).c_str(), entity);
            ASSERT_FALSE(frame.empty());

            // The ground strip in FRONT of each quad, where its shadow lands.
            const u32 texturedShadow = ShadowPixelsUnderQuad(frame, 0);  // opaque -> MUST cast
            const u32 maskedShadow = ShadowPixelsUnderQuad(frame, 1);    // masked -> must NOT
            const u32 normalMapShadow = ShadowPixelsUnderQuad(frame, 3); // opaque -> MUST cast
            //
            // Quad 2 (two-sided) is deliberately NOT asserted on: it is wound backwards, and the
            // shadow pass culls the face that is turned toward the light, so a reverse-wound
            // zero-thickness quad casts nothing under this back-light (measured, both ways round:
            // with the sun on the CAMERA side it was the only quad that DID cast). That is a
            // property of the shadow pass's cull mode, not of the material resolution this test
            // is about — asserting either way would pin an unrelated behaviour.

            // Non-vacuity FIRST: if the opaque quads cast no shadow either, the scene has no
            // shadows at all and "the masked one casts none" is worth nothing.
            ASSERT_GT(texturedShadow, 150u)
                << name << ": the OPAQUE textured quad casts no shadow — this scene is not producing shadows at "
                           "all, so the alpha-masked assertion below would be vacuous. Check the sun, the cascade "
                           "range and the ground.";
            EXPECT_GT(normalMapShadow, 150u) << name << ": the opaque normal-mapped quad casts no shadow";

            EXPECT_LT(maskedShadow, texturedShadow / 5u)
                << name << ": the ALPHA-MASKED submesh cast a shadow of " << maskedShadow
                << " dark ground pixels, against " << texturedShadow << " for the opaque quad next to it. "
                                                                        "An alpha-masked material must not cast a shadow through the shared shadow-depth shader — that "
                                                                        "shader never samples the albedo alpha, so a cutout leaf card projects as a SOLID plank. The "
                                                                        "gate must ask the RESOLVED per-submesh material (MaterialCastsShadows), not the entity's "
                                                                        "MaterialComponent — an entity without one used to cast shadows unconditionally.";
        }
    }

    // -- C1, the half no importer can produce: TWO SUBMESHES SHARING ONE MATERIAL --
    //
    // Submesh::m_MaterialIndex indexes the DEDUPLICATED imported-material array — one entry per
    // UNIQUE material, so two submeshes may legitimately point at the SAME entry. The bug wrote
    // the submesh's OWN index there instead ("one material slot per submesh"), which addresses a
    // different array. The two conventions agree on every asset in this repo, because
    // aiProcess_PreTransformVertices MERGES primitives that share a material, so Assimp always
    // hands back exactly one mesh per unique material (measured: Sponza's 103 glTF primitives /
    // 25 materials -> 25 meshes / 25 materials). The test below therefore could not tell the two
    // conventions apart, and said so — which is another way of saying it was pinning its
    // invariant by coincidence.
    //
    // So construct the case by hand: three submeshes, TWO materials, submesh 2 sharing submesh
    // 0's. The negative control at the end is the old convention, spelled out, so the positive
    // assertion cannot be vacuous: under `m_MaterialIndex = submeshIndex` the third submesh
    // indexes off the end of a two-entry array, GetImportedMaterialForSubmesh returns null, and
    // the resolver falls through to flat engine-default grey. That is what "it goes live the
    // moment a load yields two meshes sharing a material" means, in numbers.
    //
    // No GL needed: this is the resolver's addressing contract, not a picture.
    TEST(ModelCombinedMaterialIndex, SubmeshesSharingAMaterialResolveToTheSameImportedMaterial)
    {
        const Material engineDefault{};

        Ref<Material> const red = Material::CreatePBR("Red", glm::vec3(0.9f, 0.1f, 0.1f));
        Ref<Material> const green = Material::CreatePBR("Green", glm::vec3(0.1f, 0.9f, 0.1f));
        ASSERT_TRUE(red);
        ASSERT_TRUE(green);

        // Three quads; the DEDUPLICATED material array has two entries.
        const auto makeMesh = [](const std::array<u32, 3>& materialIndices) -> Ref<MeshSource>
        {
            TArray<Vertex> vertices;
            TArray<u32> indices;
            for (u32 s = 0; s < 3; ++s)
            {
                const auto baseVertex = static_cast<u32>(vertices.Num());
                const auto x = static_cast<f32>(s);
                vertices.Add(Vertex({ x + 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }));
                vertices.Add(Vertex({ x + 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }));
                vertices.Add(Vertex({ x + 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }));
                vertices.Add(Vertex({ x + 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }));
                for (u32 i : { 0u, 1u, 2u, 2u, 3u, 0u })
                {
                    indices.Add(baseVertex + i);
                }
            }
            auto meshSource = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));

            TArray<Submesh> submeshes;
            for (u32 s = 0; s < 3; ++s)
            {
                Submesh submesh;
                submesh.m_BaseVertex = s * 4;
                submesh.m_BaseIndex = s * 6;
                submesh.m_VertexCount = 4;
                submesh.m_IndexCount = 6;
                submesh.m_MaterialIndex = materialIndices[s];
                submesh.m_NodeName = "Submesh_" + std::to_string(s);
                submeshes.Add(submesh);
            }
            meshSource->SetSubmeshes(submeshes);
            return meshSource;
        };

        // THE CORRECT CONVENTION: indices into the deduplicated array. Submesh 2 shares 0's.
        Ref<MeshSource> deduplicated = makeMesh({ 0u, 1u, 0u });
        deduplicated->SetImportedMaterials({ red, green });

        const Material& m0 = ResolveSubmeshMaterial(nullptr, deduplicated.get(), 0, engineDefault);
        const Material& m1 = ResolveSubmeshMaterial(nullptr, deduplicated.get(), 1, engineDefault);
        const Material& m2 = ResolveSubmeshMaterial(nullptr, deduplicated.get(), 2, engineDefault);

        EXPECT_EQ(m0.GetName(), "Red");
        EXPECT_EQ(m1.GetName(), "Green");
        EXPECT_EQ(m2.GetName(), "Red")
            << "submesh 2 shares submesh 0's material (m_MaterialIndex == 0) but did not resolve to it — "
               "Submesh::m_MaterialIndex must address the DEDUPLICATED imported-material array";
        EXPECT_EQ(&m0, &m2) << "two submeshes sharing a material index must resolve to the SAME Material object";
        EXPECT_NE(m2.GetName(), engineDefault.GetName())
            << "submesh 2 fell through to the ENGINE DEFAULT — its material index does not address the array that "
               "ships alongside it. This is exactly the C1 failure mode, and it is invisible to every asset in the "
               "repo because PreTransformVertices merges primitives that share a material.";

        // THE NEGATIVE CONTROL — the old convention, `m_MaterialIndex = submeshIndex`, against
        // the same deduplicated two-entry array. This is not a regression guard; it is the proof
        // that the assertions above are not vacuous: they distinguish the two conventions, which
        // is precisely what no fixture in this repo can do.
        Ref<MeshSource> oldConvention = makeMesh({ 0u, 1u, 2u });
        oldConvention->SetImportedMaterials({ red, green });
        EXPECT_FALSE(oldConvention->GetImportedMaterialForSubmesh(2))
            << "the old 'one material slot per submesh' convention was supposed to index off the end of the "
               "deduplicated array here — if it does not, this fixture no longer distinguishes the conventions and "
               "the assertions above prove nothing";
        EXPECT_EQ(ResolveSubmeshMaterial(nullptr, oldConvention.get(), 2, engineDefault).GetName(),
                  engineDefault.GetName())
            << "under the old convention submesh 2 must land on the engine default — flat grey, which is what the "
               "bug rendered";
    }

    // -- C1: the combined MeshSource's material INDEX SPACE, cold vs warm --
    //
    // Model::CreateCombinedMeshSource used to write Submesh::m_MaterialIndex = the submesh's
    // OWN index, while the array that index is supposed to address is the DEDUPLICATED one the
    // cold path builds through m_MaterialIndexMap (one entry per unique aiMaterial) - and the
    // warm (.omesh) path rebuilt a different, one-entry-per-mesh array. Two index spaces, one
    // index. It now carries the submesh's real (deduplicated) material index on both paths,
    // and both paths build the same array.
    //
    // WHAT THIS TEST CANNOT DO, AND WHY (measured, not assumed):
    // Model::LoadModel imports with aiProcess_PreTransformVertices, which flattens the node
    // hierarchy and MERGES primitives that share a material. Every model therefore comes out of
    // Assimp with exactly one mesh per unique material - measured: deccer Colored 5 meshes / 5
    // materials; deccer Textured_Complex 19 glTF primitives -> 5 meshes / 5 materials;
    // With_Rotation 9 primitives -> 1 mesh / 1 material; Sponza 103 glTF primitives / 25
    // materials -> 25 meshes / 25 materials. So through THIS importer path "submesh index" and
    // "deduplicated material index" are always the same number, and NO fixture in the repo can
    // tell the two conventions apart. An earlier version of this test asserted
    // meshCount > materialCount to prove the fixture exercised deduplication; that assertion is
    // unsatisfiable by any asset here, which is itself the finding: the old convention was
    // coincidentally correct, so C1 is a LATENT bug, not an active one. It goes live the moment
    // a load yields two meshes sharing one material (importer flags change, PTV dropped for
    // instancing, a merge refused). The fix removes the coincidence; this test pins the
    // invariant that the two index spaces agree - cold, warm, and across the two consumers.
    //
    // Needs a GL context (Model::LoadModel builds GPU buffers), so it SKIPs without a GPU.
    TEST(ModelCombinedMaterialIndex, ColdImportAndWarmCacheLoadResolveTheSameMaterialPerSubmesh)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = ColoredCubesPath();
        ASSERT_TRUE(std::filesystem::exists(path)) << "deccer-cubes fixture missing: " << path.string();

        // What the ModelComponent path resolves per submesh: Model's own material array,
        // indexed by that submesh's material index.
        const auto modelMapping = [](const Model& model) -> std::vector<std::string>
        {
            const Material engineDefault{};
            std::vector<std::string> names;
            const auto& materials = model.GetMaterials();
            for (const Ref<Mesh>& submesh : model.GetMeshes())
            {
                if (!submesh)
                {
                    names.emplace_back("<null submesh>");
                    continue;
                }
                const u32 matIdx = submesh->GetSubmesh().m_MaterialIndex;
                const Material* imported = (matIdx < materials.size() && materials[matIdx]) ? materials[matIdx].get() : nullptr;
                names.push_back(imported != nullptr ? ResolveSubmeshMaterial(nullptr, imported, engineDefault).GetName()
                                                    : "<engine default>");
            }
            return names;
        };

        // What the MeshComponent / VirtualMeshComponent paths resolve: the COMBINED MeshSource
        // (the object the asset system hands them), indexed through ITS submesh table into ITS
        // imported-material array. If CreateCombinedMeshSource writes an index from one array
        // into a submesh that ships a different array, these two mappings diverge - that is
        // exactly the C1 defect, and it is invisible to any test that only looks at one of them.
        const auto combinedMapping = [](const MeshSource& combined) -> std::vector<std::string>
        {
            const Material engineDefault{};
            std::vector<std::string> names;
            for (i32 i = 0; i < combined.GetSubmeshes().Num(); ++i)
            {
                Ref<Material> const imported = combined.GetImportedMaterialForSubmesh(static_cast<u32>(i));
                names.push_back(imported ? ResolveSubmeshMaterial(nullptr, imported.get(), engineDefault).GetName()
                                         : "<engine default>");
            }
            return names;
        };

        // COLD: no cache. (Model::LoadModel writes the .omesh as a side effect of this load.)
        MeshCache::InvalidateCache(path);
        ASSERT_FALSE(MeshCache::IsMeshCacheValid(path)) << "cache invalidation did not take effect";

        Model cold{ path.string() };
        ASSERT_GT(cold.GetMeshCount(), 0u);
        ASSERT_GT(cold.GetMaterialCount(), 1u) << "fixture must be multi-material or this test proves nothing";

        Ref<MeshSource> const coldCombined = cold.CreateCombinedMeshSource();
        ASSERT_TRUE(coldCombined);
        const std::vector<std::string> coldModelNames = modelMapping(cold);
        const std::vector<std::string> coldCombinedNames = combinedMapping(*coldCombined);

        EXPECT_EQ(coldModelNames, coldCombinedNames)
            << "cold import: the combined MeshSource resolves different materials than the Model it came from - "
               "Submesh::m_MaterialIndex does not address the imported-material array shipped alongside it.";

        // WARM: the same file again, now served from the .omesh the cold load just wrote.
        ASSERT_TRUE(MeshCache::IsMeshCacheValid(path))
            << "the cold import did not write an .omesh - the warm half of this test would be vacuous";

        Model warm{ path.string() };
        ASSERT_GT(warm.GetMeshCount(), 0u);
        Ref<MeshSource> const warmCombined = warm.CreateCombinedMeshSource();
        ASSERT_TRUE(warmCombined);
        const std::vector<std::string> warmModelNames = modelMapping(warm);
        const std::vector<std::string> warmCombinedNames = combinedMapping(*warmCombined);

        EXPECT_EQ(warmModelNames, warmCombinedNames)
            << "warm cache load: the combined MeshSource and the Model disagree on the per-submesh material.";

        // And the two loads of the same file must agree with each other - the C1 symptom was
        // "the second load of a model renders differently from the first".
        ASSERT_EQ(coldModelNames.size(), warmModelNames.size()) << "cold and warm loads produced different submesh counts";
        EXPECT_EQ(coldModelNames, warmModelNames) << "the SAME model resolves different materials cold vs warm.";
        EXPECT_EQ(coldCombinedNames, warmCombinedNames)
            << "the SAME model's combined MeshSource resolves different materials cold vs warm.";

        // Non-vacuity: if every submesh resolved to the engine default (or all to one material),
        // every equality above would hold while proving nothing.
        const std::set<std::string> distinct(coldModelNames.begin(), coldModelNames.end());
        EXPECT_GT(distinct.size(), 1u)
            << "every submesh resolved to the same material - the multi-material fixture is being flattened";
        EXPECT_EQ(distinct.count("<engine default>"), 0u)
            << "at least one submesh fell through to the engine default: its m_MaterialIndex does not address "
               "the imported-material array.";
    }

    TEST_F(SubmeshMaterialPathParity, AMaterialComponentOverridesEverySubmeshOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // The other half of the precedence rule: an authored MaterialComponent must beat
        // every submesh's imported material. ModelComponent used to ignore it outright.
        for (Entity e : { m_ModelEntity, m_MeshEntity, m_VirtualEntity })
        {
            auto& mat = e.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.05f, 0.05f, 0.9f, 1.0f)); // pure blue
            mat.m_Material.SetRoughnessFactor(0.7f);
        }

        const ColourCensus model = Census(CapturePath("ModelOverride", m_ModelEntity));
        const ColourCensus mesh = Census(CapturePath("MeshOverride", m_MeshEntity));
        const ColourCensus virt = Census(CapturePath("VirtualOverride", m_VirtualEntity));

        // Everything the mesh covers must now be blue: no red/green survivors from the
        // imported materials, on any path.
        for (const auto& [name, census] : std::array<std::pair<const char*, ColourCensus>, 3>{
                 { { "Model", model }, { "Mesh", mesh }, { "Virtual", virt } } })
        {
            EXPECT_GT(census.BlueDominant, 500u) << name << " did not apply the MaterialComponent override";
            EXPECT_LT(census.RedDominant + census.GreenDominant, census.BlueDominant / 10u)
                << name << " still shows the IMPORTED materials through a MaterialComponent override";
        }
    }
} // namespace OloEngine::Tests
