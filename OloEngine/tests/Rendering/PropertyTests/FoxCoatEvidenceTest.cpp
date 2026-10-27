#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// FoxCoatEvidenceTest -- issue #1259, the animal herd's coats.
//
// The integrated acceptance scene's herd is the Khronos Fox (assets/models/Fox,
// authored in centimetres and placed at 0.011-0.014). Its coats were unbound
// 600-strand fixtures drawn at each fox's own transform: a small blob at the
// animal's origin. This file grows a SHORT (summer) and a LONG (winter) fox
// coat on the Fox's real bind-pose surface, cooks them and a card level with
// the real cookers, binds them with the real binder, and parents each coat to
// its fox, the way the live scene authors them (a-bound-coat-rides-its-body.md).
//
// THE COAT IS AUTHORED IN METRES. The groom space is the Fox's own space times
// kFoxCookScale, and the coat entity carries the inverse as its local scale, so
// inverse(coatWorld) * foxWorld is the binding's SurfaceToGroom at ANY fox
// scale: a fox placed at 0.014 wears the same cooked coat 12% larger. Grown in
// the Fox's centimetres instead, every caster frame's position tolerance
// (kGroomCasterPositionTolerance * (1 + |p|)) would be a hundred times looser.
//
// Evidence PNGs: OloEditor/assets/tests/visual/FoxCoat*_GL_*.png. GL only.
// With OLO_FOX_COATS_EXPORT=1, ExportsTheFoxCoats writes the cooked coats to
// SandboxProject/Assets/Grooms/Animals/ and registers them (see the case).
//
// RUN IT IN RELEASE: the coats are ~100k strands each.
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestTempDir.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomBindingCooker.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCoat.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomEvaluationScratch.h"
#include "OloEngine/Groom/GroomLodBuilder.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomStrandRequest.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/TemporalHistoryRegistry.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/ModelImporter.h"
#include "OloEngine/Scene/SceneCamera.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // Metres per Fox unit. The coat is grown at this scale and its entity
        // carries 1 / kFoxCookScale (exactly 80) under the fox, so the live
        // scene's foxes at 0.011-0.014 wear it in proportion.
        constexpr f32 kFoxCookScale = 0.0125f;

        // The card tier's hand-over, in pixels across the coat. The cards are
        // cooked for it (GroomCardSettings::SourcePixelSize) and the live
        // scene's GroomLodComponent authors the same value.
        constexpr f32 kCardPixelSize = 220.0f;

        [[nodiscard]] fs::path FoxPath()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "assets/models/Fox/Fox.gltf";
        }
        [[nodiscard]] fs::path FoxTexturePath()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "assets/models/Fox/Texture.png";
        }

        // A deterministic hash in [0,1), so a coat is a function of its inputs.
        [[nodiscard]] f32 Hash01(u32 a, u32 b)
        {
            u32 h = (a * 0x9E3779B1u) ^ (b * 0x85EBCA77u) ^ 0xC2B2AE3Du;
            h ^= h >> 15;
            h *= 0x2C1B3C6Du;
            h ^= h >> 12;
            h *= 0x297A2D39u;
            h ^= h >> 15;
            return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f);
        }

        // ── The Fox's own texture, read on the CPU ────────────────────────
        //
        // Four flat colours on an olive background: orange, cream, a dark
        // brown (legs, ear backs) and near-black (the nose and eye dots).
        struct Rgba8
        {
            u8 r = 0;
            u8 g = 0;
            u8 b = 0;
            u8 a = 0;
        };

        struct FoxTexture
        {
            int Width = 0;
            int Height = 0;
            std::vector<u8> Rgba;
            bool FlipV = false; // which way the mesh's TexCoord addresses rows

            [[nodiscard]] Rgba8 At(int x, int y) const
            {
                x = std::clamp(x, 0, Width - 1);
                y = std::clamp(y, 0, Height - 1);
                const sizet i = (static_cast<sizet>(y) * static_cast<sizet>(Width) + static_cast<sizet>(x)) * 4u;
                return { Rgba[i], Rgba[i + 1], Rgba[i + 2], Rgba[i + 3] };
            }
            [[nodiscard]] Rgba8 Sample(const glm::vec2& uv, bool flip) const
            {
                const f32 u = uv.x - std::floor(uv.x);
                f32 v = uv.y - std::floor(uv.y);
                if (flip)
                {
                    v = 1.0f - v;
                }
                return At(static_cast<int>(u * static_cast<f32>(Width)), static_cast<int>(v * static_cast<f32>(Height)));
            }
        };

        [[nodiscard]] bool IsBackground(const Rgba8& c)
        {
            return std::abs(c.r - 67) < 14 && std::abs(c.g - 63) < 14 && std::abs(c.b - 30) < 14;
        }
        // The nose and eye dots: nothing grows there.
        [[nodiscard]] bool IsBareSkin(const Rgba8& c)
        {
            return c.r < 60 && c.g < 60 && c.b < 60;
        }

        [[nodiscard]] FoxTexture LoadFoxTexture()
        {
            FoxTexture t;
            int channels = 0;
            stbi_set_flip_vertically_on_load_thread(0);
            u8* pixels = stbi_load(FoxTexturePath().string().c_str(), &t.Width, &t.Height, &channels, 4);
            EXPECT_NE(pixels, nullptr) << FoxTexturePath().string();
            if (pixels != nullptr)
            {
                t.Rgba.assign(pixels, pixels + static_cast<sizet>(t.Width) * static_cast<sizet>(t.Height) * 4u);
                stbi_image_free(pixels);
            }
            return t;
        }

        // ── FoxCoatColor.png, the coat's root-UV colour map ───────────────
        //
        // Derived from the Fox's texture, in the same UV layout: each flat
        // colour becomes a fox's own (rufous, cream, near-black stockings),
        // the islands are grown into the background so a root near a seam
        // never reads the olive, the region edges are softened as a pelt's
        // are, and a low-frequency value noise breaks up the flat fill.
        [[nodiscard]] std::vector<u8> BuildFoxCoatColorMap(const FoxTexture& tex)
        {
            const int w = tex.Width;
            const int h = tex.Height;
            struct Swatch
            {
                glm::vec3 Source;
                glm::vec3 Coat;
            };
            // sRGB. Rufous a little darker and redder than the cartoon orange,
            // cream rather than paper white, the brown taken to the near-black
            // of a red fox's stockings and ear backs.
            const std::array<Swatch, 4> swatches{ {
                { { 219.0f, 135.0f, 41.0f }, { 196.0f, 98.0f, 38.0f } },
                { { 255.0f, 250.0f, 242.0f }, { 238.0f, 230.0f, 214.0f } },
                { { 83.0f, 52.0f, 16.0f }, { 44.0f, 30.0f, 22.0f } },
                { { 37.0f, 37.0f, 37.0f }, { 22.0f, 20.0f, 19.0f } },
            } };
            std::vector<glm::vec3> colour(static_cast<sizet>(w) * static_cast<sizet>(h), glm::vec3(0.0f));
            std::vector<u8> filled(colour.size(), 0u);
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    const Rgba8 c = tex.At(x, y);
                    if (IsBackground(c))
                    {
                        continue;
                    }
                    // Soft assignment, so the texture's anti-aliased edges
                    // blend between the two swatches they sit between.
                    const glm::vec3 src(c.r, c.g, c.b);
                    glm::vec3 sum(0.0f);
                    f32 weight = 0.0f;
                    for (const Swatch& s : swatches)
                    {
                        const f32 d = glm::length(src - s.Source);
                        const f32 k = 1.0f / (1.0f + (d * d) / 400.0f);
                        sum += s.Coat * k;
                        weight += k;
                    }
                    const sizet i = static_cast<sizet>(y) * static_cast<sizet>(w) + static_cast<sizet>(x);
                    colour[i] = sum / weight;
                    filled[i] = 1u;
                }
            }
            // Grow the islands into the background, 24 texels.
            for (int pass = 0; pass < 24; ++pass)
            {
                std::vector<u8> next = filled;
                std::vector<glm::vec3> grown = colour;
                for (int y = 0; y < h; ++y)
                {
                    for (int x = 0; x < w; ++x)
                    {
                        const sizet i = static_cast<sizet>(y) * static_cast<sizet>(w) + static_cast<sizet>(x);
                        if (filled[i] != 0u)
                        {
                            continue;
                        }
                        glm::vec3 sum(0.0f);
                        u32 n = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                        {
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                const int nx = x + dx;
                                const int ny = y + dy;
                                if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                                {
                                    continue;
                                }
                                const sizet j = static_cast<sizet>(ny) * static_cast<sizet>(w) + static_cast<sizet>(nx);
                                if (filled[j] != 0u)
                                {
                                    sum += colour[j];
                                    ++n;
                                }
                            }
                        }
                        if (n > 0u)
                        {
                            grown[i] = sum / static_cast<f32>(n);
                            next[i] = 1u;
                        }
                    }
                }
                colour = std::move(grown);
                filled = std::move(next);
            }
            // Soften the region edges: two separable box passes, radius 3.
            for (int pass = 0; pass < 2; ++pass)
            {
                for (int axis = 0; axis < 2; ++axis)
                {
                    std::vector<glm::vec3> blurred(colour.size());
                    for (int y = 0; y < h; ++y)
                    {
                        for (int x = 0; x < w; ++x)
                        {
                            glm::vec3 sum(0.0f);
                            for (int k = -3; k <= 3; ++k)
                            {
                                const int sx = axis == 0 ? std::clamp(x + k, 0, w - 1) : x;
                                const int sy = axis == 1 ? std::clamp(y + k, 0, h - 1) : y;
                                sum += colour[static_cast<sizet>(sy) * static_cast<sizet>(w) + static_cast<sizet>(sx)];
                            }
                            blurred[static_cast<sizet>(y) * static_cast<sizet>(w) + static_cast<sizet>(x)] = sum / 7.0f;
                        }
                    }
                    colour = std::move(blurred);
                }
            }
            // Low-frequency value noise, +-8%, on a 48-texel lattice.
            const auto lattice = [](int x, int y)
            { return Hash01(static_cast<u32>(x) * 7919u + 17u, static_cast<u32>(y) * 104729u + 3u); };
            std::vector<u8> out(colour.size() * 4u);
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    const f32 fx = static_cast<f32>(x) / 48.0f;
                    const f32 fy = static_cast<f32>(y) / 48.0f;
                    const int ix = static_cast<int>(fx);
                    const int iy = static_cast<int>(fy);
                    const f32 tx = glm::smoothstep(0.0f, 1.0f, fx - static_cast<f32>(ix));
                    const f32 ty = glm::smoothstep(0.0f, 1.0f, fy - static_cast<f32>(iy));
                    const f32 n = glm::mix(glm::mix(lattice(ix, iy), lattice(ix + 1, iy), tx),
                                           glm::mix(lattice(ix, iy + 1), lattice(ix + 1, iy + 1), tx), ty);
                    const sizet i = static_cast<sizet>(y) * static_cast<sizet>(w) + static_cast<sizet>(x);
                    const glm::vec3 c = glm::clamp(colour[i] * (0.92f + 0.16f * n), glm::vec3(0.0f), glm::vec3(255.0f));
                    out[i * 4u + 0u] = static_cast<u8>(c.r + 0.5f);
                    out[i * 4u + 1u] = static_cast<u8>(c.g + 0.5f);
                    out[i * 4u + 2u] = static_cast<u8>(c.b + 0.5f);
                    out[i * 4u + 3u] = 255u;
                }
            }
            return out;
        }

        // ── The Fox's regions ─────────────────────────────────────────────
        enum class FoxRegion : u8
        {
            Body,
            Belly,
            Neck,
            Head,
            Ear,
            Leg,
            LowerLeg,
            Paw,
            Tail,
            Count
        };

        [[nodiscard]] const char* RegionName(FoxRegion r)
        {
            switch (r)
            {
                case FoxRegion::Body:
                    return "body";
                case FoxRegion::Belly:
                    return "belly";
                case FoxRegion::Neck:
                    return "neck";
                case FoxRegion::Head:
                    return "head";
                case FoxRegion::Ear:
                    return "ear";
                case FoxRegion::Leg:
                    return "leg";
                case FoxRegion::LowerLeg:
                    return "lowerleg";
                case FoxRegion::Paw:
                    return "paw";
                case FoxRegion::Tail:
                    return "tail";
                case FoxRegion::Count:
                    break;
            }
            return "body";
        }

        // The Fox rig's bone names (Fox.gltf's skin joints).
        [[nodiscard]] FoxRegion RegionFromBone(const std::string& bone)
        {
            const auto has = [&](const char* s)
            { return bone.find(s) != std::string::npos; };
            if (has("Tail"))
            {
                return FoxRegion::Tail;
            }
            if (has("Head"))
            {
                return FoxRegion::Head;
            }
            if (has("Neck"))
            {
                return FoxRegion::Neck;
            }
            if (has("Hand") || has("Foot"))
            {
                return FoxRegion::Paw;
            }
            if (has("ForeArm") || has("Leg02"))
            {
                return FoxRegion::LowerLeg;
            }
            if (has("UpperArm") || has("Leg01"))
            {
                return FoxRegion::Leg;
            }
            return FoxRegion::Body;
        }

        [[nodiscard]] bool RegionDebugEnabled()
        {
            const char* v = std::getenv("OLO_FOX_REGION_DEBUG");
            return v != nullptr && v[0] == '1';
        }

        [[nodiscard]] glm::vec3 RegionDebugTint(FoxRegion r)
        {
            switch (r)
            {
                case FoxRegion::Body:
                    return { 0.9f, 0.9f, 0.9f };
                case FoxRegion::Belly:
                    return { 1.0f, 0.5f, 0.8f };
                case FoxRegion::Neck:
                    return { 0.2f, 0.4f, 1.0f };
                case FoxRegion::Head:
                    return { 0.1f, 1.0f, 0.1f };
                case FoxRegion::Ear:
                    return { 1.0f, 1.0f, 0.1f };
                case FoxRegion::Leg:
                    return { 1.0f, 0.5f, 0.0f };
                case FoxRegion::LowerLeg:
                    return { 0.6f, 0.1f, 1.0f };
                case FoxRegion::Paw:
                    return { 1.0f, 0.1f, 0.1f };
                case FoxRegion::Tail:
                    return { 0.0f, 1.0f, 1.0f };
                default:
                    return { 1.0f, 0.2f, 0.8f };
            }
        }

        // One triangle of the Fox, in GROOM space (metres).
        struct SurfaceTriangle
        {
            std::array<glm::vec3, 3> P;
            std::array<glm::vec3, 3> N;      // welded, area-weighted vertex normals
            std::array<glm::vec3, 3> Comb{}; // the lie of the hair at each corner (SmoothCombs)
            std::array<glm::vec2, 3> UV;
            glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
            f32 Area = 0.0f;
            FoxRegion Where = FoxRegion::Body;
        };

        struct FoxFrame
        {
            glm::vec3 Min{ 1.0e9f };
            glm::vec3 Max{ -1.0e9f };
            glm::vec3 HeadCentre{ 0.0f };
            glm::vec3 TailAxis{ 0.0f, 0.0f, -1.0f }; // from the tail's root toward its tip
            f32 Area = 0.0f;
            std::array<f32, static_cast<sizet>(FoxRegion::Count)> RegionArea{};
        };

        [[nodiscard]] std::string DominantBone(const MeshSource& surface, const Skeleton& skeleton, u32 vertex)
        {
            const auto& influences = surface.GetBoneInfluences();
            if (static_cast<i32>(vertex) >= influences.Num())
            {
                return {};
            }
            const BoneInfluence& inf = influences[static_cast<i32>(vertex)];
            u32 best = 0;
            for (u32 k = 1; k < 4; ++k)
            {
                if (inf.m_Weights[k] > inf.m_Weights[best])
                {
                    best = k;
                }
            }
            const u32 id = inf.m_BoneIDs[best];
            return id < skeleton.m_BoneNames.size() ? skeleton.m_BoneNames[id] : std::string{};
        }

        // The triangle's dominant bone by SUMMED weight over its corners: a
        // 576-triangle body has few triangles per bone, and one corner's
        // vote mislabels a whole facet.
        [[nodiscard]] std::string TriangleBone(const MeshSource& surface, const Skeleton& skeleton, const u32* corners)
        {
            std::map<std::string, f32> votes;
            const auto& influences = surface.GetBoneInfluences();
            for (u32 c = 0; c < 3; ++c)
            {
                if (static_cast<i32>(corners[c]) >= influences.Num())
                {
                    continue;
                }
                const BoneInfluence& inf = influences[static_cast<i32>(corners[c])];
                for (u32 k = 0; k < 4; ++k)
                {
                    if (inf.m_Weights[k] > 0.0f && inf.m_BoneIDs[k] < skeleton.m_BoneNames.size())
                    {
                        votes[skeleton.m_BoneNames[inf.m_BoneIDs[k]]] += inf.m_Weights[k];
                    }
                }
            }
            std::string best;
            f32 bestWeight = -1.0f;
            for (const auto& [bone, w] : votes)
            {
                if (w > bestWeight)
                {
                    best = bone;
                    bestWeight = w;
                }
            }
            return best.empty() ? DominantBone(surface, skeleton, corners[0]) : best;
        }

        [[nodiscard]] std::vector<SurfaceTriangle> CollectFoxSurface(const MeshSource& surface, const Skeleton& skeleton,
                                                                     const glm::mat4& surfaceToGroom, FoxFrame& frame)
        {
            const auto& vertices = surface.GetVertices();
            const auto& indices = surface.GetIndices();
            const u32 triangles = static_cast<u32>(indices.Num() / 3);

            // Welded vertex normals: the Fox is flat-shaded, so its corners
            // are split per face. Strands rooted on a facet would all leave it
            // in one direction, and the coat would show the facets.
            const auto key = [](const glm::vec3& p)
            {
                const auto q = [](f32 v)
                { return static_cast<i64>(std::llround(static_cast<f64>(v) * 100.0)); };
                return std::array<i64, 3>{ q(p.x), q(p.y), q(p.z) };
            };
            std::map<std::array<i64, 3>, glm::vec3> welded;
            for (u32 t = 0; t < triangles; ++t)
            {
                const glm::vec3 a = vertices[static_cast<i32>(indices[static_cast<i32>(t * 3u)])].Position;
                const glm::vec3 b = vertices[static_cast<i32>(indices[static_cast<i32>(t * 3u + 1u)])].Position;
                const glm::vec3 c = vertices[static_cast<i32>(indices[static_cast<i32>(t * 3u + 2u)])].Position;
                const glm::vec3 n = glm::cross(b - a, c - a); // area-weighted
                for (const glm::vec3& p : { a, b, c })
                {
                    welded[key(p)] += n;
                }
            }

            std::vector<SurfaceTriangle> out;
            out.reserve(triangles);
            glm::vec3 headSum(0.0f);
            f32 headArea = 0.0f;
            glm::vec3 tailRoot(0.0f), tailTip(0.0f);
            f32 tailRootW = 0.0f, tailTipW = 0.0f;
            std::vector<std::string> bones;
            bones.reserve(triangles);
            const glm::mat3 normalToGroom = glm::transpose(glm::inverse(glm::mat3(surfaceToGroom)));
            for (u32 t = 0; t < triangles; ++t)
            {
                SurfaceTriangle tri;
                std::array<u32, 3> corners{};
                for (u32 c = 0; c < 3; ++c)
                {
                    corners[c] = indices[static_cast<i32>(t * 3u + c)];
                    const Vertex& v = vertices[static_cast<i32>(corners[c])];
                    tri.P[c] = glm::vec3(surfaceToGroom * glm::vec4(v.Position, 1.0f));
                    tri.UV[c] = v.TexCoord;
                    tri.N[c] = glm::normalize(normalToGroom * welded[key(v.Position)]);
                    frame.Min = glm::min(frame.Min, tri.P[c]);
                    frame.Max = glm::max(frame.Max, tri.P[c]);
                }
                glm::vec3 n = glm::cross(tri.P[1] - tri.P[0], tri.P[2] - tri.P[0]);
                tri.Area = 0.5f * glm::length(n);
                if (tri.Area <= 1.0e-10f)
                {
                    continue;
                }
                tri.Normal = glm::normalize(n);
                const std::string bone = TriangleBone(surface, skeleton, corners.data());
                tri.Where = RegionFromBone(bone);
                const glm::vec3 centroid = (tri.P[0] + tri.P[1] + tri.P[2]) / 3.0f;
                if (tri.Where == FoxRegion::Head)
                {
                    headSum += centroid * tri.Area;
                    headArea += tri.Area;
                }
                if (bone.find("Tail01") != std::string::npos)
                {
                    tailRoot += centroid * tri.Area;
                    tailRootW += tri.Area;
                }
                if (bone.find("Tail03") != std::string::npos)
                {
                    tailTip += centroid * tri.Area;
                    tailTipW += tri.Area;
                }
                bones.push_back(bone);
                out.push_back(std::move(tri));
            }
            if (headArea > 0.0f)
            {
                frame.HeadCentre = headSum / headArea;
            }
            if (tailRootW > 0.0f && tailTipW > 0.0f)
            {
                frame.TailAxis = glm::normalize((tailTip / tailTipW) - (tailRoot / tailRootW));
            }

            // Refinements no bone names.
            //  * The TAIL's first bone reaches forward over the rump. A fox's
            //    brush starts behind the hips; the rump is body coat.
            //  * The EARS stand above the skull: the head's triangles in the
            //    top quarter of its height.
            //  * The BELLY and the underside of the neck face down.
            f32 hipRear = 1.0e9f;
            f32 headLo = 1.0e9f, headHi = -1.0e9f;
            for (sizet i = 0; i < out.size(); ++i)
            {
                const glm::vec3 centroid = (out[i].P[0] + out[i].P[1] + out[i].P[2]) / 3.0f;
                if (out[i].Where == FoxRegion::Body || out[i].Where == FoxRegion::Leg)
                {
                    hipRear = std::min(hipRear, centroid.z);
                }
                if (out[i].Where == FoxRegion::Head)
                {
                    for (const auto& p : out[i].P)
                    {
                        headLo = std::min(headLo, p.y);
                        headHi = std::max(headHi, p.y);
                    }
                }
            }
            for (sizet i = 0; i < out.size(); ++i)
            {
                SurfaceTriangle& tri = out[i];
                const glm::vec3 centroid = (tri.P[0] + tri.P[1] + tri.P[2]) / 3.0f;
                if (tri.Where == FoxRegion::Tail && bones[i].find("Tail01") != std::string::npos && centroid.z > hipRear)
                {
                    tri.Where = FoxRegion::Body;
                }
                if (tri.Where == FoxRegion::Head && centroid.y > headLo + 0.72f * (headHi - headLo))
                {
                    tri.Where = FoxRegion::Ear;
                }
                if ((tri.Where == FoxRegion::Body || tri.Where == FoxRegion::Neck) && tri.Normal.y < -0.55f)
                {
                    tri.Where = FoxRegion::Belly;
                }
                frame.Area += tri.Area;
                frame.RegionArea[static_cast<sizet>(tri.Where)] += tri.Area;
            }
            return out;
        }

        // ── Coat recipes ──────────────────────────────────────────────────
        struct FoxLayer
        {
            FoxRegion Where = FoxRegion::Body;
            GroomCoatRole Role = GroomCoatRole::Undercoat;
            f32 StrandsPerM2 = 10000.0f;
            f32 Length = 0.02f;         // metres
            f32 RootDiameter = 1.0e-3f; // metres: a strand is a lock, see below
            f32 Lift = 0.4f;            // how far off the skin the strand leaves
            f32 Droop = 0.04f;          // per-segment gravity bend
            u32 Points = 6;
            f32 Clump = 0.0f;
            f32 Wander = 0.25f;
            glm::vec3 TipTint{ 1.0f };
        };

        struct FoxRecipe
        {
            const char* Name = "FoxCoat";
            std::vector<FoxLayer> Layers;
            u32 GuideEvery = 12;
            u32 Seed = 1;
        };

        // OLO_FOX_TUNE="density,length,width" scales every layer, for tuning
        // the look without a rebuild. Unset, all three are 1: the recipes.
        struct CoatTune
        {
            f32 Density = 1.0f;
            f32 Length = 1.0f;
            f32 Width = 1.0f;
        };
        [[nodiscard]] const CoatTune& Tune()
        {
            static const CoatTune tune = []()
            {
                CoatTune t;
                if (const char* v = std::getenv("OLO_FOX_TUNE"); v != nullptr)
                {
                    (void)std::sscanf(v, "%f,%f,%f", &t.Density, &t.Length, &t.Width);
                }
                return t;
            }();
            return tune;
        }

        [[nodiscard]] const char* RoleSuffix(GroomCoatRole role)
        {
            switch (role)
            {
                case GroomCoatRole::Undercoat:
                    return "undercoat";
                case GroomCoatRole::GuardHair:
                    return "guard";
                case GroomCoatRole::LongHair:
                    return "longhair";
                default:
                    return "coat";
            }
        }

        // WIDTHS CARRY COVERAGE, as on the horses: a real fox has hundreds of
        // hairs per square centimetre, these coats a few, so each strand is a
        // lock about a millimetre across. What hides the skin is density x
        // length x width summed over the layers (about 2.5-3.5 on the body
        // here, so under 10% of the skin is uncovered before the lift and the
        // second layer overlap it).

        // A red fox in its summer coat: short and sleek, a full brush.
        [[nodiscard]] FoxRecipe ShortCoatRecipe()
        {
            using R = FoxRegion;
            using Role = GroomCoatRole;
            const glm::vec3 one(1.0f);
            const glm::vec3 tipped(0.82f, 0.76f, 0.72f); // guard hair darkens toward the tip
            FoxRecipe r;
            r.Name = "FoxShortCoat";
            r.Seed = 1259;
            r.Layers = {
                { R::Body, Role::Undercoat, 80000.0f, 0.022f, 1.7e-3f, 0.45f, 0.03f, 5, 0.0f, 0.25f, one },
                { R::Body, Role::GuardHair, 24000.0f, 0.038f, 1.9e-3f, 0.30f, 0.05f, 7, 0.25f, 0.20f, tipped },
                { R::Belly, Role::Undercoat, 80000.0f, 0.022f, 1.7e-3f, 0.45f, 0.03f, 5, 0.0f, 0.30f, one },
                { R::Belly, Role::GuardHair, 18000.0f, 0.034f, 1.8e-3f, 0.35f, 0.06f, 6, 0.25f, 0.25f, one },
                { R::Neck, Role::Undercoat, 80000.0f, 0.026f, 1.7e-3f, 0.50f, 0.03f, 5, 0.0f, 0.25f, one },
                { R::Neck, Role::GuardHair, 24000.0f, 0.045f, 1.9e-3f, 0.35f, 0.06f, 7, 0.3f, 0.20f, tipped },
                { R::Head, Role::Undercoat, 120000.0f, 0.014f, 1.6e-3f, 0.40f, 0.01f, 4, 0.0f, 0.20f, one },
                { R::Head, Role::GuardHair, 30000.0f, 0.020f, 1.6e-3f, 0.30f, 0.02f, 5, 0.1f, 0.20f, one },
                { R::Ear, Role::Undercoat, 120000.0f, 0.016f, 1.3e-3f, 0.60f, 0.0f, 4, 0.0f, 0.25f, one },
                { R::Leg, Role::Undercoat, 100000.0f, 0.020f, 1.8e-3f, 0.60f, 0.02f, 5, 0.0f, 0.25f, one },
                { R::Leg, Role::GuardHair, 22000.0f, 0.030f, 1.8e-3f, 0.45f, 0.04f, 5, 0.2f, 0.25f, one },
                { R::LowerLeg, Role::Undercoat, 120000.0f, 0.018f, 1.7e-3f, 0.60f, 0.02f, 4, 0.0f, 0.25f, one },
                { R::Paw, Role::Undercoat, 120000.0f, 0.013f, 1.7e-3f, 0.55f, 0.02f, 4, 0.0f, 0.25f, one },
                { R::Tail, Role::Undercoat, 60000.0f, 0.045f, 1.6e-3f, 0.80f, 0.03f, 6, 0.0f, 0.30f, one },
                { R::Tail, Role::LongHair, 45000.0f, 0.085f, 1.7e-3f, 0.95f, 0.05f, 11, 0.4f, 0.25f, tipped },
            };
            return r;
        }

        // The same fox in its winter coat: a dense undercoat under long guard
        // hair, a heavy ruff at the neck and chest, and a big brush.
        [[nodiscard]] FoxRecipe LongCoatRecipe()
        {
            using R = FoxRegion;
            using Role = GroomCoatRole;
            const glm::vec3 one(1.0f);
            const glm::vec3 tipped(0.78f, 0.72f, 0.68f);
            FoxRecipe r;
            r.Name = "FoxLongCoat";
            r.Seed = 2591;
            r.Layers = {
                { R::Body, Role::Undercoat, 55000.0f, 0.040f, 1.8e-3f, 0.55f, 0.04f, 6, 0.0f, 0.30f, one },
                { R::Body, Role::GuardHair, 18000.0f, 0.070f, 2.0e-3f, 0.40f, 0.08f, 10, 0.35f, 0.25f, tipped },
                { R::Belly, Role::Undercoat, 55000.0f, 0.038f, 1.8e-3f, 0.55f, 0.05f, 6, 0.0f, 0.35f, one },
                { R::Belly, Role::GuardHair, 14000.0f, 0.065f, 1.9e-3f, 0.40f, 0.10f, 9, 0.35f, 0.30f, one },
                { R::Neck, Role::Undercoat, 55000.0f, 0.050f, 1.8e-3f, 0.60f, 0.05f, 7, 0.0f, 0.30f, one },
                { R::Neck, Role::GuardHair, 22000.0f, 0.090f, 2.0e-3f, 0.50f, 0.10f, 12, 0.4f, 0.25f, tipped },
                { R::Head, Role::Undercoat, 120000.0f, 0.016f, 1.6e-3f, 0.45f, 0.01f, 4, 0.0f, 0.20f, one },
                { R::Head, Role::GuardHair, 30000.0f, 0.026f, 1.7e-3f, 0.35f, 0.03f, 5, 0.15f, 0.20f, one },
                { R::Ear, Role::Undercoat, 120000.0f, 0.018f, 1.3e-3f, 0.60f, 0.0f, 4, 0.0f, 0.25f, one },
                { R::Leg, Role::Undercoat, 90000.0f, 0.026f, 1.8e-3f, 0.60f, 0.03f, 5, 0.0f, 0.25f, one },
                { R::Leg, Role::GuardHair, 20000.0f, 0.042f, 1.9e-3f, 0.45f, 0.06f, 7, 0.25f, 0.25f, one },
                { R::LowerLeg, Role::Undercoat, 120000.0f, 0.020f, 1.8e-3f, 0.60f, 0.02f, 5, 0.0f, 0.25f, one },
                { R::Paw, Role::Undercoat, 120000.0f, 0.015f, 1.8e-3f, 0.55f, 0.02f, 4, 0.0f, 0.25f, one },
                { R::Tail, Role::Undercoat, 55000.0f, 0.065f, 1.8e-3f, 0.85f, 0.04f, 8, 0.0f, 0.35f, one },
                { R::Tail, Role::LongHair, 50000.0f, 0.125f, 1.9e-3f, 1.00f, 0.06f, 14, 0.45f, 0.30f, tipped },
            };
            return r;
        }

        // The way each region's hair lies, in groom space (the Fox faces +Z).
        [[nodiscard]] glm::vec3 CombDirection(FoxRegion region, const FoxFrame& frame)
        {
            const glm::vec3 back(0.0f, 0.0f, -1.0f);
            const glm::vec3 down(0.0f, -1.0f, 0.0f);
            const glm::vec3 up(0.0f, 1.0f, 0.0f);
            switch (region)
            {
                case FoxRegion::Tail:
                    return frame.TailAxis;
                case FoxRegion::Ear:
                    return up;
                case FoxRegion::Head:
                    return glm::normalize(back + down * 0.15f);
                case FoxRegion::Neck:
                    return glm::normalize(back * 0.6f + down * 0.8f);
                case FoxRegion::Belly:
                    return glm::normalize(back + down * 0.25f);
                case FoxRegion::Leg:
                case FoxRegion::LowerLeg:
                case FoxRegion::Paw:
                    return glm::normalize(down + back * 0.15f);
                default:
                    return glm::normalize(back + down * 0.3f);
            }
        }

        // The lie of the hair at each corner: the area-weighted mean of the
        // regions' comb directions over every triangle sharing the corner.
        // With one direction per region, the hair turned at a region's edge
        // in one step, and each step read as a lit facet of the body.
        void SmoothCombs(std::vector<SurfaceTriangle>& tris, const FoxFrame& frame)
        {
            const auto key = [](const glm::vec3& p)
            {
                const auto q = [](f32 v)
                { return static_cast<i64>(std::llround(static_cast<f64>(v) * 100000.0)); };
                return std::array<i64, 3>{ q(p.x), q(p.y), q(p.z) };
            };
            std::map<std::array<i64, 3>, glm::vec3> combAt;
            for (const SurfaceTriangle& tri : tris)
            {
                for (const glm::vec3& p : tri.P)
                {
                    combAt[key(p)] += CombDirection(tri.Where, frame) * tri.Area;
                }
            }
            for (SurfaceTriangle& tri : tris)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    const glm::vec3 sum = combAt[key(tri.P[c])];
                    tri.Comb[c] = glm::dot(sum, sum) > 1.0e-12f ? glm::normalize(sum) : CombDirection(tri.Where, frame);
                }
            }
        }

        struct GrownCoat
        {
            Ref<GroomAsset> Groom;
            u32 Strands = 0;
            u32 Guides = 0;
            u32 Points = 0;
            u32 SkippedBare = 0;
            std::array<u32, static_cast<sizet>(FoxRegion::Count)> PerRegion{};
        };

        [[nodiscard]] GrownCoat GrowFoxCoat(const std::vector<SurfaceTriangle>& surface,
                                            const FoxTexture& texture, const FoxRecipe& recipe)
        {
            GrownCoat out;
            GroomBuilder builder;
            std::string reason;
            std::vector<u16> groupIds(recipe.Layers.size(), 0);
            for (sizet l = 0; l < recipe.Layers.size(); ++l)
            {
                const FoxLayer& layer = recipe.Layers[l];
                const std::string name = std::string(RegionName(layer.Where)) + "_" + RoleSuffix(layer.Role);
                EXPECT_TRUE(builder.AddGroup(name, groupIds[l], reason)) << reason;
                GroomCoatGroupDesc desc;
                desc.Role = static_cast<u8>(layer.Role);
                desc.Tint = RegionDebugEnabled() ? RegionDebugTint(layer.Where) : glm::vec3(1.0f);
                desc.TipTint = RegionDebugEnabled() ? glm::vec3(1.0f) : layer.TipTint;
                desc.Clump = layer.Clump;
                std::vector<std::string> reasons;
                EXPECT_TRUE(builder.SetGroupCoat(groupIds[l], desc, reasons))
                    << (reasons.empty() ? std::string{} : reasons.front());
            }

            std::vector<glm::vec3> points;
            std::vector<f32> widths;
            const glm::vec3 up(0.0f, 1.0f, 0.0f);
            for (sizet l = 0; l < recipe.Layers.size(); ++l)
            {
                const FoxLayer& layer = recipe.Layers[l];
                u32 inLayer = 0;
                for (u32 t = 0; t < static_cast<u32>(surface.size()); ++t)
                {
                    const SurfaceTriangle& tri = surface[t];
                    if (tri.Where != layer.Where)
                    {
                        continue;
                    }
                    const f32 expected = tri.Area * layer.StrandsPerM2 * Tune().Density;
                    u32 count = static_cast<u32>(expected);
                    if (Hash01(t, recipe.Seed * 131u + static_cast<u32>(l)) < (expected - static_cast<f32>(count)))
                    {
                        ++count;
                    }
                    for (u32 s = 0; s < count; ++s)
                    {
                        const u32 salt = (t * 977u) + (s * 31u) + static_cast<u32>(l) * 7919u + recipe.Seed;
                        f32 a = Hash01(salt, 1u);
                        f32 b = Hash01(salt, 2u);
                        if (a + b > 1.0f)
                        {
                            a = 1.0f - a;
                            b = 1.0f - b;
                        }
                        const f32 c = 1.0f - a - b;
                        const glm::vec3 root = (tri.P[0] * c) + (tri.P[1] * a) + (tri.P[2] * b);
                        const glm::vec2 uv = (tri.UV[0] * c) + (tri.UV[1] * a) + (tri.UV[2] * b);
                        if (IsBareSkin(texture.Sample(uv, texture.FlipV)))
                        {
                            ++out.SkippedBare; // the nose and the eyes
                            continue;
                        }
                        // The smooth normal at the root, so a facet's strands
                        // fan with the body rather than leaving it as one.
                        glm::vec3 normal = glm::normalize((tri.N[0] * c) + (tri.N[1] * a) + (tri.N[2] * b));
                        if (glm::dot(normal, tri.Normal) < 0.6f)
                        {
                            normal = tri.Normal;
                        }
                        const glm::vec3 combBase = (tri.Comb[0] * c) + (tri.Comb[1] * a) + (tri.Comb[2] * b);
                        glm::vec3 comb = combBase - normal * glm::dot(combBase, normal);
                        comb = glm::dot(comb, comb) > 1.0e-6f ? glm::normalize(comb) : glm::vec3(0.0f);

                        const f32 length = layer.Length * Tune().Length * (0.75f + (0.5f * Hash01(salt, 3u)));
                        const glm::vec3 wander =
                            glm::vec3(Hash01(salt, 4u) - 0.5f, Hash01(salt, 5u) - 0.5f, Hash01(salt, 6u) - 0.5f) *
                            layer.Wander;
                        glm::vec3 direction = glm::normalize((normal * layer.Lift) + comb + wander);
                        // The strand must LEAVE the facet it grows on. On a thin
                        // part (an ear), the smoothed normal leans toward the
                        // far side, and a comb projected on it pointed the inner
                        // ear's hair into the ear, behind its own skin.
                        if (const f32 out = glm::dot(direction, tri.Normal); out < 0.35f)
                        {
                            direction = glm::normalize(direction + tri.Normal * (0.35f - out) * 1.5f);
                        }

                        points.clear();
                        widths.clear();
                        glm::vec3 p = root;
                        const f32 step = length / static_cast<f32>(layer.Points - 1u);
                        for (u32 k = 0; k < layer.Points; ++k)
                        {
                            const f32 along = static_cast<f32>(k) / static_cast<f32>(layer.Points - 1u);
                            points.push_back(p);
                            // A hair tapers to a fine tip, not to a blunt end.
                            widths.push_back(layer.RootDiameter * Tune().Width * (1.0f - (0.7f * along * along)));
                            direction = glm::normalize(direction + (comb * 0.18f) - (up * layer.Droop));
                            p += direction * step;
                        }

                        GroomCurveInput input;
                        input.Points = points;
                        input.Widths = widths;
                        input.RootUV = uv;
                        input.GroupId = groupIds[l];
                        input.IsGuide = (inLayer % recipe.GuideEvery) == 0u;
                        if (!builder.AddCurve(input, reason))
                        {
                            ADD_FAILURE() << reason;
                            return out;
                        }
                        out.Guides += input.IsGuide ? 1u : 0u;
                        out.Points += layer.Points;
                        ++out.PerRegion[static_cast<sizet>(layer.Where)];
                        ++inLayer;
                        ++out.Strands;
                    }
                }
            }

            builder.SetName(recipe.Name);
            out.Groom = builder.Build(reason);
            EXPECT_TRUE(out.Groom) << reason;
            if (out.Groom)
            {
                EXPECT_TRUE(GroomCooker::Canonicalize(*out.Groom, reason)) << reason;
            }
            return out;
        }

        [[nodiscard]] GroomSurfaceView MakeSurfaceView(const MeshSource& surface, const Skeleton* skeleton)
        {
            GroomSurfaceView view;
            const auto& vertices = surface.GetVertices();
            const auto& indices = surface.GetIndices();
            view.PositionData = reinterpret_cast<const std::byte*>(vertices.GetData());
            view.PositionStride = static_cast<u32>(sizeof(Vertex));
            view.VertexCount = static_cast<u32>(vertices.Num());
            view.Indices = indices.GetData();
            view.IndexCount = static_cast<u32>(indices.Num());
            view.BoneCount = skeleton != nullptr ? static_cast<u32>(skeleton->m_FinalBoneMatrices.size()) : 0u;
            view.SkeletonNameHash = skeleton != nullptr ? HashGroomSkeletonNames(skeleton->m_BoneNames) : 0u;
            return view;
        }

        // Closest point on triangle abc to p (Ericson, Real-Time Collision
        // Detection 5.1.5).
        [[nodiscard]] glm::vec3 ClosestOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b,
                                                  const glm::vec3& c)
        {
            const glm::vec3 ab = b - a;
            const glm::vec3 ac = c - a;
            const glm::vec3 ap = p - a;
            const f32 d1 = glm::dot(ab, ap);
            const f32 d2 = glm::dot(ac, ap);
            if (d1 <= 0.0f && d2 <= 0.0f)
                return a;
            const glm::vec3 bp = p - b;
            const f32 d3 = glm::dot(ab, bp);
            const f32 d4 = glm::dot(ac, bp);
            if (d3 >= 0.0f && d4 <= d3)
                return b;
            const f32 vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
                return a + ab * (d1 / (d1 - d3));
            const glm::vec3 cp = p - c;
            const f32 d5 = glm::dot(ab, cp);
            const f32 d6 = glm::dot(ac, cp);
            if (d6 >= 0.0f && d5 <= d6)
                return c;
            const f32 vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
                return a + ac * (d2 / (d2 - d6));
            const f32 va = d3 * d6 - d5 * d4;
            if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
                return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
            const f32 denom = 1.0f / (va + vb + vc);
            return a + ab * (vb * denom) + ac * (vc * denom);
        }
    } // namespace

    class FoxCoatEvidenceTest : public RendererAttachedTest
    {
      public:
        struct FoxRig
        {
            std::string Name; // FoxShortCoat / FoxLongCoat
            std::string Tag;  // ShortCoat / LongCoat
            Entity Body;
            Entity Coat;
            Ref<AnimatedModel> Model;
            GrownCoat Grown;
            Ref<GroomBindingAsset> Binding;
            AssetHandle GroomHandle = 0;
            AssetHandle BindingHandle = 0;
            FoxFrame Frame; // in groom space
            glm::vec3 Position{ 0.0f };
            f32 Side = 1.0f; // which side the fox is judged from: +x, the sun's side
            f32 ClipStart = 0.0f;
            u32 CardCount = 0;
        };

        FoxRig m_Short;
        FoxRig m_Long;
        FoxTexture m_Texture;
        AssetHandle m_ColorMap = 0;
        fs::path m_ProjectDir;

        static constexpr glm::vec3 kShortPosition{ -1.5f, 0.0f, 0.0f };
        static constexpr glm::vec3 kLongPosition{ 1.5f, 0.0f, 0.0f };

        void BuildScene() override
        {
            if (!Project::GetActive() || !Project::HasAssetManager())
            {
                std::error_code ec;
                m_ProjectDir = TempDir("project");
                fs::create_directories(m_ProjectDir / "Assets", ec);
                ASSERT_FALSE(ec);
                {
                    std::ofstream proj(m_ProjectDir / "Evidence.oloproj");
                    proj << "Project:\n"
                            "  Name: FoxCoatEvidence\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(m_ProjectDir / "Evidence.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false);
                Project::SetAssetManager(assetManager);
            }
            else
            {
                m_ProjectDir = Project::GetProjectDirectory();
            }

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();
            auto& rs = Renderer3D::GetRendererSettings();
            rs.EditorDebugDrawsEnabled = false;
            rs.ShowComponentGizmos = false;
            rs.ShowGrid = false;
            rs.ShowWorldAxisHelper = false;
            rs.AnimalSchedulingEnabled = false;
            auto& post = Renderer3D::GetPostProcessSettings();
            post.TAAEnabled = true;
            post.TAAFeedback = 0.9f;
            post.TAASharpness = 0.0f;

            {
                // AnimalPopulation.olo's key light, so the evidence is graded
                // under the light the herd is seen in.
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.5f, -0.55f, -0.67f));
                dl.m_Color = glm::vec3(1.0f, 0.95f, 0.86f);
                dl.m_Intensity = 5.0f;
            }
            {
                // A primary runtime camera makes RunFrames render, and so step
                // the clips and the guide solver. Close: the coats on strands.
                Entity cameraEntity = scene.CreateEntity("RuntimeCamera");
                auto& t = cameraEntity.GetComponent<TransformComponent>();
                t.Translation = glm::vec3(0.0f, 1.2f, 4.5f);
                t.SetRotationEuler(glm::vec3(-0.15f, 0.0f, 0.0f));
                auto& cc = cameraEntity.AddComponent<CameraComponent>();
                cc.Primary = true;
                cc.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
                cc.Camera.SetPerspective(glm::radians(45.0f), 0.05f, 400.0f);
                cc.Camera.SetViewportSize(kWidth, kHeight);
            }
            {
                Entity ground = scene.CreateEntity("Ground");
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, -0.02f, 0.0f);
                tc.Scale = glm::vec3(60.0f, 0.04f, 60.0f);
                ground.AddComponent<MeshComponent>(MeshPrimitives::CreateCube()->GetMeshSource()).m_Primitive =
                    MeshPrimitive::Cube;
                ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(0.24f, 0.26f, 0.18f, 1.0f));
            }

            m_Texture = LoadFoxTexture();
            ASSERT_GT(m_Texture.Width, 0);

            // The coat's colour map, as the export writes it and the editor
            // loads it: a PNG in the project, imported through the project's
            // EditorAssetManager and loaded by its texture serializer.
            const std::vector<u8> colorMap = BuildFoxCoatColorMap(m_Texture);
            const fs::path colorPath = Project::GetAssetDirectory() / "FoxCoatColor.png";
            ASSERT_NE(::stbi_write_png(colorPath.string().c_str(), m_Texture.Width, m_Texture.Height, 4, colorMap.data(),
                                       m_Texture.Width * 4),
                      0);
            auto editorAssets = Project::GetAssetManager().As<EditorAssetManager>();
            ASSERT_TRUE(editorAssets);
            m_ColorMap = editorAssets->ImportAsset(colorPath);
            ASSERT_NE(static_cast<u64>(m_ColorMap), 0u);
            ASSERT_TRUE(AssetManager::GetAsset<Texture2D>(m_ColorMap));

            InstallFox(m_Short, "FoxShortCoat", "ShortCoat", kShortPosition, 1.0f, 0.0f, ShortCoatRecipe());
            ASSERT_FALSE(HasFatalFailure());
            InstallFox(m_Long, "FoxLongCoat", "LongCoat", kLongPosition, 1.0f, 0.45f, LongCoatRecipe());
            ASSERT_FALSE(HasFatalFailure());
        }

        void InstallFox(FoxRig& out, const std::string& name, const std::string& tag, const glm::vec3& position, f32 side,
                        f32 clipTime, const FoxRecipe& recipe)
        {
            Scene& scene = GetScene();
            out.Name = name;
            out.Tag = tag;
            out.Position = position;
            out.Side = side;
            out.ClipStart = clipTime;
            out.Model = Ref<AnimatedModel>::Create(FoxPath().string());
            ASSERT_TRUE(out.Model);
            ASSERT_TRUE(out.Model->HasSkeleton());

            out.Body = scene.CreateEntity(name + "Body");
            auto& bt = out.Body.GetComponent<TransformComponent>();
            bt.Translation = position;
            bt.Scale = glm::vec3(kFoxCookScale);
            (void)ModelImporter::PopulateAnimatedEntity(out.Body, out.Model, FoxPath().string(), true);
            ASSERT_TRUE(out.Body.HasComponent<MeshComponent>());
            ASSERT_TRUE(out.Body.HasComponent<SkeletonComponent>());
            ASSERT_TRUE(out.Body.HasComponent<AnimationStateComponent>());
            {
                auto& anim = out.Body.GetComponent<AnimationStateComponent>();
                bool found = false;
                for (sizet i = 0; i < anim.m_AvailableClips.size(); ++i)
                {
                    if (anim.m_AvailableClips[i]->Name == "Walk")
                    {
                        anim.m_CurrentClipIndex = static_cast<int>(i);
                        anim.m_CurrentClip = anim.m_AvailableClips[i];
                        found = true;
                    }
                }
                ASSERT_TRUE(found) << "the Fox has a Walk clip";
                anim.m_CurrentTime = clipTime;
                anim.m_IsPlaying = true;
            }

            const Ref<MeshSource> surface = out.Body.GetComponent<MeshComponent>().m_MeshSource;
            ASSERT_TRUE(surface);
            const Skeleton* skeleton = out.Body.GetComponent<SkeletonComponent>().m_Skeleton.Raw();
            ASSERT_NE(skeleton, nullptr);

            // THE GROOM SPACE: the Fox's own space in metres.
            const glm::mat4 surfaceToGroom = glm::scale(glm::mat4(1.0f), glm::vec3(kFoxCookScale));
            std::vector<SurfaceTriangle> tris = CollectFoxSurface(*surface, *skeleton, surfaceToGroom, out.Frame);
            SmoothCombs(tris, out.Frame);
            ASSERT_FALSE(tris.empty());
            ASSERT_GT(out.Frame.HeadCentre.z, 0.0f) << "the Fox faces +Z";

            // Which way the mesh's TexCoord addresses the texture's rows,
            // MEASURED: the right way puts nearly every root inside a UV
            // island, the wrong way puts a third of them on the background.
            {
                u32 onBackground[2] = { 0u, 0u };
                u32 samples = 0;
                for (u32 t = 0; t < static_cast<u32>(tris.size()); ++t)
                {
                    const glm::vec2 uv = (tris[t].UV[0] + tris[t].UV[1] + tris[t].UV[2]) / 3.0f;
                    for (u32 flip = 0; flip < 2; ++flip)
                    {
                        onBackground[flip] += IsBackground(m_Texture.Sample(uv, flip != 0u)) ? 1u : 0u;
                    }
                    ++samples;
                }
                m_Texture.FlipV = onBackground[1] < onBackground[0];
                const f64 wrong = static_cast<f64>(onBackground[m_Texture.FlipV ? 1 : 0]) / static_cast<f64>(samples);
                std::printf("[fox] TexCoord rows %s; triangle centroids on the background: %u (as stored) / %u (flipped) of %u\n",
                            m_Texture.FlipV ? "flipped" : "as stored", onBackground[0], onBackground[1], samples);
                EXPECT_LT(wrong, 0.02) << "the coat's root UVs must land inside the Fox's UV islands";
            }

            out.Grown = GrowFoxCoat(tris, m_Texture, recipe);
            ASSERT_TRUE(out.Grown.Groom);

            // The cooked card level (#1252), through the real builder, cooked
            // for the hand-over the live scene authors.
            {
                GroomCardSettings cardSettings;
                cardSettings.CellSize = 0.010f;
                cardSettings.PointsPerCard = 6;
                cardSettings.SourcePixelSize = kCardPixelSize;
                GroomLodLevel level;
                GroomCardBuildStats cardStats;
                std::string reason;
                ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*out.Grown.Groom, cardSettings, level, reason, &cardStats))
                    << reason;
                out.CardCount = level.GetCurveCount();
                ASSERT_TRUE(GroomLodBuilder::AttachLodLevels(*out.Grown.Groom, { std::move(level) }, reason)) << reason;
            }

            std::printf("[fox] %s: %u strands (%.1f points each), %u guides, %u cards, %u bare roots skipped; area %.3f m2;",
                        name.c_str(), out.Grown.Strands,
                        static_cast<f64>(out.Grown.Points) / static_cast<f64>(std::max(out.Grown.Strands, 1u)),
                        out.Grown.Guides, out.CardCount, out.Grown.SkippedBare, static_cast<f64>(out.Frame.Area));
            for (sizet r = 0; r < out.Grown.PerRegion.size(); ++r)
            {
                std::printf(" %s=%u (%.3f m2)", RegionName(static_cast<FoxRegion>(r)), out.Grown.PerRegion[r],
                            static_cast<f64>(out.Frame.RegionArea[r]));
            }
            std::printf("\n");
            std::fflush(stdout);

            GroomBindingBuildSettings bind;
            bind.SurfaceToGroom = surfaceToGroom;
            std::vector<u8> bytes;
            GroomBindingBuildStats stats;
            std::string reason;
            ASSERT_TRUE(GroomBindingCooker::CookPair(*out.Grown.Groom, MakeSurfaceView(*surface, skeleton), name.c_str(),
                                                     bind, bytes, out.Binding, stats, reason))
                << reason;
            ASSERT_TRUE(GroomBindingSerializer::DecodeFromBytes(bytes.data(), bytes.size(), out.Binding, reason)) << reason;

            out.GroomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(out.Grown.Groom);
            out.BindingHandle = AssetManager::AddMemoryOnlyAsset<GroomBindingAsset>(out.Binding);
            out.Coat = MakeCoat(name, out.GroomHandle, out.BindingHandle, out.Body, out.Grown.Strands);
        }

        // Every groom component, AS generate_animal_population_scene.py
        // AUTHORS THE HERD'S COATS. Keep the two in step: the evidence is only
        // evidence for the live scene while they agree.
        Entity MakeCoat(const std::string& name, AssetHandle groom, AssetHandle binding, Entity body, u32 strands)
        {
            Entity coat = GetScene().CreateEntity(name);
            // Under the fox, at the inverse of the cook scale: the coat's
            // space is the fox's, in metres.
            coat.GetComponent<TransformComponent>().Scale = glm::vec3(1.0f / kFoxCookScale);
            coat.SetParent(body);

            auto& gc = coat.AddComponent<GroomComponent>();
            gc.m_Groom = groom;
            gc.m_ShowPreview = false;
            gc.m_RenderStrands = true;
            gc.m_MaxRenderStrands = strands;
            gc.m_CompositionMode = static_cast<u8>(GroomCompositionMode::StochasticAlpha);
            gc.m_StrandColor = glm::vec3(1.0f);

            // Pale fibres; the coat map carries the fox's colours (the dog's
            // recipe, groom-dual-scattering.md: a pale coat with pale fibres,
            // the hue compounding through the crossings).
            auto& fibre = coat.AddComponent<GroomFibreComponent>();
            fibre.m_PigmentMode = static_cast<u8>(GroomFibrePigmentMode::BaseColor);
            fibre.m_BaseColor = glm::vec3(1.0f, 0.94f, 0.86f);
            fibre.m_Intensity = 1.0f;
            fibre.m_LongitudinalRoughness = 0.4f;
            // OLO_FOX_FIBRE="r,g,b,intensity" for tuning.
            if (const char* tune = std::getenv("OLO_FOX_FIBRE"); tune != nullptr)
            {
                (void)std::sscanf(tune, "%f,%f,%f,%f", &fibre.m_BaseColor.r, &fibre.m_BaseColor.g, &fibre.m_BaseColor.b,
                                  &fibre.m_Intensity);
            }

            auto& coatComp = coat.AddComponent<GroomCoatComponent>();
            coatComp.m_ColorMap = RegionDebugEnabled() ? AssetHandle(0) : m_ColorMap;
            coatComp.m_LengthJitter = 0.2f;
            coatComp.m_ShadeJitter = 0.06f;
            coatComp.m_ClumpCellSize = 0.010f;

            auto& coatShadow = coat.AddComponent<GroomCoatShadowComponent>();
            coatShadow.m_Enabled = true;
            coatShadow.m_Resolution = 128u;
            coatShadow.m_Kappa = 0.9f;
            coatShadow.m_StepVoxels = 1.5f;
            coatShadow.m_MultipleScattering = true;
            coatShadow.m_BakeAtRest = true;

            auto& lod = coat.AddComponent<GroomLodComponent>();
            lod.m_Enabled = true;
            lod.m_CardPixelSize = kCardPixelSize;
            lod.m_MeshPixelSize = 8.0f;

            auto& sceneShadow = coat.AddComponent<GroomSceneShadowComponent>();
            sceneShadow.m_CastShadows = true;
            sceneShadow.m_ReceiveShadows = true;

            auto& bc = coat.AddComponent<GroomBindingComponent>();
            bc.m_Binding = binding;
            bc.m_TargetEntity = body.GetUUID();
            bc.m_Enabled = true;

            auto& sim = coat.AddComponent<GroomSimulationComponent>();
            sim.m_Enabled = true;
            sim.m_Collide = true;
            return coat;
        }

        [[nodiscard]] static const GroomRenderStats& PassStats()
        {
            if (const auto* pass = Renderer3D::GetGroomRenderPass())
            {
                return pass->GetStats();
            }
            static const GroomRenderStats kEmpty{};
            return kEmpty;
        }

        static void SetPath(RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
        }

        static void ColdHistory()
        {
            Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::SceneReset);
        }

        [[nodiscard]] std::array<FoxRig*, 2> Foxes()
        {
            return { &m_Short, &m_Long };
        }

        struct MotionResult
        {
            GroomRenderStats Last;
            f32 WorstStretch = 1.0f;
            u32 MinGroomsDeformed = 0xFFFFFFFFu;
            u32 MaxBindingRefused = 0;
            u32 MaxHeldAtRest = 0;
        };

        MotionResult Play(u32 frames)
        {
            MotionResult r;
            for (u32 f = 0; f < frames; ++f)
            {
                RunFrames(1, 1.0f / 60.0f);
                r.Last = PassStats();
                r.MinGroomsDeformed = std::min(r.MinGroomsDeformed, r.Last.GroomsDeformed);
                r.MaxBindingRefused = std::max(r.MaxBindingRefused, r.Last.GroomsBindingRefused);
                r.MaxHeldAtRest = std::max(r.MaxHeldAtRest, r.Last.RootsHeldAtRest);
                if (std::abs(r.Last.WorstStretchRatio - 1.0f) > std::abs(r.WorstStretch - 1.0f))
                {
                    r.WorstStretch = r.Last.WorstStretchRatio;
                }
            }
            return r;
        }

        // Both clips back to their start, both solvers re-seeded.
        MotionResult PlayFromStart(u32 frames)
        {
            for (FoxRig* f : Foxes())
            {
                auto& anim = f->Body.GetComponent<AnimationStateComponent>();
                anim.m_CurrentTime = f->ClipStart;
                anim.m_IsPlaying = true;
                ++f->Coat.GetComponent<GroomSimulationComponent>().m_ResetKey;
            }
            return Play(frames);
        }

        void Freeze()
        {
            for (FoxRig* f : Foxes())
            {
                f->Body.GetComponent<AnimationStateComponent>().m_IsPlaying = false;
            }
        }

        struct View
        {
            glm::vec3 Eye;
            glm::vec3 Target;
            f32 Fov = 35.0f;
        };

        // World-space point of a groom-space point of this fox's coat.
        [[nodiscard]] glm::vec3 World(FoxRig& f, const glm::vec3& groomPoint)
        {
            return glm::vec3(GetScene().GetWorldTransform(f.Coat) * glm::vec4(groomPoint, 1.0f));
        }

        // The four hero framings: the fox from the front, three-quarter
        // front, its open side, and its head close up.
        [[nodiscard]] std::vector<std::pair<std::string, View>> HeroViews(FoxRig& f)
        {
            const glm::vec3 centre = World(f, (f.Frame.Min + f.Frame.Max) * 0.5f);
            const glm::vec3 head = World(f, f.Frame.HeadCentre);
            const f32 length = (f.Frame.Max.z - f.Frame.Min.z);
            const glm::vec3 body = centre + glm::vec3(0.0f, -0.05f * length, 0.1f * length);
            return {
                { "Front", { body + glm::vec3(0.1f * f.Side, 0.12f, 1.0f) * length, body + glm::vec3(0.0f, 0.06f * length, 0.0f), 35.0f } },
                { "ThreeQuarter", { body + glm::vec3(0.7f * f.Side, 0.25f, 0.75f) * length, body, 35.0f } },
                { "Side", { body + glm::vec3(1.15f * f.Side, 0.1f, 0.0f) * length, body, 35.0f } },
                { "Head", { head + glm::vec3(0.4f * f.Side, 0.0f, 0.7f) * length, head - glm::vec3(0.0f, 0.04f * length, 0.0f), 28.0f } },
            };
        }

        [[nodiscard]] static EditorCamera CameraFor(const View& view)
        {
            const glm::vec3 d = glm::normalize(view.Target - view.Eye);
            EditorCamera camera(view.Fov, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 400.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(view.Eye, std::atan2(d.x, -d.z), std::asin(std::clamp(-d.y, -1.0f, 1.0f)));
            return camera;
        }

        // Converge TAA on a still view, read back the composite, write a PNG.
        void Capture(const std::string& name, const View& view, std::vector<u8>& out, u32 settleFrames = 32)
        {
            RunEditorFrames(CameraFor(view), settleFrames);
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            ASSERT_TRUE(fb);
            ReadbackRgba8(fb->GetColorAttachmentHandle(0), kWidth, kHeight, out);
            ASSERT_EQ(out.size(), static_cast<sizet>(kWidth) * kHeight * 4u);
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = out.data() + (static_cast<sizet>(y) * rowBytes);
                u8* bot = out.data() + (static_cast<sizet>(kHeight - 1u - y) * rowBytes);
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }
            if (!name.empty())
            {
                WritePng(name, out, kWidth, kHeight);
            }
        }

        static void WritePng(const std::string& name, const std::vector<u8>& rgba, u32 width, u32 height)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / (name + ".png")).string();
            ASSERT_NE(::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
                                       static_cast<int>(width) * 4),
                      0)
                << path;
        }

        // Per stochastic frame, which surface is in front at each pixel: the
        // entity-ID target of SceneColor. Averaged over `frames` frames, the
        // fox's own ID is the skin a viewer sees through the coat, and the
        // coat's ID its coverage. Luma is the coat's summed linear luma.
        struct IdCount
        {
            f64 Coat = 0.0;
            f64 Body = 0.0;
            f64 CoatLuma = 0.0;
            u32 Frames = 0;
        };

        IdCount CountIds(const View& view, i32 coatId, i32 bodyId, u32 frames, bool luma = false,
                         std::vector<f32>* skinMap = nullptr)
        {
            const EditorCamera camera = CameraFor(view);
            Renderer3D::ResetFrameSequences();
            ColdHistory();
            RunEditorFrames(camera, 6);
            IdCount out;
            std::vector<i32> ids;
            std::vector<f32> colour;
            for (u32 f = 0; f < frames; ++f)
            {
                RunEditorFrames(camera, 1);
                auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
                EXPECT_TRUE(fb);
                if (!fb)
                {
                    return out;
                }
                const u32 width = fb->GetSpecification().Width;
                const u32 height = fb->GetSpecification().Height;
                ids.resize(static_cast<sizet>(width) * height);
                ::glGetTextureImage(fb->GetColorAttachmentRendererID(1), 0, GL_RED_INTEGER, GL_INT,
                                    static_cast<GLsizei>(ids.size() * sizeof(i32)), ids.data());
                if (luma)
                {
                    colour.resize(static_cast<sizet>(width) * height * 4u);
                    ReadbackRgbaFloat(fb->GetColorAttachmentHandle(0), width, height, colour);
                }
                for (sizet i = 0; i < ids.size(); ++i)
                {
                    if (ids[i] == coatId)
                    {
                        out.Coat += 1.0;
                        if (luma)
                        {
                            out.CoatLuma += (0.2126 * colour[i * 4u]) + (0.7152 * colour[(i * 4u) + 1u]) +
                                            (0.0722 * colour[(i * 4u) + 2u]);
                        }
                    }
                    else if (ids[i] == bodyId)
                    {
                        out.Body += 1.0;
                        if (skinMap != nullptr)
                        {
                            skinMap->resize(ids.size(), 0.0f);
                            (*skinMap)[i] += 1.0f / static_cast<f32>(frames);
                        }
                    }
                }
                ++out.Frames;
            }
            if (out.Frames > 0u)
            {
                out.Coat /= static_cast<f64>(out.Frames);
                out.Body /= static_cast<f64>(out.Frames);
                out.CoatLuma /= static_cast<f64>(out.Frames);
            }
            return out;
        }

        [[nodiscard]] static i32 IdOf(Entity e)
        {
            return static_cast<i32>(static_cast<u32>(e));
        }

        [[nodiscard]] const GroomStrandRequest* RequestFor(const FoxRig& f) const
        {
            for (const auto& req : Renderer3D::GetGroomStrandRequests())
            {
                if (req.EntityID == IdOf(f.Coat))
                {
                    return &req;
                }
            }
            return nullptr;
        }

        // How far the coat's drawn roots are from the fox's skin THIS frame,
        // in groom metres. The skin is skinned here, independently of the
        // binding: linear blend skinning of the bind-pose surface with the
        // skeleton's current palette, then the nearest point over every
        // triangle -- so a root bound to the wrong triangle, at the wrong
        // scale, or held at the bind pose while the fox walks is measured, not
        // assumed. `bindPose` measures against the UNSKINNED surface instead:
        // the negative control that the measure can see a detached coat.
        struct RootDistance
        {
            f64 P50 = 0.0;
            f64 P99 = 0.0;
            f64 Max = 0.0;
            u32 Roots = 0;
            u32 Invalid = 0;
        };

        RootDistance RootsToSkin(FoxRig& f, bool bindPose)
        {
            RootDistance out;
            const GroomStrandRequest* req = RequestFor(f);
            EXPECT_NE(req, nullptr) << f.Name;
            if (req == nullptr || !req->Binding)
            {
                return out;
            }
            TArray<GroomRootTransform> scratch;
            GroomSurfaceSkinScratch skinScratch;
            const std::span<const GroomRootTransform> roots = GroomCpuRootTransforms(*req, std::nullopt, scratch, skinScratch);

            const MeshSource& surface = *f.Body.GetComponent<MeshComponent>().m_MeshSource;
            const Skeleton& skeleton = *f.Body.GetComponent<SkeletonComponent>().m_Skeleton;
            const auto& vertices = surface.GetVertices();
            const auto& indices = surface.GetIndices();
            const auto& influences = surface.GetBoneInfluences();
            const glm::mat4 toGroom =
                glm::inverse(GetScene().GetWorldTransform(f.Coat)) * GetScene().GetWorldTransform(f.Body);
            std::vector<glm::vec3> skinned(static_cast<sizet>(vertices.Num()));
            for (i32 v = 0; v < vertices.Num(); ++v)
            {
                glm::vec4 p(vertices[v].Position, 1.0f);
                if (!bindPose && v < influences.Num())
                {
                    glm::vec4 sum(0.0f);
                    f32 weight = 0.0f;
                    for (u32 k = 0; k < 4; ++k)
                    {
                        const f32 w = influences[v].m_Weights[k];
                        const u32 id = influences[v].m_BoneIDs[k];
                        if (w > 0.0f && id < skeleton.m_FinalBoneMatrices.size())
                        {
                            sum += (skeleton.m_FinalBoneMatrices[id] * p) * w;
                            weight += w;
                        }
                    }
                    if (weight > 0.0f)
                    {
                        p = sum / weight;
                    }
                }
                skinned[static_cast<sizet>(v)] = glm::vec3(toGroom * glm::vec4(glm::vec3(p), 1.0f));
            }
            std::vector<f64> distances;
            for (sizet c = 0; c < roots.size(); c += 5)
            {
                if (!roots[c].Valid)
                {
                    ++out.Invalid;
                    continue;
                }
                f32 best = 1.0e9f;
                for (i32 t = 0; t + 2 < indices.Num(); t += 3)
                {
                    const glm::vec3 q =
                        ClosestOnTriangle(roots[c].Origin, skinned[indices[t]], skinned[indices[t + 1]], skinned[indices[t + 2]]);
                    best = std::min(best, glm::length(q - roots[c].Origin));
                }
                distances.push_back(best);
            }
            out.Roots = static_cast<u32>(distances.size());
            if (distances.empty())
            {
                return out;
            }
            std::sort(distances.begin(), distances.end());
            out.P50 = distances[distances.size() / 2u];
            out.P99 = distances[std::min(distances.size() - 1u, (distances.size() * 99u) / 100u)];
            out.Max = distances.back();
            return out;
        }
    };

    // =========================================================================
    // The coat stays on the fox through the Walk clip: every drawn root on the
    // skinned skin, every frame of a stride, both coats; and the coat's share
    // of a frame framed on the fox steady through the stride.
    // =========================================================================
    TEST_F(FoxCoatEvidenceTest, TheCoatRidesTheWalkingFox)
    {
        SetPath(RenderingPath::Forward);
        (void)PlayFromStart(30);

        constexpr u32 kSteps = 8;
        constexpr u32 kFramesPerStep = 8;
        std::map<std::string, std::vector<f64>> coverage;
        std::vector<std::vector<u8>> strip;
        f64 worstP99 = 0.0;
        f64 worstMax = 0.0;
        f64 controlP99 = 0.0;
        for (u32 step = 0; step < kSteps; ++step)
        {
            const MotionResult motion = Play(kFramesPerStep);
            EXPECT_EQ(motion.MinGroomsDeformed, 2u) << "step " << step << ": both coats follow their fox every frame";
            EXPECT_EQ(motion.MaxBindingRefused, 0u) << "step " << step;
            EXPECT_EQ(motion.MaxHeldAtRest, 0u) << "step " << step << ": no root held at the bind pose";
            EXPECT_LE(std::abs(motion.WorstStretch - 1.0f), std::abs(motion.Last.DeclaredStretchTolerance - 1.0f) + 1.0e-4f)
                << "step " << step;

            for (FoxRig* f : Foxes())
            {
                const GroomStrandRequest* req = RequestFor(*f);
                ASSERT_NE(req, nullptr) << f->Name;
                EXPECT_EQ(req->BindingReject, GroomBindingRejectReason::None) << f->Name;
                const RootDistance onSkin = RootsToSkin(*f, false);
                const RootDistance control = RootsToSkin(*f, true);
                std::printf("[fox] walk step %u %s: roots to the skinned skin p50 %.3f p99 %.3f max %.3f mm (%u roots, %u "
                            "invalid); to the BIND-pose skin p99 %.1f max %.1f mm\n",
                            step, f->Tag.c_str(), onSkin.P50 * 1000.0, onSkin.P99 * 1000.0, onSkin.Max * 1000.0,
                            onSkin.Roots, onSkin.Invalid, control.P99 * 1000.0, control.Max * 1000.0);
                EXPECT_GT(onSkin.Roots, 1000u) << f->Name;
                worstP99 = std::max(worstP99, onSkin.P99);
                worstMax = std::max(worstMax, onSkin.Max);
                controlP99 = std::max(controlP99, control.P99);
            }
            for (FoxRig* f : Foxes())
            {
                const View view = HeroViews(*f)[2].second; // the open side
                const IdCount ids = CountIds(view, IdOf(f->Coat), IdOf(f->Body), 4);
                coverage[f->Tag].push_back(ids.Coat);
            }
            std::vector<u8> frame;
            Capture("", { glm::vec3(0.0f, 0.95f, 3.2f), glm::vec3(0.0f, 0.45f, 0.0f), 40.0f }, frame, 12);
            ASSERT_FALSE(HasFatalFailure());
            strip.push_back(std::move(frame));
            std::fflush(stdout);
        }
        EXPECT_LT(worstP99, 0.0005) << "the coat's roots must sit on the walking fox's skin (p99 under 0.5 mm)";
        EXPECT_LT(worstMax, 0.002) << "no root more than 2 mm off the skin";
        // THE CONTROL: the same measure against the fox at its bind pose must
        // see the walk, or the assertion above could not have failed.
        EXPECT_GT(controlP99, 0.02) << "the bind-pose control did not see the walk: the measure is blind";

        for (const auto& [tag, values] : coverage)
        {
            const f64 mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<f64>(values.size());
            EXPECT_GT(mean, 0.03 * kWidth * kHeight) << tag << ": the coat must fill its framing";
            for (sizet i = 0; i < values.size(); ++i)
            {
                EXPECT_NEAR(values[i] / mean, 1.0, 0.25) << tag << " step " << i << ": the coat's share jumped mid-stride";
            }
        }

        // The stride as one strip, 4 x 2 tiles at half resolution.
        constexpr u32 kTileW = kWidth / 2u;
        constexpr u32 kTileH = kHeight / 2u;
        std::vector<u8> tiles(static_cast<sizet>(kTileW) * 4u * kTileH * 2u * 4u, 0u);
        for (u32 i = 0; i < kSteps; ++i)
        {
            const u32 ox = (i % 4u) * kTileW;
            const u32 oy = (i / 4u) * kTileH;
            for (u32 y = 0; y < kTileH; ++y)
            {
                for (u32 x = 0; x < kTileW; ++x)
                {
                    std::memcpy(&tiles[(static_cast<sizet>(oy + y) * kTileW * 4u + ox + x) * 4u],
                                &strip[i][(static_cast<sizet>(y * 2u) * kWidth + x * 2u) * 4u], 4u);
                }
            }
        }
        WritePng("FoxCoatWalk_GL_Forward_Stride", tiles, kTileW * 4u, kTileH * 2u);
    }

    // =========================================================================
    // At the hero framings the coat hides the skin: averaged over stochastic
    // frames, the fox's own surface is the front-most surface at under a few
    // percent of the pixels it covers with the coat off. The PNGs are the
    // look; the counts are what a look cannot fake.
    // =========================================================================
    TEST_F(FoxCoatEvidenceTest, TheCoatHidesTheSkinAtTheHeroFramings)
    {
        SetPath(RenderingPath::Forward);
        (void)PlayFromStart(40);
        Freeze();

        for (FoxRig* f : Foxes())
        {
            SCOPED_TRACE(f->Name);
            auto& gc = f->Coat.GetComponent<GroomComponent>();
            for (const auto& [label, view] : HeroViews(*f))
            {
                gc.m_RenderStrands = false;
                const IdCount bare = CountIds(view, IdOf(f->Coat), IdOf(f->Body), 4);
                gc.m_RenderStrands = true;
                std::vector<f32> skinMap;
                const IdCount coated = CountIds(view, IdOf(f->Coat), IdOf(f->Body), 24, false, &skinMap);
                ASSERT_GT(bare.Body, 2000.0) << label << ": the fox must be in frame for its skin to be measured";
                const f64 skinShowing = coated.Body / bare.Body;
                std::printf("[fox] hero %s %s: fox %.0f px bare; coated: coat %.0f px, skin still showing %.0f px = %.2f%%\n",
                            f->Tag.c_str(), label.c_str(), bare.Body, coated.Coat, coated.Body, 100.0 * skinShowing);
                std::fflush(stdout);
                // Measured 2.0-4.5% on every framing. The head close-up keeps
                // the eyes and the nose bare, and its fur is the shortest.
                const f64 bound = label == "Head" ? 0.06 : 0.05;
                EXPECT_LT(skinShowing, bound) << label << ": skin shows through the coat";

                std::vector<u8> frame;
                ColdHistory();
                Capture("FoxCoat_GL_Forward_" + f->Tag + "_" + label, view, frame);
                ASSERT_FALSE(HasFatalFailure());
                // Where the skin shows: the frame in grey, the fox's own surface
                // in red by how often it was the front-most surface.
                if (skinMap.size() == static_cast<sizet>(kWidth) * kHeight)
                {
                    std::vector<u8> map(frame.size());
                    for (u32 y = 0; y < kHeight; ++y)
                    {
                        for (u32 x = 0; x < kWidth; ++x)
                        {
                            const sizet o = (static_cast<sizet>(y) * kWidth + x) * 4u;
                            const f32 skin = skinMap[static_cast<sizet>(kHeight - 1u - y) * kWidth + x];
                            const u8 grey = static_cast<u8>((frame[o] + frame[o + 1] + frame[o + 2]) / 6);
                            map[o] = static_cast<u8>(std::min(255.0f, grey + 255.0f * skin));
                            map[o + 1] = grey;
                            map[o + 2] = grey;
                            map[o + 3] = 255u;
                        }
                    }
                    WritePng("FoxCoatSkin_GL_Forward_" + f->Tag + "_" + label, map, kWidth, kHeight);
                }
            }
        }
        // THE PAIR, BOTH FROM THE SUN'S SIDE, AND HELD TO THE SOLO FRAMES. A coat
        // can hide the skin and still be the wrong colour -- a coat refused its
        // density volume renders near-black (AHerdKeepsEveryCoatVolume) -- so
        // each coat's mean linear luma in the two-coat frame must stay within a
        // factor of its own lit side view. The first pair framing looked from
        // x = 0, between the foxes: the long fox showed the flank facing away
        // from the sun, and in this sky-less scene that flank is near-black for
        // either coat (long 0.059, short seen the same way 0.039, against
        // 0.10-0.13 from the lit side), which read as a broken long coat.
        const View pair{ glm::vec3(4.6f, 1.4f, 4.2f), glm::vec3(-0.4f, 0.45f, -0.2f), 38.0f };
        for (FoxRig* f : Foxes())
        {
            const View lit = HeroViews(*f)[2].second; // the side view, from +x
            const IdCount solo = CountIds(lit, IdOf(f->Coat), IdOf(f->Body), 12, true);
            const IdCount both = CountIds(pair, IdOf(f->Coat), IdOf(f->Body), 12, true);
            ASSERT_GT(solo.Coat, 1000.0) << f->Name;
            ASSERT_GT(both.Coat, 1000.0) << f->Name << ": both coats must be in the pair frame";
            const f64 soloLuma = solo.CoatLuma / solo.Coat;
            const f64 pairLuma = both.CoatLuma / both.Coat;
            std::printf("[fox] pair %s: mean coat luma %.4f in the pair, %.4f alone (ratio %.2f)\n", f->Tag.c_str(),
                        pairLuma, soloLuma, pairLuma / soloLuma);
            std::fflush(stdout);
            EXPECT_GT(pairLuma / soloLuma, 0.65) << f->Name << ": the coat went dark in the two-coat frame";
            EXPECT_LT(pairLuma / soloLuma, 1.5) << f->Name;
        }
        std::vector<u8> frame;
        ColdHistory();
        Capture("FoxCoat_GL_Forward_Pair", pair, frame);
    }

    // =========================================================================
    // The LOD ladder keeps the coat from near to far: past the hand-over the
    // coat draws its cooked cards, and the cards keep the strands' coverage
    // and energy. Frozen pose; ladder on against ladder off at each stop, the
    // same stochastic frames in both arms.
    // =========================================================================
    TEST_F(FoxCoatEvidenceTest, TheCardsKeepTheCoatFromNearToFar)
    {
        SetPath(RenderingPath::Forward);
        (void)PlayFromStart(30);
        Freeze();

        for (FoxRig* f : Foxes())
        {
            SCOPED_TRACE(f->Name);
            const glm::vec3 target = World(*f, (f->Frame.Min + f->Frame.Max) * 0.5f);
            const glm::vec3 away = glm::normalize(glm::vec3(f->Position.x < 0.0f ? -1.0f : 1.0f, 0.18f, 0.35f));
            const auto viewAt = [&](f32 distance)
            { return View{ target + away * distance, target, 40.0f }; };

            // Where the hand-over is, measured: the ladder's own apparent size
            // at one distance, scaled (it is inversely proportional).
            constexpr f32 kProbe = 6.0f;
            (void)CountIds(viewAt(kProbe), IdOf(f->Coat), IdOf(f->Body), 1);
            const GroomStrandRequest* probe = RequestFor(*f);
            ASSERT_NE(probe, nullptr);
            const f32 handover = kProbe * probe->ApparentPixelSize / kCardPixelSize;
            std::printf("[fox] lod %s: %.0f px at %.1f m, hand-over at %.1f m\n", f->Tag.c_str(),
                        static_cast<f64>(probe->ApparentPixelSize), static_cast<f64>(kProbe), static_cast<f64>(handover));

            struct Stop
            {
                const char* Name;
                f32 Distance;
                bool Cards;
            };
            const Stop stops[] = {
                { "Near", 0.35f * handover, false },
                { "Mid", 0.70f * handover, false },
                { "Handover", 1.25f * handover, true },
                { "Far", 2.2f * handover, true },
            };
            auto& lod = f->Coat.GetComponent<GroomLodComponent>();
            for (const Stop& stop : stops)
            {
                const View view = viewAt(stop.Distance);
                lod.m_Enabled = true;
                const IdCount on = CountIds(view, IdOf(f->Coat), IdOf(f->Body), 24, true);
                const GroomStrandRequest* req = RequestFor(*f);
                ASSERT_NE(req, nullptr);
                const GroomRepresentation representation = req->Lod.Representation;
                std::vector<u8> crop;
                ColdHistory();
                Capture("FoxCoatLod_GL_Forward_" + f->Tag + "_" + stop.Name, view, crop);
                lod.m_Enabled = false;
                const IdCount off = CountIds(view, IdOf(f->Coat), IdOf(f->Body), 24, true);
                lod.m_Enabled = true;
                const f64 coverage = off.Coat > 0.0 ? on.Coat / off.Coat : 0.0;
                const f64 energy = off.CoatLuma > 0.0 ? on.CoatLuma / off.CoatLuma : 0.0;
                std::printf("[fox] lod %s %s (%.1f m): representation %u, coverage %.3f (%.0f / %.0f px), energy %.3f\n",
                            f->Tag.c_str(), stop.Name, static_cast<f64>(stop.Distance), static_cast<u32>(representation),
                            coverage, on.Coat, off.Coat, energy);
                std::fflush(stdout);
                EXPECT_GT(off.Coat, 200.0) << stop.Name << ": the coat must be measurable";
                EXPECT_EQ(representation, stop.Cards ? GroomRepresentation::Card : GroomRepresentation::Strand) << stop.Name;
                if (!stop.Cards && stop.Distance < 0.5f * handover)
                {
                    EXPECT_NEAR(coverage, 1.0, 0.03) << stop.Name << ": the control, both arms the same coat";
                    continue;
                }
                // groom-card-coverage.md's gross guards for the shipped
                // coat-shadow LOD, the horses' bounds.
                EXPECT_GT(coverage, 0.85) << stop.Name << ": the ladder lost the coat's coverage";
                EXPECT_LT(coverage, 1.25) << stop.Name << ": the ladder ballooned the coat";
                EXPECT_GT(energy, 0.75) << stop.Name << ": the ladder darkened the coat";
                EXPECT_LT(energy, 1.40) << stop.Name << ": the ladder brightened the coat";
            }
        }
    }

    // =========================================================================
    // A herd keeps every coat's density volume (#1259). A coat refused its
    // volume loses its self-shadow AND its dual scattering, and renders dark
    // brown where it is orange: in AnimalPopulation.olo forty foxes reached the
    // eight resident-volume slots first and the hero was refused. Sixteen foxes
    // here, twice the old slot count, all close enough to want a volume; the
    // byte budget admits the small ones past the eighth.
    // =========================================================================
    TEST_F(FoxCoatEvidenceTest, AHerdKeepsEveryCoatVolume)
    {
        SetPath(RenderingPath::Forward);
        std::vector<FoxRig> herd(14);
        for (u32 i = 0; i < herd.size(); ++i)
        {
            FoxRig& f = herd[i];
            const f32 angle = static_cast<f32>(i) * 0.45f;
            const glm::vec3 at(std::cos(angle) * (3.0f + 0.25f * static_cast<f32>(i)), 0.0f,
                               -2.0f - std::sin(angle) * (2.0f + 0.2f * static_cast<f32>(i)));
            f.Model = (i % 2u == 0u) ? m_Short.Model : m_Long.Model;
            f.Body = GetScene().CreateEntity("HerdFox" + std::to_string(i));
            auto& t = f.Body.GetComponent<TransformComponent>();
            t.Translation = at;
            t.Scale = glm::vec3(kFoxCookScale * (0.9f + 0.02f * static_cast<f32>(i % 6u)));
            (void)ModelImporter::PopulateAnimatedEntity(f.Body, f.Model, FoxPath().string(), true);
            const FoxRig& source = (i % 2u == 0u) ? m_Short : m_Long;
            f.Coat = MakeCoat("HerdFoxCoat" + std::to_string(i), source.GroomHandle, source.BindingHandle, f.Body,
                              source.Grown.Strands);
        }
        (void)PlayFromStart(12);
        const GroomCoatShadowStats& shadow = PassStats().CoatShadow;
        std::printf("[fox] herd of %zu: coat volumes shadowed %u, fallen back %u (budget %u)\n", herd.size() + 2u,
                    shadow.ShadowedGrooms, shadow.FallbackGrooms,
                    shadow.ByReason[static_cast<sizet>(GroomCoatShadowFallbackReason::BudgetExhausted)]);
        ASSERT_GT(PassStats().GroomsDrawn, 8u) << "more coats than the old eight slots must be in view";
        EXPECT_EQ(shadow.ByReason[static_cast<sizet>(GroomCoatShadowFallbackReason::BudgetExhausted)], 0u)
            << "a coat in the herd was refused its volume";
        EXPECT_EQ(shadow.FallbackGrooms, 0u);
        EXPECT_EQ(shadow.ShadowedGrooms, PassStats().GroomsDrawn);
    }

    // =========================================================================
    // The coat colour map reads back the same through the editor's import as
    // through a direct Texture2D load: the coat samples it on the CPU at each
    // strand's root UV, so a row order that differed between the two would
    // paint the evidence and the live scene from mirrored halves of the map.
    // =========================================================================
    TEST_F(FoxCoatEvidenceTest, TheColourMapReadsTheSameThroughEveryLoadPath)
    {
        const Ref<Texture2D> imported = AssetManager::GetAsset<Texture2D>(m_ColorMap);
        ASSERT_TRUE(imported);
        const Ref<Texture2D> direct =
            Texture2D::Create((Project::GetAssetDirectory() / "FoxCoatColor.png").string(), true);
        ASSERT_TRUE(direct);
        TArray64<u8> a, b;
        ASSERT_TRUE(imported->GetData(a));
        ASSERT_TRUE(direct->GetData(b));
        ASSERT_EQ(a.Num(), b.Num());
        ASSERT_EQ(a.Num(), static_cast<i64>(m_Texture.Width) * m_Texture.Height * 4);
        // Row 0 of the readback against the PNG's own rows: which end is it?
        const std::vector<u8> png = BuildFoxCoatColorMap(m_Texture);
        const auto rowDiff = [&](const TArray64<u8>& data, int pngRow)
        {
            u64 diff = 0;
            for (int x = 0; x < m_Texture.Width * 4; ++x)
            {
                diff += static_cast<u64>(std::abs(static_cast<int>(data[x]) -
                                                  static_cast<int>(png[static_cast<sizet>(pngRow) * m_Texture.Width * 4 + x])));
            }
            return diff;
        };
        std::printf("[fox] colour map row 0 vs PNG top/bottom: imported %llu/%llu, direct %llu/%llu\n",
                    static_cast<unsigned long long>(rowDiff(a, 0)),
                    static_cast<unsigned long long>(rowDiff(a, m_Texture.Height - 1)),
                    static_cast<unsigned long long>(rowDiff(b, 0)),
                    static_cast<unsigned long long>(rowDiff(b, m_Texture.Height - 1)));
        sizet differing = 0;
        for (i64 i = 0; i < a.Num(); ++i)
        {
            differing += a[i] != b[i] ? 1u : 0u;
        }
        EXPECT_EQ(differing, 0u) << "the editor import and a direct load read the colour map back differently";
    }

    // =========================================================================
    // The cooked bindings accept the Fox however it is loaded: a COLD import
    // (the mesh cache dropped first) and then a WARM load from the .omesh cache
    // that import wrote -- the two paths the editor takes on a scene's first and
    // second open. A binding addresses triangles by index, so a load that
    // returned a different index buffer would refuse the coat on one of them
    // (mesh-cache-must-return-the-surface-it-was-given.md). Both loads are
    // forced, or on a tree whose cache already exists both are warm.
    // =========================================================================
    TEST_F(FoxCoatEvidenceTest, TheBindingsAcceptColdAndWarmLoadsOfTheFox)
    {
        MeshCache::InvalidateCache(FoxPath());
        MeshCache::InvalidateCache(FoxPath(), AnimatedModel::kCachePrefix);
        for (const char* which : { "cold", "warm" })
        {
            const Ref<AnimatedModel> reloaded = Ref<AnimatedModel>::Create(FoxPath().string());
            ASSERT_TRUE(reloaded);
            const Ref<MeshSource> body = reloaded->GetEntityMeshSource();
            ASSERT_TRUE(body);
            const GroomSurfaceView view = MakeSurfaceView(*body, reloaded->GetSkeleton().Raw());
            for (FoxRig* f : Foxes())
            {
                EXPECT_EQ(f->Binding->CheckCompatibility(GroomBindingBuilder::SignGroom(*f->Grown.Groom),
                                                         GroomBindingBuilder::SignTarget(view)),
                          GroomBindingRejectReason::None)
                    << f->Name << ": the binding must accept a " << which << " load of the Fox";
            }
        }
        // And the live pass attaches both coats, with no refusal, on a frame.
        const MotionResult motion = PlayFromStart(4);
        EXPECT_EQ(motion.MinGroomsDeformed, 2u);
        EXPECT_EQ(motion.MaxBindingRefused, 0u);
    }

    // =========================================================================
    // Not a test: the generator of the live coats. With OLO_FOX_COATS_EXPORT=1
    // it writes FoxShortCoat / FoxLongCoat (.ologroom + .ologroombinding) and
    // FoxCoatColor.png to SandboxProject/Assets/Grooms/Animals/, and registers
    // them through the Sandbox project's own EditorAssetManager, the only
    // writer of AssetRegistry.oar. A file already registered keeps its handle.
    // generate_animal_population_scene.py reads the handles back from the
    // registry by path, so after a registry merge the recipe is: take either
    // side's AssetRegistry.oar, re-run this, re-run the two scene generators.
    // =========================================================================
    TEST_F(FoxCoatEvidenceTest, ExportsTheFoxCoats)
    {
        const char* flag = std::getenv("OLO_FOX_COATS_EXPORT");
        if (flag == nullptr || flag[0] != '1')
        {
            GTEST_SKIP() << "set OLO_FOX_COATS_EXPORT=1 to regenerate SandboxProject/Assets/Grooms/Animals/Fox*";
        }

        const fs::path sandbox = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject";
        ASSERT_TRUE(Project::Load(sandbox / "Sandbox.oloproj"));
        auto assets = Ref<EditorAssetManager>::Create();
        assets->Initialize(false);
        Project::SetAssetManager(assets);
        const fs::path assetDir = Project::GetAssetDirectory();
        const fs::path groomDir = assetDir / "Grooms" / "Animals";
        std::error_code ec;
        fs::create_directories(groomDir, ec);
        ASSERT_FALSE(ec);

        const fs::path colorPath = groomDir / "FoxCoatColor.png";
        const std::vector<u8> colorMap = BuildFoxCoatColorMap(m_Texture);
        ASSERT_NE(::stbi_write_png(colorPath.string().c_str(), m_Texture.Width, m_Texture.Height, 4, colorMap.data(),
                                   m_Texture.Width * 4),
                  0);
        const AssetHandle color = assets->ImportAsset(colorPath);
        ASSERT_NE(static_cast<u64>(color), 0u);
        std::printf("[fox] exported FoxCoatColor.png: texture %llu\n", static_cast<unsigned long long>(static_cast<u64>(color)));

        for (FoxRig* f : Foxes())
        {
            std::string reason;
            std::vector<u8> groomBytes, bindingBytes;
            ASSERT_TRUE(GroomSerializer::EncodeToBytes(*f->Grown.Groom, groomBytes, reason)) << reason;
            ASSERT_TRUE(GroomBindingSerializer::EncodeToBytes(*f->Binding, bindingBytes, reason)) << reason;
            const fs::path groomPath = groomDir / (f->Name + ".ologroom");
            const fs::path bindingPath = groomDir / (f->Name + ".ologroombinding");
            std::ofstream(groomPath, std::ios::binary)
                .write(reinterpret_cast<const char*>(groomBytes.data()), static_cast<std::streamsize>(groomBytes.size()));
            std::ofstream(bindingPath, std::ios::binary)
                .write(reinterpret_cast<const char*>(bindingBytes.data()), static_cast<std::streamsize>(bindingBytes.size()));
            const AssetHandle groom = assets->ImportAsset(groomPath);
            const AssetHandle binding = assets->ImportAsset(bindingPath);
            ASSERT_NE(static_cast<u64>(groom), 0u) << groomPath.string();
            ASSERT_NE(static_cast<u64>(binding), 0u) << bindingPath.string();

            // The loose files are the cooked bytes, and the binding accepts
            // them (the scene loader's own compatibility check).
            auto loadedGroom = AssetManager::GetAsset<GroomAsset>(groom);
            auto loadedBinding = AssetManager::GetAsset<GroomBindingAsset>(binding);
            ASSERT_TRUE(loadedGroom);
            ASSERT_TRUE(loadedBinding);
            std::vector<u8> again;
            ASSERT_TRUE(GroomSerializer::EncodeToBytes(*loadedGroom, again, reason)) << reason;
            EXPECT_EQ(again, groomBytes) << "loose groom";
            const Ref<MeshSource> bodyRef = f->Model->GetEntityMeshSource();
            ASSERT_TRUE(bodyRef);
            const MeshSource& body = *bodyRef;
            EXPECT_EQ(loadedBinding->CheckCompatibility(GroomBindingBuilder::SignGroom(*loadedGroom),
                                                        GroomBindingBuilder::SignTarget(
                                                            MakeSurfaceView(body, f->Model->GetSkeleton().Raw()))),
                      GroomBindingRejectReason::None)
                << f->Name;

            std::printf("[fox] exported %s: groom %llu (%zu bytes, %u strands, %u cards), binding %llu (%zu bytes)\n",
                        f->Name.c_str(), static_cast<unsigned long long>(static_cast<u64>(groom)), groomBytes.size(),
                        f->Grown.Strands, f->CardCount, static_cast<unsigned long long>(static_cast<u64>(binding)),
                        bindingBytes.size());
        }
        ASSERT_TRUE(assets->SerializeAssetRegistry());
        std::fflush(stdout);

        // Leave a SCRATCH project active, never the Sandbox one (see
        // GroomAnimalsAcceptanceEvidenceTest.ExportsTheLiveScene).
        const fs::path scratch = TempDir("after-export");
        fs::create_directories(scratch / "Assets", ec);
        {
            std::ofstream proj(scratch / "Scratch.oloproj");
            proj << "Project:\n"
                    "  Name: FoxCoatScratch\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }
        ASSERT_TRUE(Project::Load(scratch / "Scratch.oloproj"));
        auto scratchAssets = Ref<EditorAssetManager>::Create();
        scratchAssets->Initialize(false);
        Project::SetAssetManager(scratchAssets);
    }
} // namespace OloEngine::Tests
