#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// GroomAnimalsAcceptanceEvidenceTest -- epic #1223, the integrated acceptance.
//
// Every groom child (#1232, #1246-#1253) on three MOVING subjects in one
// scene: a short-coated horse and a long-coated horse (SandboxProject/Assets/
// Models/Horse, CC0, walking), and a human head (InfiniteScanHead's rigged
// variant, turning). Each coat is grown on its body's own bind-pose surface,
// cooked, and bound with the real cooker; each body plays its own clip; every
// coat carries every child's component. docs/guides/groom-animals.md explains
// the subjects and the live scene this file also exports.
//
// Evidence PNGs: OloEditor/assets/tests/visual/GroomAnimals*_GL_<Path>[_*].png.
// GL only -- every Vulkan cell, and the ray-traced proxies, are live-only.
//
// The epic's criteria are judgement words ("convincing"), so these assertions
// are what a frame cannot fake -- coverage floors, per-child negative controls
// against a measured repeat floor, conservation through a stride and across
// the LOD ladder, temporal settling, cost against named hardware, and the
// cooked-asset round trips -- and the PNGs are for looking at. Where a
// measured value is a known gap, the assertion states the gap rather than
// hiding it (visual-quality-criteria.md).
//
// RUN IT IN RELEASE. The coats are ~200k strands; a Debug binary pays ~11 s a
// frame for them.
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"
#include "../../MemoryCeiling.h"

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
#include "OloEngine/Groom/GroomLodBuilder.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/ModelImporter.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/AnimalScheduler.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/TemporalHistoryRegistry.h"
#include "OloEngine/Renderer/TemporalSequenceMetrics.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <numeric>
#include <cstdlib>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

        [[nodiscard]] fs::path HorsePath()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject/Assets/Models/Horse/Horse.gltf";
        }
        [[nodiscard]] fs::path HorseAlbedoPath()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject/Assets/Models/Horse/HorseAlbedo.jpg";
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

        // Coat regions of the horse. Classified per triangle from the bone
        // that dominates its skinning (the rig's own anatomy), refined by
        // where the triangle faces for the two regions no bone names: the
        // mane crest along the top of the neck, and the forelock at the poll.
        enum class Region : u8
        {
            Body,
            Neck,
            Mane,
            Head,
            Forelock,
            Leg,
            LowerLeg,
            Tail,
            Scalp,
            Bare, // skin that grows nothing (the face, the shoulders of a bust)
            Count
        };

        [[nodiscard]] const char* RegionName(Region region)
        {
            switch (region)
            {
                case Region::Body:
                    return "body";
                case Region::Neck:
                    return "neck";
                case Region::Mane:
                    return "mane";
                case Region::Head:
                    return "head";
                case Region::Forelock:
                    return "forelock";
                case Region::Leg:
                    return "leg";
                case Region::LowerLeg:
                    return "lowerleg";
                case Region::Tail:
                    return "tail";
                case Region::Scalp:
                    return "scalp";
                case Region::Bare:
                    return "bare";
                case Region::Count:
                    break;
            }
            return "body";
        }

        // One triangle of the body, already in the GROOM's space (metres).
        struct SurfaceTriangle
        {
            std::array<glm::vec3, 3> P;
            std::array<glm::vec2, 3> UV;
            glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
            f32 Area = 0.0f;
            std::string Bone;
            Region Where = Region::Body;
        };

        struct BodyFrame
        {
            glm::vec3 Forward{ 0.0f, 0.0f, 1.0f }; // toward the head
            glm::vec3 Up{ 0.0f, 1.0f, 0.0f };
            glm::vec3 Min{ 0.0f };
            glm::vec3 Max{ 0.0f };
            glm::vec3 HeadCentre{ 0.0f };
            f32 HeadLength = 0.0f;
            glm::vec3 Crown{ 0.0f }; // the scalp's whorl, for a human head
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

        [[nodiscard]] bool IsOneOf(const std::string& bone, std::initializer_list<const char*> names)
        {
            return std::any_of(names.begin(), names.end(), [&](const char* n)
                               { return bone == n; });
        }

        // The horse rig's bone names (prepare_horse.py documents the chains).
        [[nodiscard]] Region RegionFromBone(const std::string& bone)
        {
            if (IsOneOf(bone, { "Bone.003", "Bone.004" }))
            {
                return Region::Tail;
            }
            if (IsOneOf(bone, { "Bone.002", "Bone.001_L", "Bone.001_R" }))
            {
                return Region::Head;
            }
            if (bone == "Bone.001")
            {
                return Region::Neck;
            }
            if (IsOneOf(bone, { "Bone_L.002", "Bone_R.002", "Bone_L.005", "Bone_R.005" }))
            {
                return Region::LowerLeg;
            }
            if (IsOneOf(bone, { "Bone_L", "Bone_R", "Bone_L.001", "Bone_R.001", "Bone_L.004", "Bone_R.004" }))
            {
                return Region::Leg;
            }
            return Region::Body;
        }

        enum class Subject : u8
        {
            Horse,
            HumanHead
        };

        // The human bust (HeadRigged.gltf): metres, base on the origin, face
        // toward +Z, 0.43 m tall. The hairline is a height threshold that
        // rises from the nape to the forehead, and lifts again over the ears.
        // Measured on the scan: the forehead hairline sits at ~0.84 of the
        // bust's height, the temples at ~0.72, the nape at ~0.55.
        constexpr f32 kBustHeight = 0.43f;

        [[nodiscard]] Region ClassifyScalp(const glm::vec3& local, const glm::vec3& normal)
        {
            const f32 x = local.x / kBustHeight;
            const f32 y = local.y / kBustHeight;
            const f32 z = local.z / kBustHeight;
            const f32 front = std::clamp((z + 0.02f) / 0.16f, 0.0f, 1.0f); // 0 at the back, 1 at the brow
            f32 hairline = 0.55f + (0.29f * front * front);
            if (std::abs(x) > 0.19f)
            {
                hairline = std::max(hairline, 0.71f); // clear the ears
            }
            const bool facesOut = normal.y > -0.2f;
            return (y > hairline && facesOut) ? Region::Scalp : Region::Bare;
        }

        [[nodiscard]] std::vector<SurfaceTriangle> CollectSurface(const MeshSource& surface, const Skeleton& skeleton,
                                                                  const glm::mat4& surfaceToGroom, BodyFrame& frame,
                                                                  Subject subject = Subject::Horse)
        {
            std::vector<SurfaceTriangle> out;
            const auto& vertices = surface.GetVertices();
            const auto& indices = surface.GetIndices();
            const u32 triangles = static_cast<u32>(indices.Num() / 3);
            out.reserve(triangles);
            glm::vec3 headSum(0.0f), tailSum(0.0f);
            u32 headCount = 0, tailCount = 0;
            frame.Min = glm::vec3(1.0e9f);
            frame.Max = glm::vec3(-1.0e9f);

            // Which way the face points, MEASURED rather than assumed: the
            // nose is the furthest-forward point at nose height, so whichever
            // Z extreme is larger there is the front. A hairline drawn on the
            // wrong side puts the hair over the face.
            f32 faceSign = 1.0f;
            if (subject == Subject::HumanHead)
            {
                f32 zMax = -1.0e9f, zMin = 1.0e9f;
                for (const Vertex& v : vertices)
                {
                    const f32 y = v.Position.y / kBustHeight;
                    if (y > 0.58f && y < 0.68f && std::abs(v.Position.x) < 0.03f)
                    {
                        zMax = std::max(zMax, v.Position.z);
                        zMin = std::min(zMin, v.Position.z);
                    }
                }
                faceSign = (zMax >= -zMin) ? 1.0f : -1.0f;
                std::printf("[groom-animals] human face toward %cZ (nose-height z range %.3f..%.3f)\n",
                            faceSign > 0.0f ? '+' : '-', static_cast<f64>(zMin), static_cast<f64>(zMax));
            }

            for (u32 t = 0; t < triangles; ++t)
            {
                SurfaceTriangle tri;
                glm::vec3 shading(0.0f);
                for (u32 c = 0; c < 3; ++c)
                {
                    const u32 index = indices[static_cast<i32>(t * 3u + c)];
                    const Vertex& v = vertices[static_cast<i32>(index)];
                    tri.P[c] = glm::vec3(surfaceToGroom * glm::vec4(v.Position, 1.0f));
                    tri.UV[c] = v.TexCoord;
                    shading += glm::mat3(surfaceToGroom) * v.Normal;
                    frame.Min = glm::min(frame.Min, tri.P[c]);
                    frame.Max = glm::max(frame.Max, tri.P[c]);
                }
                glm::vec3 n = glm::cross(tri.P[1] - tri.P[0], tri.P[2] - tri.P[0]);
                tri.Area = 0.5f * glm::length(n);
                if (tri.Area <= 1.0e-10f)
                {
                    continue;
                }
                n = glm::normalize(n);
                if (glm::dot(shading, shading) > 1.0e-12f && glm::dot(n, shading) < 0.0f)
                {
                    n = -n;
                }
                tri.Normal = n;
                tri.Bone = DominantBone(surface, skeleton, indices[static_cast<i32>(t * 3u)]);
                if (subject == Subject::HumanHead)
                {
                    // Classified in the mesh's OWN space, where the hairline
                    // is measured, whatever transform the entity carries.
                    glm::vec3 local(0.0f);
                    glm::vec3 localShading(0.0f);
                    for (u32 c = 0; c < 3; ++c)
                    {
                        const Vertex& v = vertices[static_cast<i32>(indices[static_cast<i32>(t * 3u + c)])];
                        local += glm::vec3(v.Position.x, v.Position.y, v.Position.z * faceSign) / 3.0f;
                        localShading += glm::vec3(v.Normal.x, v.Normal.y, v.Normal.z * faceSign);
                    }
                    tri.Where = ClassifyScalp(local, glm::normalize(localShading));
                    out.push_back(std::move(tri));
                    continue;
                }
                tri.Where = RegionFromBone(tri.Bone);
                const glm::vec3 centroid = (tri.P[0] + tri.P[1] + tri.P[2]) / 3.0f;
                if (tri.Where == Region::Head)
                {
                    headSum += centroid;
                    ++headCount;
                }
                else if (tri.Where == Region::Tail)
                {
                    tailSum += centroid;
                    ++tailCount;
                }
                out.push_back(std::move(tri));
            }
            if (headCount > 0 && tailCount > 0)
            {
                glm::vec3 f = (headSum / static_cast<f32>(headCount)) - (tailSum / static_cast<f32>(tailCount));
                f.y = 0.0f;
                frame.Forward = glm::normalize(f);
                frame.HeadCentre = headSum / static_cast<f32>(headCount);
            }
            // Head extent along the facing axis, for the forelock band.
            f32 lo = 1.0e9f, hi = -1.0e9f;
            for (const auto& tri : out)
            {
                if (tri.Where == Region::Head)
                {
                    for (const auto& p : tri.P)
                    {
                        const f32 s = glm::dot(p, frame.Forward);
                        lo = std::min(lo, s);
                        hi = std::max(hi, s);
                    }
                }
            }
            frame.HeadLength = hi > lo ? hi - lo : 0.0f;

            if (subject == Subject::HumanHead)
            {
                frame.Forward = glm::normalize(glm::vec3(surfaceToGroom * glm::vec4(0.0f, 0.0f, faceSign, 0.0f)));
                frame.Up = glm::normalize(glm::vec3(surfaceToGroom * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));
                // The whorl: slightly behind the top of the skull.
                frame.Crown = glm::vec3(surfaceToGroom * glm::vec4(0.0f, 0.97f * kBustHeight, -0.03f * faceSign, 1.0f));
                return out;
            }

            // The tail bones' weights reach over the whole rump, but hair
            // grows from the DOCK: the rearmost few centimetres. Everything
            // else those bones carry is body coat.
            f32 rearmost = -1.0e9f;
            for (const auto& tri : out)
            {
                if (tri.Where == Region::Tail)
                {
                    for (const auto& p : tri.P)
                    {
                        rearmost = std::max(rearmost, -glm::dot(p, frame.Forward));
                    }
                }
            }
            for (auto& tri : out)
            {
                if (tri.Where == Region::Tail)
                {
                    const glm::vec3 centroid = (tri.P[0] + tri.P[1] + tri.P[2]) / 3.0f;
                    if (-glm::dot(centroid, frame.Forward) < rearmost - 0.12f)
                    {
                        tri.Where = Region::Body;
                    }
                }
            }

            // The two regions no bone names.
            for (auto& tri : out)
            {
                const glm::vec3 centroid = (tri.P[0] + tri.P[1] + tri.P[2]) / 3.0f;
                const glm::vec3 sideAxis = glm::cross(frame.Forward, frame.Up);
                const f32 side = std::abs(glm::dot(tri.Normal, sideAxis));
                // Distance from the body's midline: a mane and a forelock grow
                // ALONG the top line, a hand's width at most either side of it.
                const f32 offMidline = std::abs(glm::dot(centroid - ((frame.Min + frame.Max) * 0.5f), sideAxis));
                if (tri.Where == Region::Neck && tri.Normal.y > 0.35f && side < 0.75f && offMidline < 0.07f)
                {
                    tri.Where = Region::Mane;
                }
                else if (tri.Where == Region::Head && tri.Normal.y > 0.45f && side < 0.7f && offMidline < 0.06f)
                {
                    // The rearmost quarter of the head, between the ears: the poll.
                    const f32 s = glm::dot(centroid, frame.Forward);
                    if (s < lo + 0.3f * frame.HeadLength)
                    {
                        tri.Where = Region::Forelock;
                    }
                }
            }
            return out;
        }

        // One layer of a coat on one region.
        struct CoatLayer
        {
            Region Where = Region::Body;
            GroomCoatRole Role = GroomCoatRole::Undercoat;
            f32 StrandsPerM2 = 10000.0f;
            f32 Length = 0.01f;       // metres
            f32 RootDiameter = 1e-4f; // metres
            f32 Lift = 0.5f;          // how far off the skin the strand leaves (0 = combed flat)
            f32 Droop = 0.2f;         // per-segment gravity bend
            u32 Points = 6;
            glm::vec3 Tint{ 1.0f };
            f32 Clump = 0.0f;
            f32 Wander = 0.3f;         // per-strand direction noise; frizz when high
            f32 TaperFromFront = 0.0f; // metres over which length ramps up from the region's front edge
            f32 TaperBase = 0.3f;      // length fraction AT the front edge
        };

        struct CoatRecipe
        {
            const char* Name = "coat";
            std::vector<CoatLayer> Layers;
            u32 GuideEvery = 12;
            u32 Seed = 1;
        };

        [[nodiscard]] const char* RoleSuffix(GroomCoatRole role)
        {
            switch (role)
            {
                case GroomCoatRole::Undercoat:
                    return "undercoat";
                case GroomCoatRole::GuardHair:
                    return "guard";
                case GroomCoatRole::Whisker:
                    return "whiskers";
                case GroomCoatRole::LongHair:
                    return "longhair";
                default:
                    return "coat";
            }
        }

        // The direction a region's hair is combed, in groom space.
        [[nodiscard]] glm::vec3 CombDirection(Region region, const BodyFrame& frame, const glm::vec3& at)
        {
            const glm::vec3 back = -frame.Forward;
            const glm::vec3 down = -frame.Up;
            switch (region)
            {
                case Region::Scalp:
                {
                    // Away from the whorl, then down: hair parts at the crown
                    // and falls.
                    glm::vec3 radial = at - frame.Crown;
                    radial -= frame.Up * glm::dot(radial, frame.Up);
                    radial = glm::dot(radial, radial) > 1.0e-8f ? glm::normalize(radial) : back;
                    // Hair that would fall FORWARD over the face is swept
                    // back and to the side instead, the way it parts: combed
                    // straight out from the whorl, the front hair is a
                    // curtain in front of the eyes.
                    const f32 forwardness = glm::dot(radial, frame.Forward);
                    if (forwardness > 0.0f)
                    {
                        const glm::vec3 side = glm::normalize(glm::cross(frame.Up, frame.Forward));
                        const f32 whichSide = glm::dot(at - frame.Crown, side) >= 0.0f ? 1.0f : -1.0f;
                        radial = glm::normalize(radial - frame.Forward * (1.4f * forwardness) + side * (whichSide * forwardness));
                    }
                    return glm::normalize(radial + down * 0.9f);
                }
                case Region::Leg:
                case Region::LowerLeg:
                    return glm::normalize(down + back * 0.15f);
                case Region::Tail:
                    return glm::normalize(back * 0.35f + down);
                case Region::Mane:
                {
                    // A mane falls to ONE side of the crest. On an up-facing
                    // crest "down" projects to nothing in the tangent plane,
                    // so the side has to be named or the hair stands up.
                    const glm::vec3 side = glm::normalize(glm::cross(frame.Up, frame.Forward));
                    return glm::normalize(side + down * 0.6f + back * 0.15f);
                }
                case Region::Forelock:
                    return glm::normalize(frame.Forward + down * 0.6f);
                case Region::Neck:
                    return glm::normalize(back * 0.6f + down * 0.8f);
                case Region::Head:
                    return glm::normalize(back + down * 0.2f);
                default:
                    return glm::normalize(back + down * 0.35f);
            }
        }

        // OLO_GROOM_REGION_DEBUG=1 paints each region one flat hue (and the
        // fixture drops the colour map), so the surface classification can be
        // checked by eye rather than trusted.
        [[nodiscard]] bool RegionDebugEnabled()
        {
            const char* v = std::getenv("OLO_GROOM_REGION_DEBUG");
            return v != nullptr && v[0] == '1';
        }

        [[nodiscard]] glm::vec3 RegionDebugTint(Region region)
        {
            switch (region)
            {
                case Region::Body:
                    return { 0.9f, 0.9f, 0.9f };
                case Region::Neck:
                    return { 0.2f, 0.4f, 1.0f };
                case Region::Mane:
                    return { 1.0f, 0.1f, 0.1f };
                case Region::Head:
                    return { 0.1f, 1.0f, 0.1f };
                case Region::Forelock:
                    return { 1.0f, 1.0f, 0.1f };
                case Region::Leg:
                    return { 1.0f, 0.5f, 0.0f };
                case Region::LowerLeg:
                    return { 0.6f, 0.1f, 1.0f };
                case Region::Tail:
                    return { 0.0f, 1.0f, 1.0f };
                default:
                    return { 1.0f, 0.2f, 0.8f };
            }
        }

        struct GrownCoat
        {
            Ref<GroomAsset> Groom;
            u32 Strands = 0;
            u32 Guides = 0;
            std::array<u32, static_cast<sizet>(Region::Count)> PerRegion{};
        };

        // Grow a coat on the body. Roots are sampled on the bind-pose
        // triangles, area-weighted and deterministic, with the body's own UV
        // as the root UV — so a colour map authored for the body's albedo
        // lands on the coat that grows out of it.
        [[nodiscard]] GrownCoat GrowCoat(const std::vector<SurfaceTriangle>& surface, const BodyFrame& frame,
                                         const CoatRecipe& recipe)
        {
            GrownCoat out;
            GroomBuilder builder;
            std::string reason;
            std::vector<u16> groupIds(recipe.Layers.size(), 0);
            for (sizet l = 0; l < recipe.Layers.size(); ++l)
            {
                const CoatLayer& layer = recipe.Layers[l];
                const std::string name = std::string(RegionName(layer.Where)) + "_" + RoleSuffix(layer.Role);
                EXPECT_TRUE(builder.AddGroup(name, groupIds[l], reason)) << reason;
                GroomCoatGroupDesc desc;
                desc.Role = static_cast<u8>(layer.Role);
                desc.Tint = RegionDebugEnabled() ? RegionDebugTint(layer.Where) : layer.Tint;
                desc.Clump = layer.Clump;
                std::vector<std::string> reasons;
                EXPECT_TRUE(builder.SetGroupCoat(groupIds[l], desc, reasons))
                    << (reasons.empty() ? std::string{} : reasons.front());
            }

            std::vector<glm::vec3> points;
            std::vector<f32> widths;
            for (sizet l = 0; l < recipe.Layers.size(); ++l)
            {
                const CoatLayer& layer = recipe.Layers[l];
                u32 inLayer = 0;
                f32 regionFront = -1.0e9f;
                for (const SurfaceTriangle& tri : surface)
                {
                    if (tri.Where == layer.Where)
                    {
                        for (const auto& p : tri.P)
                        {
                            regionFront = std::max(regionFront, glm::dot(p, frame.Forward));
                        }
                    }
                }
                for (u32 t = 0; t < static_cast<u32>(surface.size()); ++t)
                {
                    const SurfaceTriangle& tri = surface[t];
                    if (tri.Where != layer.Where)
                    {
                        continue;
                    }
                    const f32 expected = tri.Area * layer.StrandsPerM2;
                    u32 count = static_cast<u32>(expected);
                    if (Hash01(t, recipe.Seed * 131u + static_cast<u32>(l)) < (expected - static_cast<f32>(count)))
                    {
                        ++count;
                    }
                    const glm::vec3 centroid = (tri.P[0] + tri.P[1] + tri.P[2]) / 3.0f;
                    glm::vec3 comb = CombDirection(layer.Where, frame, centroid);
                    comb -= tri.Normal * glm::dot(comb, tri.Normal);
                    comb = glm::dot(comb, comb) > 1.0e-6f ? glm::normalize(comb) : glm::vec3(0.0f);

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
                        f32 length = layer.Length * (0.75f + (0.5f * Hash01(salt, 3u)));
                        if (layer.TaperFromFront > 0.0f)
                        {
                            // A mane is short at the poll and lengthens down
                            // the crest; full length at the front falls across
                            // the face and hangs under the jaw.
                            const f32 fromFront = regionFront - glm::dot(root, frame.Forward);
                            const f32 ramp = std::clamp(fromFront / layer.TaperFromFront, 0.0f, 1.0f);
                            length *= layer.TaperBase + ((1.0f - layer.TaperBase) * ramp);
                            if (length < 0.01f)
                            {
                                continue; // clipped: a bridle path grows nothing
                            }
                        }
                        const glm::vec3 wander =
                            glm::vec3(Hash01(salt, 4u) - 0.5f, Hash01(salt, 5u) - 0.5f, Hash01(salt, 6u) - 0.5f) *
                            layer.Wander;
                        glm::vec3 direction = glm::normalize((tri.Normal * layer.Lift) + comb + wander);

                        points.clear();
                        widths.clear();
                        glm::vec3 p = root;
                        const f32 step = length / static_cast<f32>(layer.Points - 1u);
                        for (u32 k = 0; k < layer.Points; ++k)
                        {
                            const f32 along = static_cast<f32>(k) / static_cast<f32>(layer.Points - 1u);
                            points.push_back(p);
                            widths.push_back(layer.RootDiameter * (1.0f - (0.8f * along)));
                            direction = glm::normalize(direction + (comb * 0.2f) - (frame.Up * layer.Droop));
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

        // WIDTHS CARRY COVERAGE. A real horse has thousands of hairs per cm²;
        // these coats have a few, so each strand stands for a lock and is
        // authored several times thicker than one hair. At a real 0.1-0.3 mm
        // the long coat was a few percent coverage and read as bare skin: the
        // renderer drew exactly what it was given (visual-quality-criteria.md,
        // rule 2). The human hair is near a real scalp's density and keeps a
        // near-real width.

        // A bay horse in its summer coat: short, sleek, with a dark mane and
        // tail. The colour map (the horse's own albedo) supplies the bay, the
        // blaze and the white socks.
        [[nodiscard]] CoatRecipe ShortCoatRecipe()
        {
            const glm::vec3 dark(0.22f, 0.17f, 0.14f);
            const glm::vec3 one(1.0f);
            CoatRecipe r;
            r.Name = "HorseShortCoat";
            r.Seed = 11;
            using R = Region;
            using Role = GroomCoatRole;
            r.Layers = {
                { R::Body, Role::Undercoat, 2250.0f, 0.008f, 7.8e-04f, 0.30f, 0.02f, 4, one, 0.0f, 0.2f },
                { R::Body, Role::GuardHair, 1000.0f, 0.016f, 9.8e-04f, 0.20f, 0.04f, 5, one, 0.2f, 0.2f },
                { R::Neck, Role::Undercoat, 2250.0f, 0.009f, 7.8e-04f, 0.30f, 0.02f, 4, one, 0.0f, 0.2f },
                { R::Neck, Role::GuardHair, 1000.0f, 0.018f, 9.8e-04f, 0.20f, 0.04f, 5, one, 0.2f, 0.2f },
                { R::Head, Role::Undercoat, 2250.0f, 0.005f, 6.9e-04f, 0.25f, 0.01f, 4, one, 0.0f, 0.2f },
                { R::Head, Role::GuardHair, 750.0f, 0.010f, 8.8e-04f, 0.20f, 0.02f, 4, one, 0.2f, 0.2f },
                { R::Leg, Role::Undercoat, 2250.0f, 0.006f, 6.9e-04f, 0.25f, 0.02f, 4, one, 0.0f, 0.2f },
                { R::Leg, Role::GuardHair, 750.0f, 0.012f, 8.8e-04f, 0.20f, 0.04f, 4, one, 0.2f, 0.2f },
                { R::LowerLeg, Role::GuardHair, 3000.0f, 0.012f, 6.3e-04f, 0.20f, 0.05f, 4, one, 0.2f, 0.2f },
                { R::Mane, Role::LongHair, 45000.0f, 0.36f, 9.8e-04f, 0.08f, 0.30f, 14, dark, 0.7f, 0.12f, 0.35f, 0.0f },
                { R::Forelock, Role::LongHair, 30000.0f, 0.20f, 9.8e-04f, 0.40f, 0.22f, 10, dark, 0.6f, 0.12f },
                { R::Tail, Role::LongHair, 200000.0f, 0.85f, 9.8e-04f, 0.25f, 0.45f, 18, dark, 0.6f, 0.10f },
            };
            return r;
        }

        // The same horse in a long winter coat: a dense undercoat under long
        // guard hair, feathering on the lower legs, a heavier mane and tail.
        // What #1251's two-layer coat is for.
        [[nodiscard]] CoatRecipe LongCoatRecipe()
        {
            const glm::vec3 dark(0.30f, 0.24f, 0.20f);
            const glm::vec3 one(1.0f);
            CoatRecipe r;
            r.Name = "HorseLongCoat";
            r.Seed = 23;
            using R = Region;
            using Role = GroomCoatRole;
            r.Layers = {
                { R::Body, Role::Undercoat, 3500.0f, 0.030f, 2.0e-03f, 0.55f, 0.05f, 5, one, 0.3f, 0.3f },
                { R::Body, Role::GuardHair, 1500.0f, 0.075f, 2.8e-03f, 0.35f, 0.12f, 8, one, 0.5f, 0.3f },
                { R::Neck, Role::Undercoat, 3500.0f, 0.032f, 2.0e-03f, 0.55f, 0.05f, 5, one, 0.3f, 0.3f },
                { R::Neck, Role::GuardHair, 1500.0f, 0.085f, 2.8e-03f, 0.35f, 0.12f, 8, one, 0.5f, 0.3f },
                { R::Head, Role::Undercoat, 3000.0f, 0.015f, 1.5e-03f, 0.45f, 0.03f, 4, one, 0.2f, 0.3f },
                { R::Head, Role::GuardHair, 1250.0f, 0.035f, 2.1e-03f, 0.35f, 0.06f, 6, one, 0.3f, 0.3f },
                { R::Leg, Role::Undercoat, 3000.0f, 0.020f, 1.5e-03f, 0.45f, 0.05f, 4, one, 0.2f, 0.3f },
                { R::Leg, Role::GuardHair, 1250.0f, 0.050f, 2.1e-03f, 0.35f, 0.12f, 6, one, 0.3f, 0.3f },
                { R::LowerLeg, Role::LongHair, 15000.0f, 0.09f, 1.4e-03f, 0.35f, 0.22f, 8, glm::vec3(0.9f), 0.5f, 0.2f },
                { R::Mane, Role::LongHair, 55000.0f, 0.45f, 1.1e-03f, 0.08f, 0.30f, 16, dark, 0.7f, 0.12f, 0.35f, 0.0f },
                { R::Forelock, Role::LongHair, 35000.0f, 0.25f, 1.1e-03f, 0.40f, 0.22f, 12, dark, 0.6f, 0.12f },
                { R::Tail, Role::LongHair, 225000.0f, 0.95f, 1.1e-03f, 0.25f, 0.45f, 20, dark, 0.6f, 0.10f },
            };
            return r;
        }

        // Straight hair to the jaw, dark brown. ~0.9 strands per mm² is about
        // half a real scalp's density, so the width stays near a real hair's.
        [[nodiscard]] CoatRecipe HumanHairRecipe()
        {
            CoatRecipe r;
            r.Name = "HumanHair";
            r.Seed = 37;
            r.GuideEvery = 16;
            r.Layers = {
                { Region::Scalp, GroomCoatRole::LongHair, 600000.0f, 0.20f, 1.6e-4f, 0.12f, 0.35f, 14, glm::vec3(1.0f),
                  0.35f, 0.06f, 0.10f, 0.35f },
            };
            return r;
        }
    } // namespace

    class GroomAnimalsAcceptanceEvidenceTest : public RendererAttachedTest
    {
      public:
        // One animated body with one coat bound to it.
        struct SubjectRig
        {
            std::string Name;
            std::string Tag; // the filename part: ShortCoat / LongCoat / Human
            Entity Body;
            Entity Coat;
            Ref<AnimatedModel> Model;
            GrownCoat Grown;
            AssetHandle GroomHandle = 0;
            AssetHandle BindingHandle = 0;
            Ref<GroomBindingAsset> Binding;
            BodyFrame Frame;
            glm::vec3 Position{ 0.0f };
            f32 ClipStart = 0.0f;
            u32 CardCount = 0;
        };

        SubjectRig m_ShortCoat;
        SubjectRig m_LongCoat;
        SubjectRig m_Human;
        AssetHandle m_HorseColorMap = 0;
        fs::path m_ProjectDir;

        static constexpr glm::vec3 kShortCoatPosition{ -1.4f, 0.0f, 0.0f };
        static constexpr glm::vec3 kLongCoatPosition{ 1.4f, 0.0f, 0.0f };
        static constexpr glm::vec3 kHumanPosition{ 0.0f, 1.24f, 3.6f };

        // A pixel "differs" past this, on any channel: the floor the other
        // groom evidence tests use, so their numbers compare.
        static constexpr int kDiffThreshold = 12;

        [[nodiscard]] static fs::path HeadPath()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "assets/models/InfiniteScanHead/HeadRigged.gltf";
        }

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
                            "  Name: GroomAnimalsEvidence\n"
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
            // Off: nothing here is a debug view, and the light and camera
            // gizmos are not gated by ShowComponentGizmos.
            rs.EditorDebugDrawsEnabled = false;
            rs.ShowComponentGizmos = false;
            rs.ShowGrid = false;
            rs.ShowWorldAxisHelper = false;
            rs.AnimalSchedulingEnabled = false; // the herd case turns it on explicitly
            SetTaa(0.9f);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.75f, -0.5f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }
            {
                // A primary runtime camera is what makes RunFrames render at
                // all, and therefore what steps the animation and the solver.
                // It stands close enough that the moving coats are on the
                // STRAND tier: the LOD ladder answers to the camera that renders
                // the motion, and a runtime camera parked 60 m away put both
                // horses on cards for every simulated frame. (At 7.5 m they were
                // still on cards: the engine's default card threshold is 256 px.)
                Entity cameraEntity = scene.CreateEntity("RuntimeCamera");
                auto& cameraTransform = cameraEntity.GetComponent<TransformComponent>();
                cameraTransform.Translation = glm::vec3(0.0f, 1.6f, 5.5f);
                cameraTransform.SetRotationEuler(glm::vec3(-0.12f, 0.0f, 0.0f));
                auto& cc = cameraEntity.AddComponent<CameraComponent>();
                cc.Primary = true;
                // PERSPECTIVE, explicitly: a CameraComponent defaults to
                // orthographic. The first version of this fixture left it there,
                // so every simulated frame -- and every LOD decision taken on one
                // -- saw an orthographic camera, and the live scene exported from
                // it opened to an empty viewport.
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
                ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(0.23f, 0.21f, 0.18f, 1.0f));
            }
            {
                // The human bust stands on a plinth at head height.
                Entity plinth = scene.CreateEntity("Plinth");
                auto& tc = plinth.GetComponent<TransformComponent>();
                tc.Translation = kHumanPosition - glm::vec3(0.0f, 0.62f, 0.0f);
                tc.Scale = glm::vec3(0.5f, 1.24f, 0.5f);
                plinth.AddComponent<MeshComponent>(MeshPrimitives::CreateCube()->GetMeshSource()).m_Primitive =
                    MeshPrimitive::Cube;
                plinth.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(0.35f, 0.34f, 0.33f, 1.0f));
            }

            Ref<Texture2D> colorMap = Texture2D::Create(HorseAlbedoPath().string(), false);
            ASSERT_TRUE(colorMap);
            m_HorseColorMap = AssetManager::AddMemoryOnlyAsset<Texture2D>(colorMap);

            InstallSubject(m_ShortCoat, "ShortCoatHorse", "ShortCoat", HorsePath(), kShortCoatPosition, "Walk", 0.0f,
                           Subject::Horse, ShortCoatRecipe());
            ASSERT_FALSE(HasFatalFailure());
            InstallSubject(m_LongCoat, "LongCoatHorse", "LongCoat", HorsePath(), kLongCoatPosition, "Walk", 0.55f,
                           Subject::Horse, LongCoatRecipe());
            ASSERT_FALSE(HasFatalFailure());
            InstallSubject(m_Human, "Human", "Human", HeadPath(), kHumanPosition, "LookAround", 0.0f,
                           Subject::HumanHead, HumanHairRecipe());
            ASSERT_FALSE(HasFatalFailure());
        }

        void InstallSubject(SubjectRig& out, const std::string& name, const std::string& tag, const fs::path& modelPath,
                            const glm::vec3& position, const char* clip, f32 clipTime, Subject kind,
                            const CoatRecipe& recipe)
        {
            Scene& scene = GetScene();
            out.Name = name;
            out.Tag = tag;
            out.Position = position;
            out.ClipStart = clipTime;
            ASSERT_TRUE(fs::exists(modelPath)) << modelPath.string();
            out.Model = Ref<AnimatedModel>::Create(modelPath.string());
            ASSERT_TRUE(out.Model);
            ASSERT_FALSE(out.Model->GetMeshes().empty()) << modelPath.string();
            ASSERT_TRUE(out.Model->HasSkeleton()) << modelPath.string();

            out.Body = MakeBody(name, out.Model, modelPath, position, clip, clipTime);
            ASSERT_FALSE(HasFatalFailure());

            const Ref<MeshSource> surface = out.Body.GetComponent<MeshComponent>().m_MeshSource;
            ASSERT_TRUE(surface);
            const Skeleton* skeleton = out.Body.GetComponent<SkeletonComponent>().m_Skeleton.Raw();
            ASSERT_NE(skeleton, nullptr);

            // The coat entity sits at the world origin with an identity
            // transform, so the groom's space IS world space and the binding
            // maps the body's object space into it.
            const glm::mat4 bodyWorld = out.Body.GetComponent<TransformComponent>().GetTransform();
            const std::vector<SurfaceTriangle> tris = CollectSurface(*surface, *skeleton, bodyWorld, out.Frame, kind);
            out.Grown = GrowCoat(tris, out.Frame, recipe);
            ASSERT_TRUE(out.Grown.Groom);

            // THE COOKED CARD LEVEL (#1252), through the real builder, so what
            // draws at card range is the bytes the cook writes. The horse's
            // root UVs are its albedo atlas, inside [0,1] and therefore inside
            // the builder's +-16 clustering domain.
            if (kind == Subject::Horse)
            {
                GroomCardSettings cardSettings;
                cardSettings.CellSize = 0.012f;
                cardSettings.PointsPerCard = 6;
                GroomLodLevel level;
                GroomCardBuildStats cardStats;
                std::string reason;
                ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*out.Grown.Groom, cardSettings, level, reason, &cardStats))
                    << reason;
                out.CardCount = level.GetCurveCount();
                ASSERT_TRUE(GroomLodBuilder::AttachLodLevels(*out.Grown.Groom, { std::move(level) }, reason)) << reason;
            }

            std::printf("[groom-animals] %s: %u strands, %u guides, %u cards;", name.c_str(), out.Grown.Strands,
                        out.Grown.Guides, out.CardCount);
            for (sizet r = 0; r < out.Grown.PerRegion.size(); ++r)
            {
                if (out.Grown.PerRegion[r] > 0)
                {
                    std::printf(" %s=%u", RegionName(static_cast<Region>(r)), out.Grown.PerRegion[r]);
                }
            }
            std::printf("\n");
            std::fflush(stdout);

            GroomBindingBuildSettings bind;
            bind.SurfaceToGroom = bodyWorld;
            std::vector<u8> bytes;
            Ref<GroomBindingAsset> binding;
            GroomBindingBuildStats stats;
            std::string reason;
            ASSERT_TRUE(GroomBindingCooker::CookPair(*out.Grown.Groom, MakeSurfaceView(*surface, skeleton), name.c_str(),
                                                     bind, bytes, binding, stats, reason))
                << reason;
            ASSERT_TRUE(GroomBindingSerializer::DecodeFromBytes(bytes.data(), bytes.size(), out.Binding, reason))
                << reason;

            out.GroomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(out.Grown.Groom);
            out.BindingHandle = AssetManager::AddMemoryOnlyAsset<GroomBindingAsset>(out.Binding);

            out.Coat = MakeCoat(name + "Coat", out.GroomHandle, out.BindingHandle, out.Body, kind, glm::vec3(0.0f),
                                out.Grown.Strands);
        }

        Entity MakeBody(const std::string& name, const Ref<AnimatedModel>& model, const fs::path& modelPath,
                        const glm::vec3& position, const char* clip, f32 clipTime)
        {
            Entity body = GetScene().CreateEntity(name);
            body.GetComponent<TransformComponent>().Translation = position;
            (void)ModelImporter::PopulateAnimatedEntity(body, model, modelPath.string(), true);
            EXPECT_TRUE(body.HasComponent<MeshComponent>());
            EXPECT_TRUE(body.HasComponent<SkeletonComponent>());
            EXPECT_TRUE(body.HasComponent<AnimationStateComponent>());
            if (body.HasComponent<AnimationStateComponent>())
            {
                auto& anim = body.GetComponent<AnimationStateComponent>();
                bool found = false;
                for (sizet i = 0; i < anim.m_AvailableClips.size(); ++i)
                {
                    if (anim.m_AvailableClips[i]->Name == clip)
                    {
                        anim.m_CurrentClipIndex = static_cast<int>(i);
                        anim.m_CurrentClip = anim.m_AvailableClips[i];
                        found = true;
                    }
                }
                EXPECT_TRUE(found) << name << " has no clip '" << clip << "'";
                anim.m_CurrentTime = clipTime;
                anim.m_IsPlaying = true;
            }
            return body;
        }

        // Every child's component, as a shipping scene would author a coat.
        Entity MakeCoat(const std::string& name, AssetHandle groom, AssetHandle binding, Entity body, Subject kind,
                        const glm::vec3& offset, u32 strands)
        {
            Entity coatEntity = GetScene().CreateEntity(name);
            coatEntity.GetComponent<TransformComponent>().Translation = offset;

            auto& gc = coatEntity.AddComponent<GroomComponent>();
            gc.m_Groom = groom;
            gc.m_ShowPreview = false;
            gc.m_RenderStrands = true;
            gc.m_MaxRenderStrands = strands;
            gc.m_CompositionMode = static_cast<u8>(GroomCompositionMode::StochasticAlpha); // #1246's selected mode
            gc.m_StrandColor = glm::vec3(1.0f);

            auto& fibre = coatEntity.AddComponent<GroomFibreComponent>(); // #1247
            auto& coat = coatEntity.AddComponent<GroomCoatComponent>();   // #1251
            coat.m_LengthJitter = 0.2f;
            coat.m_ShadeJitter = 0.1f;
            if (kind == Subject::Horse)
            {
                fibre.m_PigmentMode = static_cast<u8>(GroomFibrePigmentMode::BaseColor);
                fibre.m_BaseColor = glm::vec3(0.92f, 0.9f, 0.88f);
                fibre.m_Intensity = 6.0f;
                coat.m_ColorMap = RegionDebugEnabled() ? AssetHandle(0) : m_HorseColorMap;
            }
            else
            {
                // Dark brown. At eumelanin 1.6 and intensity 6 the unshadowed
                // coat washed out to grey under the key light.
                fibre.m_PigmentMode = static_cast<u8>(GroomFibrePigmentMode::Melanin);
                fibre.m_Eumelanin = 3.0f;
                fibre.m_Pheomelanin = 0.25f;
                fibre.m_Intensity = 3.0f;
            }

            // #1248, REQUESTED on a bound coat. Since #1426 the pass bakes a
            // deformed groom's volume from the pose it draws, so every moving
            // coat here is shadowed -- asserted in the integration case, with
            // the refusal count.
            auto& coatShadow = coatEntity.AddComponent<GroomCoatShadowComponent>();
            coatShadow.m_Enabled = true;

            auto& lod = coatEntity.AddComponent<GroomLodComponent>(); // #1252, engine defaults
            lod.m_Enabled = true;

            auto& bc = coatEntity.AddComponent<GroomBindingComponent>(); // #1249
            bc.m_Binding = binding;
            bc.m_TargetEntity = body.GetUUID();
            bc.m_Enabled = true;

            auto& sim = coatEntity.AddComponent<GroomSimulationComponent>(); // #1250
            sim.m_Enabled = true;
            sim.m_Collide = true;
            return coatEntity;
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

        static void SetTaa(f32 feedback)
        {
            auto& post = Renderer3D::GetPostProcessSettings();
            post.TAAEnabled = true;
            post.TAAFeedback = feedback;
            post.TAASharpness = 0.0f;
        }

        static void ColdHistory()
        {
            Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::SceneReset);
        }

        [[nodiscard]] std::array<SubjectRig*, 3> Subjects()
        {
            return { &m_ShortCoat, &m_LongCoat, &m_Human };
        }

        // What a run of runtime frames did, accumulated over the WHOLE run
        // (a capture is a zero-step editor frame, so per-step counters read
        // after one are zero).
        struct MotionResult
        {
            GroomRenderStats Last;
            f32 WorstStretch = 1.0f;
            u32 ContactsEver = 0;
            u32 MaxHeldAtRest = 0;
            u32 MaxHistoryRejectedAfterFirst = 0;
            u32 MinGroomsDeformed = 0xFFFFFFFFu;
            u32 MaxBindingRefused = 0;
            u32 RepresentationChanges = 0;
        };

        // Every subject back to the same point of its clip, every solver
        // re-seeded, then `frames` runtime frames at 60 Hz. Two arms of an A/B
        // that both start here see the identical pose sequence.
        // A REPLAY MUST BE A FUNCTION OF THE CLIP START ALONE, and rewinding the
        // clip is not enough to make it one. After a rewind the skeleton has no
        // bone history until its animation has ticked twice, and the binding
        // withholds history until then, so the guide solver RESEEDS on every
        // history-less frame. How many there are depends on what ran BEFORE the
        // rewind: one when the clip was already ticking (a capture's settle
        // frames), two when it was not (a fresh fixture, or whatever the
        // previous test left). A replay that reseeded once integrated one step
        // more than one that reseeded twice, and the coat diverged from there.
        //
        // Measured with a per-frame solver trace: replay A reseeded on frames 0
        // and 1, replay B on frame 0 alone, with identical palettes, surfaces
        // and root transforms. That one step was the whole "repeat floor"
        // residue TheMovingCoatIsSelfShadowedThroughTheWalk failed on: its luma
        // drift grew from 6.5k alone to 26k after the rest of the suite.
        //
        // So every replay holds the solver in reseed for the first TWO frames,
        // the longest a rewind can withhold history, and integrates from the
        // same frame whatever came before.
        MotionResult PlayFromStart(u32 frames)
        {
            const auto reseedAll = [this]()
            {
                for (SubjectRig* s : Subjects())
                {
                    ++s->Coat.GetComponent<GroomSimulationComponent>().m_ResetKey;
                }
            };
            for (SubjectRig* s : Subjects())
            {
                auto& anim = s->Body.GetComponent<AnimationStateComponent>();
                anim.m_CurrentTime = s->ClipStart;
                anim.m_IsPlaying = true;
            }
            reseedAll();
            if (frames == 0u)
            {
                return Play(0u);
            }
            MotionResult first = Play(1u);
            reseedAll();
            MotionResult rest = Play(frames - 1u);
            // The per-frame maxima and minima fold across the split, so the
            // result means what it meant when this was one Play(frames).
            rest.ContactsEver += first.ContactsEver;
            rest.MaxHeldAtRest = std::max(rest.MaxHeldAtRest, first.MaxHeldAtRest);
            rest.MinGroomsDeformed = std::min(rest.MinGroomsDeformed, first.MinGroomsDeformed);
            rest.MaxBindingRefused = std::max(rest.MaxBindingRefused, first.MaxBindingRefused);
            rest.RepresentationChanges += first.RepresentationChanges;
            if (std::abs(first.WorstStretch - 1.0f) > std::abs(rest.WorstStretch - 1.0f))
            {
                rest.WorstStretch = first.WorstStretch;
            }
            // MaxHistoryRejectedAfterFirst needs no fold: Play excludes the
            // first frames of a run, and `first` is one frame long.
            return rest;
        }

        MotionResult Play(u32 frames)
        {
            MotionResult r;
            for (u32 f = 0; f < frames; ++f)
            {
                RunFrames(1, 1.0f / 60.0f);
                r.Last = PassStats();
                r.ContactsEver += r.Last.SimulationContacts;
                r.MaxHeldAtRest = std::max(r.MaxHeldAtRest, r.Last.RootsHeldAtRest);
                r.MinGroomsDeformed = std::min(r.MinGroomsDeformed, r.Last.GroomsDeformed);
                r.MaxBindingRefused = std::max(r.MaxBindingRefused, r.Last.GroomsBindingRefused);
                // Two legitimate discontinuities are excluded, both documented
                // in groom-surface-binding.md ("both history gates have to
                // hold"): the first two frames of a run, where the skeleton's
                // own HasBoneHistory() is still false because the animation has
                // not ticked twice yet, and a representation hand-over. What
                // must not happen is a rejection on a frame where nothing
                // changed.
                if (f > 1 && r.Last.Lod.RepresentationChanges == 0)
                {
                    r.MaxHistoryRejectedAfterFirst = std::max(r.MaxHistoryRejectedAfterFirst, r.Last.GroomsHistoryRejected);
                }
                r.RepresentationChanges += r.Last.Lod.RepresentationChanges;
                if (std::abs(r.Last.WorstStretchRatio - 1.0f) > std::abs(r.WorstStretch - 1.0f))
                {
                    r.WorstStretch = r.Last.WorstStretchRatio;
                }
            }
            return r;
        }

        struct View
        {
            glm::vec3 Eye;
            glm::vec3 Target;
            f32 Fov = 40.0f;
        };

        [[nodiscard]] View ViewOf(const SubjectRig& s) const
        {
            if (&s == &m_ShortCoat)
            {
                return { s.Position + glm::vec3(-4.6f, 1.4f, 0.4f), s.Position + glm::vec3(0.0f, 1.15f, 0.0f), 40.0f };
            }
            if (&s == &m_LongCoat)
            {
                return { s.Position + glm::vec3(4.6f, 1.4f, 0.4f), s.Position + glm::vec3(0.0f, 1.15f, 0.0f), 40.0f };
            }
            return { s.Position + glm::vec3(0.30f, 0.34f, 0.95f), s.Position + glm::vec3(0.0f, 0.33f, 0.0f), 32.0f };
        }

        // Look at `target` from `eye`, converge TAA, read back, write the PNG
        // (unless `name` is empty).
        void Capture(const std::string& name, const View& view, std::vector<u8>& out, u32 settleFrames = 24)
        {
            const glm::vec3 d = glm::normalize(view.Target - view.Eye);
            const f32 yaw = std::atan2(d.x, -d.z);
            const f32 pitch = std::asin(std::clamp(-d.y, -1.0f, 1.0f));
            EditorCamera camera(view.Fov, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 400.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(view.Eye, yaw, pitch);
            // StochasticAlpha is converged by TAA, so a still camera needs
            // enough frames for the history to settle. Editor frames advance
            // no animation and no simulation: every frame here is one pose.
            RunEditorFrames(camera, settleFrames);
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            ASSERT_TRUE(fb);
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
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

        [[nodiscard]] static u32 CountDiffering(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size())
            {
                return 0;
            }
            u32 n = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                const int dr = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
                const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
                const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                if (dr > kDiffThreshold || dg > kDiffThreshold || db > kDiffThreshold)
                {
                    ++n;
                }
            }
            return n;
        }

        // Rec. 709 luma summed over the frame, in 8-bit units.
        [[nodiscard]] static f64 SummedLuma(const std::vector<u8>& rgba)
        {
            f64 sum = 0.0;
            for (sizet i = 0; i + 3 < rgba.size(); i += 4)
            {
                sum += 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
            }
            return sum;
        }

        [[nodiscard]] static f64 Fraction(u32 pixels)
        {
            return static_cast<f64>(pixels) / static_cast<f64>(kWidth * kHeight);
        }

        // The coat's own pixels in a view: the frame with every coat drawn
        // against the same frame with THIS coat hidden. A coverage floor on
        // this is what a difference-only assertion cannot give: an empty
        // frame differs from nothing.
        u32 CoatPixels(SubjectRig& s, const View& view, const std::string& onName, const std::string& offName,
                       std::vector<u8>* outOn = nullptr)
        {
            // OFF first, so the last frame drawn -- the one PassStats() and
            // the request list describe -- is the coat-on frame.
            std::vector<u8> on, off;
            s.Coat.GetComponent<GroomComponent>().m_RenderStrands = false;
            ColdHistory();
            Capture(offName, view, off);
            s.Coat.GetComponent<GroomComponent>().m_RenderStrands = true;
            ColdHistory();
            Capture(onName, view, on);
            if (outOn != nullptr)
            {
                *outOn = std::move(on);
                return CountDiffering(*outOn, off);
            }
            return CountDiffering(on, off);
        }

        // Nearest-neighbour enlargement of a region (visual-quality-criteria
        // rule 1: a defect invisible at 1280x720 is obvious at 3x).
        static void WriteEnlargedCrop(const std::string& name, const std::vector<u8>& rgba, u32 x0, u32 y0, u32 w, u32 h,
                                      u32 scale)
        {
            std::vector<u8> out(static_cast<sizet>(w) * scale * h * scale * 4u);
            for (u32 y = 0; y < h * scale; ++y)
            {
                for (u32 x = 0; x < w * scale; ++x)
                {
                    const u32 sx = std::min(x0 + (x / scale), kWidth - 1u);
                    const u32 sy = std::min(y0 + (y / scale), kHeight - 1u);
                    std::memcpy(&out[(static_cast<sizet>(y) * w * scale + x) * 4u],
                                &rgba[(static_cast<sizet>(sy) * kWidth + sx) * 4u], 4u);
                }
            }
            WritePng(name, out, w * scale, h * scale);
        }

        [[nodiscard]] const GroomStrandRequest* RequestFor(const SubjectRig& s) const
        {
            for (const auto& req : Renderer3D::GetGroomStrandRequests())
            {
                if (req.EntityID == static_cast<i32>(static_cast<u32>(s.Coat)))
                {
                    return &req;
                }
            }
            return nullptr;
        }
    };

    // =========================================================================
    // Criterion 1 -- every child integrates on moving short- and long-coated
    // animals and a human hair asset, on every GL raster path.
    // =========================================================================
    TEST_F(GroomAnimalsAcceptanceEvidenceTest, EveryChildIntegratesOnTheMovingSubjectsOnEveryPath)
    {
        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        const PathCase paths[] = { { "Forward", RenderingPath::Forward },
                                   { "ForwardPlus", RenderingPath::ForwardPlus },
                                   { "Deferred", RenderingPath::Deferred } };

        for (const PathCase& path : paths)
        {
            SCOPED_TRACE(path.Name);
            SetPath(path.Path);
            const MotionResult motion = PlayFromStart(60);
            const GroomRenderStats& s = motion.Last;
            std::printf("[groom-animals] %s: drawn=%u strands=%u deformed=%u refused=%u simulated=%u guides=%u "
                        "contacts=%llu stretch=%.4f (declared %.4f) lit=%u historyRejectedAfterFirst=%u "
                        "coatShadow{shadowed=%u poseUnavailable=%u stale=%u rebakes=%u drift=%.2f} "
                        "lod{strand=%u card=%u changes=%u}\n",
                        path.Name, s.GroomsDrawn, s.StrandsDrawn, s.GroomsDeformed, s.GroomsBindingRefused,
                        s.GroomsSimulated, s.GuidesSimulated, static_cast<unsigned long long>(motion.ContactsEver),
                        static_cast<f64>(motion.WorstStretch), static_cast<f64>(s.DeclaredStretchTolerance), s.GroomsLit,
                        motion.MaxHistoryRejectedAfterFirst, s.CoatShadow.ShadowedGrooms,
                        s.CoatShadow.ByReason[static_cast<sizet>(GroomCoatShadowFallbackReason::DeformedPoseUnavailable)],
                        s.CoatShadow.ByReason[static_cast<sizet>(GroomCoatShadowFallbackReason::RepresentationStale)],
                        s.CoatShadow.DeformedRebakes, static_cast<f64>(s.CoatShadow.MaxDriftVoxels),
                        s.Lod.ByRepresentation[static_cast<sizet>(GroomRepresentation::Strand)],
                        s.Lod.ByRepresentation[static_cast<sizet>(GroomRepresentation::Card)],
                        motion.RepresentationChanges);
            std::fflush(stdout);

            // The census, taken off the pass's own counters for the frame it drew.
            EXPECT_EQ(s.GroomsDrawn, 3u) << "all three coats draw";
            EXPECT_EQ(motion.MinGroomsDeformed, 3u) << "#1249: every coat follows its body on EVERY frame";
            EXPECT_EQ(motion.MaxBindingRefused, 0u) << "#1249: no binding refused";
            EXPECT_EQ(motion.MaxHeldAtRest, 0u) << "#1249: no root held at the bind pose";
            EXPECT_EQ(s.GroomsSimulated, 3u) << "#1250: every coat's guides are simulated";
            EXPECT_GT(motion.ContactsEver, 0u) << "#1250: the body proxy catches the coat";
            EXPECT_LE(std::abs(motion.WorstStretch - 1.0f), std::abs(s.DeclaredStretchTolerance - 1.0f) + 1.0e-4f)
                << "#1250: length is preserved through the whole run";
            EXPECT_EQ(s.GroomsLit, 3u) << "#1247: every coat is lit by the fibre model";
            EXPECT_EQ(motion.MaxHistoryRejectedAfterFirst, 0u)
                << "#1249/#1256: a continuous walk must not drop a coat's motion history";

            // #1251: the coats carry their roles and the horses their colour map.
            for (SubjectRig* subject : Subjects())
            {
                const GroomStrandRequest* req = RequestFor(*subject);
                ASSERT_NE(req, nullptr) << subject->Name;
                EXPECT_TRUE(req->Lit) << subject->Name;
                EXPECT_EQ(req->BindingReject, GroomBindingRejectReason::None) << subject->Name;
                if (subject != &m_Human)
                {
                    EXPECT_TRUE(req->Coat.ColorMap) << subject->Name << ": the albedo colour map resolved";
                }
            }

            // #1248 on a moving body (#1426): every bound coat is baked from
            // the pose it is drawn at, so every one is shadowed and none is
            // refused -- neither for want of a pose nor for a stale volume.
            EXPECT_EQ(s.CoatShadow.ShadowedGrooms, 3u) << "#1426: every moving coat is self-shadowed";
            EXPECT_EQ(s.CoatShadow.FallbackGrooms, 0u);
            EXPECT_EQ(
                s.CoatShadow.ByReason[static_cast<sizet>(GroomCoatShadowFallbackReason::DeformedPoseUnavailable)], 0u)
                << "#1426: a bound coat is no longer refused its volume";
            EXPECT_EQ(s.CoatShadow.ByReason[static_cast<sizet>(GroomCoatShadowFallbackReason::RepresentationStale)],
                      0u)
                << "#1426: the rebake keeps up with the walk";

            // Coverage floors, per subject: each coat is visibly there.
            for (SubjectRig* subject : Subjects())
            {
                const std::string on = "GroomAnimals_GL_" + std::string(path.Name) + "_" + subject->Tag;
                const std::string off = "GroomAnimalsOff_GL_" + std::string(path.Name) + "_" + subject->Tag;
                const u32 coat = CoatPixels(*subject, ViewOf(*subject), on, off);
                ASSERT_FALSE(HasFatalFailure());
                std::printf("[groom-animals] %s %s: coat covers %u px (%.2f%% of the frame)\n", path.Name,
                            subject->Tag.c_str(), coat, 100.0 * Fraction(coat));
                std::fflush(stdout);
                EXPECT_GT(Fraction(coat), 0.01) << subject->Tag << " on " << path.Name
                                                << ": the coat must cover at least 1% of a frame framed on it";
            }
        }

        std::vector<u8> overview;
        SetPath(RenderingPath::Deferred);
        ColdHistory();
        Capture("GroomAnimals_GL_Deferred_Overview", { glm::vec3(5.5f, 2.3f, 9.0f), glm::vec3(0.0f, 1.1f, 0.8f), 45.0f },
                overview);
    }

    // =========================================================================
    // #1429: a freshly opened groom scene is not bald. GroomAnimals.olo does not
    // carry TAA (nothing in a scene file does), every coat in it asks for
    // StochasticAlpha, and that mode's no-resolve fallback draws no sub-pixel
    // hair -- so before the fix the editor opened on bald horses and a hairless
    // human until someone ticked TAA. The scene now REQUESTS the resolve.
    //
    // Two arms per subject, TAA unticked in both:
    //   SceneResolve     the request honoured (the default): the coat is there
    //   SceneResolveOff  the request refused by the diagnostic switch: the
    //                    issue's own frame, reproduced -- the instrument check
    //                    that the first arm's coverage is the request's doing
    // =========================================================================
    TEST_F(GroomAnimalsAcceptanceEvidenceTest, AFreshSceneWithTAAOffStillCoatsItsSubjects)
    {
        auto& post = Renderer3D::GetPostProcessSettings();
        auto& rs = Renderer3D::GetRendererSettings();
        post.TAAEnabled = false; // what a scene loaded from disk gets
        (void)PlayFromStart(30);
        // FREEZE the clips. The edit-mode preview advances any clip that is
        // playing, so without this every 24-frame capture lands on a different
        // stride, and the coat-on minus coat-off difference counts the legs
        // moving as coat: the first run of this case measured the refused arm
        // at 85-125% of the honoured one while its PNG showed a bald horse.
        for (SubjectRig* subject : Subjects())
        {
            subject->Body.GetComponent<AnimationStateComponent>().m_IsPlaying = false;
        }

        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        const PathCase paths[] = { { "Forward", RenderingPath::Forward },
                                   { "ForwardPlus", RenderingPath::ForwardPlus },
                                   { "Deferred", RenderingPath::Deferred } };
        constexpr auto kNoResolve = static_cast<sizet>(GroomCompositionFallbackReason::TemporalResolveUnavailable);

        for (const PathCase& path : paths)
        {
            SCOPED_TRACE(path.Name);
            SetPath(path.Path);
            for (SubjectRig* subject : Subjects())
            {
                SCOPED_TRACE(subject->Tag);
                const std::string suffix = "_GL_" + std::string(path.Name) + "_" + subject->Tag;

                rs.HonourSceneTemporalResolveRequests = true;
                const u32 coated = CoatPixels(*subject, ViewOf(*subject), "GroomAnimalsSceneResolve" + suffix, "");
                ASSERT_FALSE(HasFatalFailure());
                const GroomCompositionStats honoured = PassStats().Composition;
                const u32 askedHonoured = Renderer3D::GetSceneTemporalResolveGrooms();
                const bool taaWanted = Renderer3D::IsEngineTAAWanted();

                rs.HonourSceneTemporalResolveRequests = false;
                const u32 bald = CoatPixels(*subject, ViewOf(*subject), "GroomAnimalsSceneResolveOff" + suffix, "");
                ASSERT_FALSE(HasFatalFailure());
                const GroomCompositionStats refused = PassStats().Composition;
                rs.HonourSceneTemporalResolveRequests = true;

                std::printf("[groom-animals] #1429 %s %s: TAA unticked, request honoured -> coat %u px (%.2f%%), "
                            "refused -> %u px (%.2f%%); fell back %u -> %u\n",
                            path.Name,
                            subject->Tag.c_str(), coated, 100.0 * Fraction(coated), bald,
                            100.0 * Fraction(bald), honoured.GroomsFellBack, refused.GroomsFellBack);
                std::fflush(stdout);

                // The chain, link by link, so a failure names the broken one.
                EXPECT_EQ(askedHonoured, 3u) << "every stochastic coat in the scene asks for the resolve";
                EXPECT_TRUE(taaWanted) << "an honoured request runs TAA with TAAEnabled off";
                EXPECT_EQ(honoured.GroomsFellBack, 0u) << "with the request honoured no coat is refused its mode";
                EXPECT_EQ(refused.ByReason[kNoResolve], 3u)
                    << "the control: refuse the request and every coat falls back for want of a resolve";

                // The pixels: the coverage floor the integration test above
                // holds with TAA ticked, now with it unticked...
                EXPECT_GT(Fraction(coated), 0.01) << "a freshly opened scene must show the coat";
                // ...and the control is the issue's bald frame, which is what
                // makes the floor above the request's doing and not the
                // fixture's.
                EXPECT_LT(bald * 5u, coated) << "the refused arm must reproduce the bald coat the issue reports";
            }
        }

        // MSAA 4 (deferred) and upscaling: the request must still produce a
        // resolve the groom accepts. FSR2 subsumes engine TAA and is a resolve
        // itself; FSR1 is not, so engine TAA runs under it. Framing under an
        // upscale is #1397's, so these cells assert the DECISION, not coverage.
        struct ResolveCase
        {
            const char* Name;
            RenderingPath Path;
            u32 Samples;
            UpscaleMode Upscale;
            UpscalerTechnique Technique;
        };
        const ResolveCase cells[] = {
            { "DeferredMSAA4", RenderingPath::Deferred, 4u, UpscaleMode::Off, UpscalerTechnique::Spatial },
            { "ForwardFSR1", RenderingPath::Forward, 1u, UpscaleMode::Quality, UpscalerTechnique::Spatial },
            { "ForwardFSR2", RenderingPath::Forward, 1u, UpscaleMode::Quality, UpscalerTechnique::Temporal },
        };
        for (const ResolveCase& cell : cells)
        {
            SCOPED_TRACE(cell.Name);
            rs.Deferred.MSAASampleCount = cell.Samples;
            post.Upscale = cell.Upscale;
            post.Technique = cell.Technique;
            SetPath(cell.Path);
            std::vector<u8> px;
            ColdHistory();
            Capture("", ViewOf(m_LongCoat), px);
            ASSERT_FALSE(HasFatalFailure());
            const GroomCompositionStats& stats = PassStats().Composition;
            std::printf("[groom-animals] #1429 %s: considered=%u fellBack=%u taaWanted=%d\n", cell.Name,
                        stats.GroomsConsidered,
                        stats.GroomsFellBack, Renderer3D::IsEngineTAAWanted() ? 1 : 0);
            std::fflush(stdout);
            EXPECT_GT(stats.GroomsConsidered, 0u);
            EXPECT_EQ(stats.GroomsFellBack, 0u) << "the requested resolve must survive " << cell.Name;
            // WHICH resolve, or the FSR2 cell could quietly be an FSR1 cell with
            // engine TAA (an unavailable FSR2 falls back to FSR1) and the "FSR2
            // subsumes the request" path would never run.
            if (cell.Technique == UpscalerTechnique::Temporal)
            {
                EXPECT_TRUE(Renderer3D::IsTemporalUpscaleActive()) << "FSR2 must own this frame";
            }
            else
            {
                EXPECT_FALSE(Renderer3D::IsTemporalUpscaleActive());
                EXPECT_TRUE(Renderer3D::IsEngineTAAWanted()) << "engine TAA runs on the scene's request";
            }
        }
        rs.Deferred.MSAASampleCount = 1u;
        post.Upscale = UpscaleMode::Off;
        post.Technique = UpscalerTechnique::Spatial;
    }

    // =========================================================================
    // Criterion 1, the negative controls: each active visual lever, switched
    // off on the moving long-coated animal, changes the frame. LOD is checked
    // by the near-to-far case instead.
    // =========================================================================
    TEST_F(GroomAnimalsAcceptanceEvidenceTest, ActiveVisualLeversChangeTheMovingLongCoat)
    {
        SetPath(RenderingPath::Forward);
        SubjectRig& s = m_LongCoat;
        const View view = ViewOf(s);
        constexpr u32 kMotion = 45;

        std::vector<u8> baseline;
        (void)PlayFromStart(kMotion);
        ColdHistory();
        Capture("GroomAnimalsControl_GL_Forward_LongCoat", view, baseline);
        ASSERT_FALSE(HasFatalFailure());

        // THE REPEAT FLOOR FIRST: the identical configuration run again. A
        // stochastic coat under TAA does not reproduce bit-for-bit (the hash
        // and the jitter advance per frame), so every lever is judged against
        // this measured floor, never against zero.
        (void)PlayFromStart(kMotion);
        std::vector<u8> again;
        ColdHistory();
        Capture("", view, again);
        const u32 noise = CountDiffering(again, baseline);
        std::printf("[groom-animals] repeat floor: %u px differ (%.3f%%)\n", noise, 100.0 * Fraction(noise));
        EXPECT_LT(Fraction(noise), 0.03) << "an identical re-run must reproduce the frame up to stochastic noise";

        struct Lever
        {
            const char* Child;
            const char* Name;
            std::function<void(bool)> Set; // true = the shipped state
            bool ExpectChange;
        };
        Entity coat = s.Coat;
        const std::vector<Lever> levers = {
            { "#1246", "Composition",
              [coat](bool on) mutable
              {
                  coat.GetComponent<GroomComponent>().m_CompositionMode = static_cast<u8>(
                      on ? GroomCompositionMode::StochasticAlpha : GroomCompositionMode::OpaqueRibbon);
              },
              true },
            { "#1247", "Fibre", [coat](bool on) mutable
              { coat.GetComponent<GroomFibreComponent>().m_Enabled = on; },
              true },
            // #1248 on a moving animal (#1426). Before, a bound coat was
            // refused its volume and this lever changed 2.32% of the frame
            // against a 2.23% repeat floor -- disconnected. It must now clear
            // the same bar as every other child.
            { "#1248", "CoatShadow",
              [coat](bool on) mutable
              { coat.GetComponent<GroomCoatShadowComponent>().m_Enabled = on; }, true },
            { "#1249", "Binding",
              [coat](bool on) mutable
              { coat.GetComponent<GroomBindingComponent>().m_Enabled = on; }, true },
            { "#1250", "Simulation",
              [coat](bool on) mutable
              { coat.GetComponent<GroomSimulationComponent>().m_Enabled = on; }, true },
            { "#1251", "CoatAuthoring",
              [coat](bool on) mutable
              { coat.GetComponent<GroomCoatComponent>().m_Enabled = on; }, true },
            // #1252 is not expected to change the picture much: its contract is
            // that thinning is COMPENSATED. Its evidence is the near-to-far case.
            { "#1252", "Lod", [coat](bool on) mutable
              { coat.GetComponent<GroomLodComponent>().m_Enabled = on; },
              false },
        };

        for (const Lever& lever : levers)
        {
            SCOPED_TRACE(lever.Name);
            lever.Set(false);
            (void)PlayFromStart(kMotion);
            std::vector<u8> off;
            ColdHistory();
            Capture("GroomAnimals" + std::string(lever.Name) + "Off_GL_Forward_LongCoat", view, off);
            lever.Set(true);
            ASSERT_FALSE(HasFatalFailure());
            const u32 changed = CountDiffering(off, baseline);
            std::printf("[groom-animals] lever %s %s off: %u px differ (%.2f%%, %.2fx the repeat floor)\n", lever.Child,
                        lever.Name, changed, 100.0 * Fraction(changed),
                        noise > 0u ? static_cast<f64>(changed) / static_cast<f64>(noise) : 0.0);
            std::fflush(stdout);
            if (lever.ExpectChange && std::string_view(lever.Child) == "#1248")
            {
                // A DIRECTIONAL term, judged by its direction (#1426). The
                // coat shadow only ever darkens, while the repeat floor is a
                // stochastic coat re-dithering in both directions -- so a pixel
                // count scores the two on the same scale and undersells the
                // one with a sign. The prediction is: switched off, the frame
                // is BRIGHTER, by several times the luma the identical re-run
                // drifts. The pixel count must still clear the floor itself.
                const f64 leverLuma = SummedLuma(off) - SummedLuma(baseline);
                const f64 repeatLuma = std::abs(SummedLuma(again) - SummedLuma(baseline));
                std::printf("[groom-animals] lever #1248 luma: off - on = %+.0f, repeat drift %.0f (%.1fx)\n",
                            leverLuma, repeatLuma, repeatLuma > 0.0 ? leverLuma / repeatLuma : 0.0);
                std::fflush(stdout);
                EXPECT_GT(changed, noise) << "#1248: the coat shadow changes the moving coat by less than the repeat "
                                             "floor";
                EXPECT_GT(leverLuma, 5.0 * repeatLuma)
                    << "#1248: switching the coat shadow off must BRIGHTEN the moving coat by clearly more than an "
                       "identical re-run drifts";
            }
            else if (lever.ExpectChange)
            {
                EXPECT_GT(changed, 2u * noise) << lever.Child << " " << lever.Name
                                               << ": switching the child off must change the frame past twice the "
                                                  "repeat floor";
            }
        }
    }

    // =========================================================================
    // #1426: the moving coat's self-shadow follows the walk. The rebake keeps
    // the volume within its drift bound on every frame, costs what it costs
    // (printed for #1427), and a frozen bake is DETECTED as stale rather than
    // sampled -- the negative control that makes "within the bound" mean
    // something.
    // =========================================================================
    TEST_F(GroomAnimalsAcceptanceEvidenceTest, TheMovingCoatShadowFollowsTheWalk)
    {
        SetPath(RenderingPath::Forward);
        const GroomCoatShadow::CoatRebakePolicy shipped = Renderer3D::GetGroomCoatRebakePolicy();
        ASSERT_EQ(shipped, GroomCoatShadow::CoatRebakePolicy{}) << "a previous case left a policy behind";

        struct WalkCost
        {
            u32 Frames = 0;
            u32 Rebakes = 0;
            u64 BakeMicroseconds = 0;
            u64 WorstFrameMicroseconds = 0;
            // #1445's split, summed over the walk. Pose is the evaluation of
            // the drawn centrelines the bake reads, outside BakeMicroseconds.
            u64 PoseMicroseconds = 0;
            u64 DriftMicroseconds = 0;
            u64 SegmentMicroseconds = 0;
            u64 BinMicroseconds = 0;
            u64 PackMicroseconds = 0;
            u64 UploadMicroseconds = 0;
            u32 MaxBakeStride = 0;
            f32 WorstDriftVoxels = 0.0f;
            u32 MinShadowed = 0xFFFFFFFFu;
            u32 StaleEver = 0;
        };
        const auto walk = [this](u32 frames)
        {
            for (SubjectRig* s : Subjects())
            {
                auto& anim = s->Body.GetComponent<AnimationStateComponent>();
                anim.m_CurrentTime = s->ClipStart;
                anim.m_IsPlaying = true;
                ++s->Coat.GetComponent<GroomSimulationComponent>().m_ResetKey;
            }
            WalkCost cost;
            for (u32 f = 0; f < frames; ++f)
            {
                RunFrames(1, 1.0f / 60.0f);
                const GroomCoatShadowStats& c = PassStats().CoatShadow;
                // Frame 0 builds every volume for the first time; it is the
                // warm-up, not the steady-state cost.
                if (f == 0)
                {
                    continue;
                }
                ++cost.Frames;
                cost.Rebakes += c.DeformedRebakes;
                cost.BakeMicroseconds += c.BakeMicroseconds;
                cost.WorstFrameMicroseconds = std::max(cost.WorstFrameMicroseconds, c.BakeMicroseconds);
                cost.PoseMicroseconds += PassStats().DeformedPoseMicroseconds;
                cost.DriftMicroseconds += c.DriftMicroseconds;
                cost.SegmentMicroseconds += c.BakeSegmentMicroseconds;
                cost.BinMicroseconds += c.BakeBinMicroseconds;
                cost.PackMicroseconds += c.BakePackMicroseconds;
                cost.UploadMicroseconds += c.BakeUploadMicroseconds;
                cost.MaxBakeStride = std::max(cost.MaxBakeStride, c.MaxBakeStride);
                cost.WorstDriftVoxels = std::max(cost.WorstDriftVoxels, c.MaxDriftVoxels);
                cost.MinShadowed = std::min(cost.MinShadowed, c.ShadowedGrooms);
                cost.StaleEver += c.ByReason[static_cast<sizet>(GroomCoatShadowFallbackReason::RepresentationStale)];
            }
            return cost;
        };
        const auto report = [](const char* label, const WalkCost& c)
        {
            std::printf("[groom-animals] coat rebake %s: %u rebakes over %u frames x 3 coats (%.2f per frame), bake "
                        "%.2f ms/frame mean, %.2f ms worst frame, worst drift in use %.2f vox, min shadowed %u, "
                        "stale %u\n",
                        label, c.Rebakes, c.Frames, static_cast<f64>(c.Rebakes) / std::max(1u, c.Frames),
                        static_cast<f64>(c.BakeMicroseconds) / 1000.0 / std::max(1u, c.Frames),
                        static_cast<f64>(c.WorstFrameMicroseconds) / 1000.0, static_cast<f64>(c.WorstDriftVoxels),
                        c.MinShadowed, c.StaleEver);
            const f64 perFrame = 1000.0 * std::max(1u, c.Frames);
            std::printf("[groom-animals] coat rebake %s by stage, ms/frame: pose %.2f, drift %.2f, segments %.2f, "
                        "binning %.2f, pack %.2f, texture create+upload %.2f; subset stride up to %u\n",
                        label, static_cast<f64>(c.PoseMicroseconds) / perFrame,
                        static_cast<f64>(c.DriftMicroseconds) / perFrame,
                        static_cast<f64>(c.SegmentMicroseconds) / perFrame,
                        static_cast<f64>(c.BinMicroseconds) / perFrame, static_cast<f64>(c.PackMicroseconds) / perFrame,
                        static_cast<f64>(c.UploadMicroseconds) / perFrame, c.MaxBakeStride);
            std::fflush(stdout);
        };

        // ── The shipped policy over a walk ─────────────────────────────
        constexpr u32 kWalk = 60;
        const WalkCost shippedCost = walk(kWalk);
        report("shipped (0.5 vox)", shippedCost);
        EXPECT_EQ(shippedCost.MinShadowed, 3u) << "a moving coat lost its self-shadow mid-walk";
        EXPECT_EQ(shippedCost.StaleEver, 0u) << "the shipped policy let a volume go stale";
        EXPECT_LE(shippedCost.WorstDriftVoxels, shipped.MaxDriftVoxels + 1.0e-4f)
            << "a coat was shadowed by a volume further from its pose than the rebake bound";
        EXPECT_GT(shippedCost.Rebakes, 0u) << "the coats walked and nothing was rebaked: the volume is not following";

        // ── The cadence sweep, on the real walk ────────────────────────
        //
        // The lag each bound buys is the worst drift in use; the cost is the
        // rebakes and the bake time. The rule doc quotes these lines.
        for (const f32 bound : { 0.25f, 1.0f, 2.0f })
        {
            GroomCoatShadow::CoatRebakePolicy policy;
            policy.MaxDriftVoxels = bound;
            policy.StaleDriftVoxels = std::max(bound, shipped.StaleDriftVoxels);
            Renderer3D::SetGroomCoatRebakePolicy(policy);
            const WalkCost cost = walk(kWalk);
            char label[32];
            std::snprintf(label, sizeof(label), "bound %.2f vox", static_cast<f64>(bound));
            report(label, cost);
            EXPECT_LE(cost.WorstDriftVoxels, bound + 1.0e-4f) << label;
        }

        // ── The negative control: a frozen bake is detected ────────────
        //
        // The rebake switched off, the walk played: the volume stays at the
        // first frame's pose while the coat walks away from it. The detector
        // must say so -- the coat falls back as RepresentationStale and reads
        // fully lit -- rather than keep shadowing it by where it was.
        GroomCoatShadow::CoatRebakePolicy frozen;
        frozen.RebakeOnDrift = false;
        // SEEDED at the walk's first pose, under the shipped policy, before the
        // freeze. Otherwise the resident volume is the previous sweep's END
        // pose, the reset to the clip start makes it stale on frame one, and
        // this case would pass on the jump without ever testing drift that
        // accumulates while the coat walks.
        // TWO frames: the first bake of an entry is a full one that measures
        // the coat, and the second takes the subset it chose (#1445). A stride
        // change is a rebuild rather than drift, so seeding with one frame would
        // count that rebuild inside the frozen walk.
        Renderer3D::SetGroomCoatRebakePolicy(shipped);
        (void)walk(2);
        Renderer3D::SetGroomCoatRebakePolicy(frozen);
        const WalkCost frozenCost = walk(kWalk);
        report("frozen", frozenCost);
        Renderer3D::SetGroomCoatRebakePolicy(shipped);
        EXPECT_EQ(frozenCost.Rebakes, 0u) << "the frozen policy rebaked";
        EXPECT_GT(frozenCost.StaleEver, 0u) << "a volume frozen at the first frame's pose was never detected as stale "
                                               "over a whole walk";
        EXPECT_LT(frozenCost.MinShadowed, 3u) << "a stale volume was still sampled";
    }

    // =========================================================================
    // #1426: the pictures. The moving long coat, held at several points of the
    // walk and seen from two sides, with and without its self-shadow, on
    // Forward; then the same pose under MSAA and under upscale on Deferred.
    // Each cell is its own A/B, judged against the measured repeat floor.
    // =========================================================================
    TEST_F(GroomAnimalsAcceptanceEvidenceTest, TheMovingCoatIsSelfShadowedThroughTheWalk)
    {
        SubjectRig& s = m_LongCoat;
        auto& coatShadow = s.Coat.GetComponent<GroomCoatShadowComponent>();
        ASSERT_TRUE(coatShadow.m_Enabled);

        const View side = ViewOf(s);
        const View front{ s.Position + glm::vec3(2.9f, 1.3f, 3.4f), s.Position + glm::vec3(0.0f, 1.1f, 0.0f), 40.0f };

        // Summed luma, ON minus OFF: the sign says which way the term pushed
        // the frame. The fixture's own SummedLuma, so this case and the lever
        // case score luma the same way.
        const auto lumaDelta = [](const std::vector<u8>& on, const std::vector<u8>& off)
        { return SummedLuma(on) - SummedLuma(off); };

        // Shadow on, then off, at the SAME pose. EACH ARM REPLAYS THE WALK to
        // `frame` first: a capture's settle frames DO advance the clip, so two
        // captures taken back to back are two different poses. The first
        // version of this case captured them back to back, measured a "repeat
        // floor" of 109k px between two poses, and read the legs' motion as
        // the shadow's effect. Returns the shadowed count of the ON arm.
        const auto abAt = [&](u32 frame, const std::string& onName, const std::string& offName, const View& view,
                              std::vector<u8>& on, std::vector<u8>& off)
        {
            coatShadow.m_Enabled = true;
            (void)PlayFromStart(frame);
            const u32 shadowed = PassStats().CoatShadow.ShadowedGrooms;
            ColdHistory();
            Capture(onName, view, on);
            coatShadow.m_Enabled = false;
            (void)PlayFromStart(frame);
            ColdHistory();
            Capture(offName, view, off);
            coatShadow.m_Enabled = true;
            return shadowed;
        };

        SetPath(RenderingPath::Forward);

        // The repeat floor: the identical replay-and-capture twice, the same
        // way every arm below is taken.
        std::vector<u8> first;
        std::vector<u8> second;
        (void)PlayFromStart(30);
        ColdHistory();
        Capture("", side, first);
        (void)PlayFromStart(30);
        ColdHistory();
        Capture("", side, second);
        ASSERT_FALSE(HasFatalFailure());
        const u32 floor = CountDiffering(first, second);
        const f64 repeatLuma = std::abs(lumaDelta(first, second));
        std::printf("[groom-animals] moving-coat shadow: repeat floor %u px, luma drift %.0f\n", floor, repeatLuma);

        for (const u32 frame : { 15u, 30u, 45u })
        {
            for (const auto& [angle, view] : { std::pair<const char*, View>{ "Side", side },
                                               std::pair<const char*, View>{ "Front", front } })
            {
                const std::string cell = "_GL_Forward_LongCoat_F" + std::to_string(frame) + "_" + angle;
                std::vector<u8> on;
                std::vector<u8> off;
                const u32 shadowed = abAt(frame, "GroomAnimalsMovingCoatShadow" + cell,
                                          "GroomAnimalsMovingCoatShadowOff" + cell, view, on, off);
                ASSERT_FALSE(HasFatalFailure());
                EXPECT_EQ(shadowed, 3u) << "F" << frame << " " << angle;
                const u32 moved = CountDiffering(on, off);
                const f64 delta = lumaDelta(on, off);
                std::printf("[groom-animals] moving-coat shadow F%u %s: %u px (%.2fx floor), luma %.0f\n", frame,
                            angle, moved, floor > 0u ? static_cast<f64>(moved) / floor : 0.0, delta);
                std::fflush(stdout);
                // Every cell's A/B clears the pixel floor. Beyond that the two
                // views make DIFFERENT claims, and the difference is measured:
                //
                // SIDE: the dense flank, neck and mane fill the frame, so the
                // term DARKENS it, by several times what an identical replay
                // drifts -- the lever case's prediction, on every frame of the
                // walk.
                //
                // FRONT: the sparse lower-leg feathering fills much of the
                // frame, and on a sparse coat turning the term on BRIGHTENS it
                // (groom-coat-self-shadowing.md rule 3: the measured occlusion
                // replaces a fake root ramp that darkened more). At F45 Front
                // the signed diff is 20 344 px brighter, all on the legs,
                // against 19 855 darker under the mane and on the chest, and
                // the two CANCEL in a summed luma (+25 532 against a 6 553
                // drift). A net sum cannot score a view whose true answer has
                // both signs, so the front views are printed and looked at,
                // and hold only to the floor.
                EXPECT_GT(moved, floor) << "F" << frame << " " << angle;
                if (std::string_view(angle) == "Side")
                {
                    EXPECT_LT(delta, -5.0 * repeatLuma)
                        << "F" << frame << " Side: the self-shadow did not clearly darken the moving coat";
                }
                if (frame == 30u && std::string(angle) == "Side")
                {
                    // visual-quality-criteria rule 1: judge it enlarged.
                    WriteEnlargedCrop("GroomAnimalsMovingCoatShadowCrop_GL_Forward_LongCoat_F30_Side", on, 440u, 180u,
                                      400u, 300u, 3u);
                    WriteEnlargedCrop("GroomAnimalsMovingCoatShadowOffCrop_GL_Forward_LongCoat_F30_Side", off, 440u,
                                      180u, 400u, 300u, 3u);
                }
            }
        }

        // ── MSAA and upscale, one moving pose each, on Deferred ─────────
        SetPath(RenderingPath::Deferred);
        auto& deferred = Renderer3D::GetRendererSettings().Deferred;
        auto& post = Renderer3D::GetPostProcessSettings();
        const u32 restoreSamples = deferred.MSAASampleCount;
        const UpscaleMode restoreUpscale = post.Upscale;

        struct Cell
        {
            const char* Name;
            u32 Samples;
            UpscaleMode Upscale;
        };
        for (const Cell& cell : { Cell{ "NoMsaa", 1u, UpscaleMode::Off }, Cell{ "Msaa4", 4u, UpscaleMode::Off },
                                  Cell{ "Scaled", 1u, UpscaleMode::Performance } })
        {
            deferred.MSAASampleCount = cell.Samples;
            post.Upscale = cell.Upscale;
            Renderer3D::ApplyRendererSettings();
            const std::string name = std::string("_GL_Deferred_LongCoat_F30_") + cell.Name;
            std::vector<u8> on;
            std::vector<u8> off;
            const u32 shadowed =
                abAt(30u, "GroomAnimalsMovingCoatShadow" + name, "GroomAnimalsMovingCoatShadowOff" + name, side, on, off);
            if (HasFatalFailure())
            {
                break;
            }
            const u32 moved = CountDiffering(on, off);
            const f64 delta = lumaDelta(on, off);
            std::printf("[groom-animals] moving-coat shadow Deferred %s: shadowed=%u, %u px (%.2fx floor), luma %.0f\n",
                        cell.Name, shadowed, moved, floor > 0u ? static_cast<f64>(moved) / floor : 0.0, delta);
            std::fflush(stdout);
            EXPECT_EQ(shadowed, 3u) << cell.Name;
            EXPECT_GT(moved, floor) << cell.Name;
            EXPECT_GT(std::abs(delta), 5.0 * repeatLuma)
                << cell.Name << ": the self-shadow barely changes the moving coat";
            EXPECT_LT(delta, 0.0) << cell.Name << ": the self-shadow did not darken the coat";
        }
        deferred.MSAASampleCount = restoreSamples;
        post.Upscale = restoreUpscale;
        Renderer3D::ApplyRendererSettings();
    }

    // =========================================================================
    // Criterion 2, motion: through a whole walk stride the coat stays on the
    // body (every root deformed, none held), keeps its length, keeps its
    // history, and keeps a steady share of the frame.
    // =========================================================================
    TEST_F(GroomAnimalsAcceptanceEvidenceTest, TheCoatStaysOnTheBodyThroughTheStride)
    {
        SetPath(RenderingPath::Forward);
        (void)PlayFromStart(30);

        constexpr u32 kSteps = 8;
        constexpr u32 kFramesPerStep = 9; // 8 x 0.15 s = one 1.2 s walk stride
        const View view = ViewOf(m_ShortCoat);
        std::vector<u32> coverage;
        std::vector<std::vector<u8>> frames;
        for (u32 step = 0; step < kSteps; ++step)
        {
            const MotionResult motion = Play(kFramesPerStep);
            EXPECT_EQ(motion.MinGroomsDeformed, 3u) << "step " << step;
            EXPECT_EQ(motion.MaxHeldAtRest, 0u) << "step " << step;
            EXPECT_EQ(motion.MaxHistoryRejectedAfterFirst, 0u) << "step " << step;
            EXPECT_LE(std::abs(motion.WorstStretch - 1.0f), std::abs(motion.Last.DeclaredStretchTolerance - 1.0f) + 1.0e-4f)
                << "step " << step;
            std::vector<u8> on;
            const u32 coat = CoatPixels(m_ShortCoat, view, "", "", &on);
            ASSERT_FALSE(HasFatalFailure());
            coverage.push_back(coat);
            frames.push_back(std::move(on));
            std::printf("[groom-animals] stride step %u: coat %u px\n", step, coat);
            std::fflush(stdout);
        }

        // A coat that detached, popped or thinned on some frame shows up here.
        const f64 mean = std::accumulate(coverage.begin(), coverage.end(), 0.0) / static_cast<f64>(coverage.size());
        for (u32 step = 0; step < kSteps; ++step)
        {
            EXPECT_NEAR(static_cast<f64>(coverage[step]) / mean, 1.0, 0.35)
                << "step " << step << ": the coat's share of the frame jumped mid-stride";
        }

        // The stride as one strip, 4 x 2, half resolution each.
        constexpr u32 kTileW = kWidth / 2u;
        constexpr u32 kTileH = kHeight / 2u;
        std::vector<u8> strip(static_cast<sizet>(kTileW) * 4u * kTileH * 2u * 4u, 0u);
        for (u32 i = 0; i < kSteps; ++i)
        {
            const u32 ox = (i % 4u) * kTileW;
            const u32 oy = (i / 4u) * kTileH;
            for (u32 y = 0; y < kTileH; ++y)
            {
                for (u32 x = 0; x < kTileW; ++x)
                {
                    std::memcpy(&strip[(static_cast<sizet>(oy + y) * kTileW * 4u + ox + x) * 4u],
                                &frames[i][(static_cast<sizet>(y * 2u) * kWidth + x * 2u) * 4u], 4u);
                }
            }
        }
        WritePng("GroomAnimalsStride_GL_Forward_ShortCoat", strip, kTileW * 4u, kTileH * 2u);
    }

    // =========================================================================
    // Criterion 2, LOD (#1252, #1428): walking the camera away hands each coat
    // from strands to cooked cards, and the coat keeps its part of the picture.
    //
    // FOUR LINEAR QUANTITIES, measured apart, because a single number hid what
    // was wrong (groom-card-coverage.md). Per frame the ENTITY-ID target says
    // which pixels show this coat and the HDR scene colour says what they
    // show, averaged over 64 frames of the stochastic coat with no temporal
    // resolve in the way:
    //   * COVERAGE: the coat's pixels. Geometry, and occlusion by the body.
    //   * RADIANCE: the coat's mean linear luma per pixel with its self-shadow
    //     made transparent (kappa = 0, which leaves the volume mode active and
    //     so does not bring back the root ramp the volume replaces).
    //   * SHADOW: lit over unshadowed luma, the mean self-shadow transmittance.
    //   * ENERGY: the coat's summed linear luma -- the product of the three,
    //     and the quantity a pop is made of.
    // Each is the ladder-on value over the ladder-off one at the same stop.
    //
    // The pose is FROZEN, and each coat is looked at from its OWN open side:
    // the first version put the short coat behind the long horse, so its
    // numbers described a sliver of coat around another animal (#1428).
    // =========================================================================
    TEST_F(GroomAnimalsAcceptanceEvidenceTest, TheCoatKeepsItsCoverageFromNearToFar)
    {
        SetPath(RenderingPath::Forward);
        (void)PlayFromStart(30);
        for (SubjectRig* subject : Subjects())
        {
            subject->Body.GetComponent<AnimationStateComponent>().m_IsPlaying = false;
        }
        constexpr u32 kFrames = 64;

        struct Linear
        {
            f64 Cov = 0.0;  // coat pixels, per frame
            f64 Luma = 0.0; // linear luma on them, per frame
        };
        const auto linear = [&](const View& view, i32 entityId)
        {
            const glm::vec3 d = glm::normalize(view.Target - view.Eye);
            EditorCamera camera(view.Fov, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 400.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(view.Eye, std::atan2(d.x, -d.z), std::asin(std::clamp(-d.y, -1.0f, 1.0f)));
            ColdHistory();
            RunEditorFrames(camera, 8);
            Linear out;
            std::vector<f32> colour;
            std::vector<i32> ids;
            for (u32 f = 0; f < kFrames; ++f)
            {
                RunEditorFrames(camera, 1);
                auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
                EXPECT_TRUE(fb);
                if (!fb)
                {
                    return out;
                }
                // The target's own size, and only its ACTIVE viewport: under an
                // upscaler the scene renders into a smaller rectangle of it.
                const u32 width = fb->GetSpecification().Width;
                const u32 height = fb->GetSpecification().Height;
                ReadbackRgbaFloat(fb->GetColorAttachmentRendererID(0), width, height, colour);
                ids.resize(static_cast<sizet>(width) * height);
                ::glGetTextureImage(fb->GetColorAttachmentRendererID(1), 0, GL_RED_INTEGER, GL_INT,
                                    static_cast<GLsizei>(ids.size() * sizeof(i32)), ids.data());
                const u32 activeWidth = std::min(fb->GetActiveViewportWidth(), width);
                const u32 activeHeight = std::min(fb->GetActiveViewportHeight(), height);
                for (sizet i = 0; i < ids.size(); ++i)
                {
                    if (ids[i] == entityId && (i % width) < activeWidth && (i / width) < activeHeight)
                    {
                        out.Cov += 1.0 / kFrames;
                        out.Luma += ((0.2126 * colour[i * 4]) + (0.7152 * colour[(i * 4) + 1]) +
                                     (0.0722 * colour[(i * 4) + 2])) /
                                    kFrames;
                    }
                }
            }
            return out;
        };

        struct Stop
        {
            const char* Name;
            f32 Distance;
        };
        const Stop nearStop{ "Near", 5.0f };
        const Stop midStop{ "Mid", 14.0f };
        const Stop handoverStop{ "Handover", 19.0f };
        const Stop farStop{ "Far", 32.0f };

        const auto viewAt = [&](const SubjectRig& s, const Stop& stop)
        {
            // A FIXED field of view: the animal genuinely shrinks with distance,
            // which is what the ladder answers to. Each horse from the side that
            // faces away from the other.
            const glm::vec3 away = glm::normalize(glm::vec3(&s == &m_ShortCoat ? -1.0f : 1.0f, 0.12f, 0.08f));
            const glm::vec3 target = s.Position + glm::vec3(0.0f, 1.15f, 0.0f);
            return View{ target + away * stop.Distance, target, 40.0f };
        };

        struct Arm
        {
            Linear Lit;
            Linear Unshadowed;
            u32 Strands = 0;
            GroomRepresentation Representation = GroomRepresentation::Strand;
        };
        const auto measure = [&](SubjectRig& s, bool ladder, const Stop& stop, const std::string& crop)
        {
            s.Coat.GetComponent<GroomLodComponent>().m_Enabled = ladder;
            const View view = viewAt(s, stop);
            const i32 id = static_cast<i32>(static_cast<u32>(s.Coat));
            Arm arm;
            arm.Lit = linear(view, id);
            // THIS coat's representation and strands, from its own request: the
            // pass's counts are every groom drawn.
            if (const GroomStrandRequest* req = RequestFor(s); req != nullptr)
            {
                arm.Representation = req->Lod.Representation;
                const GroomCoatContext coatContext{ &req->Coat, req->Groom->GetGroupCoats() };
                arm.Strands = PlanGroomStrandMesh(req->BuildSource(), req->Build, &coatContext).StrandsSelected;
            }
            auto& shadow = s.Coat.GetComponent<GroomCoatShadowComponent>();
            const f32 kappa = shadow.m_Kappa;
            shadow.m_Kappa = 0.0f;
            arm.Unshadowed = linear(view, id);
            shadow.m_Kappa = kappa;
            if (!crop.empty())
            {
                // Rule 1 of the visual criteria: look at it enlarged. The crop is
                // centred on the animal and shrinks with it.
                std::vector<u8> frame;
                ColdHistory();
                Capture("", view, frame);
                const u32 half = std::max(24u, static_cast<u32>(220.0f * 5.0f / stop.Distance));
                const u32 scale = std::max(2u, 360u / half);
                WriteEnlargedCrop(crop + "_Crop", frame, kWidth / 2u - half, kHeight / 2u - half * 9u / 16u, half * 2u,
                                  half * 9u / 8u, std::min(scale, 8u));
            }
            return arm;
        };

        struct Kept
        {
            f64 Coverage = 0.0;
            f64 Radiance = 0.0;
            f64 Shadow = 0.0;
            f64 Energy = 0.0;
            Arm On;
            Arm Off;
        };
        const auto keep = [&](SubjectRig& s, const Stop& stop, const std::string& label, bool crops)
        {
            const std::string tail = label + "_" + s.Tag + "_" + stop.Name;
            Kept k;
            k.On = measure(s, true, stop, crops ? "GroomAnimalsLod_GL_" + tail : "");
            k.Off = measure(s, false, stop, crops ? "GroomAnimalsLodOff_GL_" + tail : "");
            s.Coat.GetComponent<GroomLodComponent>().m_Enabled = true;
            const auto ratio = [](f64 on, f64 off) { return off > 0.0 ? on / off : 0.0; };
            k.Coverage = ratio(k.On.Lit.Cov, k.Off.Lit.Cov);
            k.Radiance = ratio(k.On.Unshadowed.Luma / std::max(k.On.Unshadowed.Cov, 1.0),
                               k.Off.Unshadowed.Luma / std::max(k.Off.Unshadowed.Cov, 1.0));
            k.Shadow = ratio(k.On.Lit.Luma / std::max(k.On.Unshadowed.Luma, 1.0e-9),
                             k.Off.Lit.Luma / std::max(k.Off.Unshadowed.Luma, 1.0e-9));
            k.Energy = ratio(k.On.Lit.Luma, k.Off.Lit.Luma);
            std::printf("[groom-animals] lod %s %s %s: representation %u, strands %u / %u, coverage %.3f (%.0f / %.0f px), "
                        "radiance %.3f, shadow %.3f, energy %.3f\n",
                        label.c_str(), s.Tag.c_str(), stop.Name, static_cast<u32>(k.On.Representation), k.On.Strands,
                        k.Off.Strands, k.Coverage, k.On.Lit.Cov, k.Off.Lit.Cov, k.Radiance, k.Shadow, k.Energy);
            std::fflush(stdout);
            return k;
        };

        // THE BOUNDS (groom-card-coverage.md, "Measured").
        //
        // THE CONTRACT is +-10% of the coat's energy, on a CONVERGED self-shadow
        // volume (128^3, half-voxel march): there the card tier is the strand
        // tier within a few percent on both coats. At the SHIPPED coat-shadow
        // LOD the volume is 16^3 or coarser at the hand-over, and a coarse
        // volume's error depends on the representation -- the long coat's
        // cards read 1.2-1.3x the strands' energy there. That is the shadow
        // LOD's accuracy, filed rather than fixed here because the fix spends
        // shadow budget (#1508); the shipped cells hold a
        // gross guard that the pre-#1428 card tier failed at every stop.
        //
        // COVERAGE is bounded apart, wider on the high side: a card is as wide
        // as its members cover AVERAGED over the directions around it, and a
        // flat mane or tail seen face-on covers more than that average -- the
        // short coat, which is mostly mane and tail at range, reads 1.10-1.13.
        constexpr f64 kEnergyTolerance = 0.10;
        constexpr f64 kCoverageFloor = 0.90;
        constexpr f64 kCoverageCeiling = 1.20;
        constexpr f64 kShippedEnergyFloor = 0.75;
        constexpr f64 kShippedEnergyCeiling = 1.40;

        for (SubjectRig* s : { &m_LongCoat, &m_ShortCoat })
        {
            SCOPED_TRACE(s->Name);
            // THE CONTROL: close up both arms draw every strand at its own width,
            // so the two frames are the same frame and anything but 1 is the
            // measurement, not the ladder.
            const Kept atNear = keep(*s, nearStop, "Forward", true);
            ASSERT_FALSE(HasFatalFailure());
            ASSERT_GT(atNear.Off.Lit.Cov, 1000.0) << "the coat must be visible to be conserved at all";
            EXPECT_EQ(atNear.On.Strands, atNear.Off.Strands) << "the control drew different strands";
            EXPECT_NEAR(atNear.Coverage, 1.0, 0.01) << "two identical frames measured different coats";
            EXPECT_NEAR(atNear.Energy, 1.0, 0.03) << "two identical frames measured different coats";

            const Kept atMid = keep(*s, midStop, "Forward", true);
            ASSERT_FALSE(HasFatalFailure());
            EXPECT_EQ(atMid.On.Representation, GroomRepresentation::Strand) << "Mid must be on the strand tier";
            EXPECT_NEAR(atMid.Coverage, 1.0, 0.10) << "the strand budget did not keep the coverage";
            // The short coat's stride step keeps 1.08-1.15 of its energy with
            // no card in sight (#1509); it is held to a guard until that lands.
            const f64 midTolerance = s == &m_ShortCoat ? 0.20 : kEnergyTolerance;
            EXPECT_NEAR(atMid.Energy, 1.0, midTolerance) << "the strand budget did not keep the coat";

            for (const Stop& stop : { handoverStop, farStop })
            {
                SCOPED_TRACE(stop.Name);
                const Kept k = keep(*s, stop, "Forward", true);
                ASSERT_FALSE(HasFatalFailure());
                EXPECT_EQ(k.On.Representation, GroomRepresentation::Card) << "past the hand-over the coat is on cards";
                EXPECT_LT(k.On.Strands * 4u, k.Off.Strands) << "and draws under a quarter of the strands";
                EXPECT_GT(k.Coverage, kCoverageFloor) << "the card tier lost coverage";
                EXPECT_LT(k.Coverage, kCoverageCeiling) << "the card tier ballooned the coat";
                EXPECT_GT(k.Energy, kShippedEnergyFloor) << "the card tier lost the coat";
                EXPECT_LT(k.Energy, kShippedEnergyCeiling) << "the card tier ballooned the coat";
            }

            // THE CONTRACT, on a converged self-shadow volume for both arms.
            auto& shadow = s->Coat.GetComponent<GroomCoatShadowComponent>();
            auto& lod = s->Coat.GetComponent<GroomLodComponent>();
            const GroomCoatShadowComponent shippedShadow = shadow;
            const u32 shippedShadowSteps = lod.m_ShadowSteps;
            shadow.m_Resolution = 128u;
            shadow.m_MaxLodSteps = 0u;
            shadow.m_StepVoxels = 0.5f;
            lod.m_ShadowSteps = 0u;
            for (const Stop& stop : { handoverStop, farStop })
            {
                SCOPED_TRACE(stop.Name);
                const Kept k = keep(*s, stop, "ConvergedShadow", false);
                ASSERT_FALSE(HasFatalFailure());
                EXPECT_EQ(k.On.Representation, GroomRepresentation::Card);
                EXPECT_NEAR(k.Energy, 1.0, kEnergyTolerance) << "the card tier does not keep the coat";
            }
            shadow = shippedShadow;
            lod.m_ShadowSteps = shippedShadowSteps;
        }

        // The hand-over on the other two paths, and under MSAA, where the pop
        // would show if the card tier's coverage depended on how the frame is
        // composed.
        auto& rs = Renderer3D::GetRendererSettings();
        auto& post = Renderer3D::GetPostProcessSettings();
        // Restored on every exit, an early ASSERT included: these are process
        // globals, and the next test would otherwise run upscaled.
        struct Restore
        {
            u32 Samples;
            UpscaleMode Upscale;
            UpscalerTechnique Technique;
            ~Restore()
            {
                Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = Samples;
                Renderer3D::GetPostProcessSettings().Upscale = Upscale;
                Renderer3D::GetPostProcessSettings().Technique = Technique;
                SetPath(RenderingPath::Forward);
            }
        } const restore{ rs.Deferred.MSAASampleCount, post.Upscale, post.Technique };
        struct PathCell
        {
            const char* Name;
            RenderingPath Path;
            u32 Samples;
            UpscaleMode Upscale;
            UpscalerTechnique Technique;
        };
        // And under the editor's upscalers, where the scene renders at a lower
        // resolution and the ladder measures the coat at that resolution.
        for (const PathCell& cell :
             { PathCell{ "Deferred", RenderingPath::Deferred, 1u, UpscaleMode::Off, UpscalerTechnique::Spatial },
               PathCell{ "ForwardPlus", RenderingPath::ForwardPlus, 1u, UpscaleMode::Off, UpscalerTechnique::Spatial },
               PathCell{ "DeferredMsaa4", RenderingPath::Deferred, 4u, UpscaleMode::Off, UpscalerTechnique::Spatial },
               PathCell{ "ForwardFSR1", RenderingPath::Forward, 1u, UpscaleMode::Quality, UpscalerTechnique::Spatial },
               PathCell{ "ForwardFSR2", RenderingPath::Forward, 1u, UpscaleMode::Quality,
                         UpscalerTechnique::Temporal } })
        {
            SCOPED_TRACE(cell.Name);
            rs.Deferred.MSAASampleCount = cell.Samples;
            post.Upscale = cell.Upscale;
            post.Technique = cell.Technique;
            SetPath(cell.Path);
            const Kept k = keep(m_LongCoat, handoverStop, cell.Name, true);
            ASSERT_FALSE(HasFatalFailure());
            EXPECT_GT(k.Off.Lit.Cov, 1000.0) << cell.Name << ": the coat must be measurable on this path";
            EXPECT_EQ(k.On.Representation, GroomRepresentation::Card);
            // WHICH resolve: an unavailable FSR2 falls back to FSR1, and the
            // FSR2 cell would quietly measure the other upscaler.
            if (cell.Technique == UpscalerTechnique::Temporal)
            {
                EXPECT_TRUE(Renderer3D::IsTemporalUpscaleActive()) << "FSR2 must own this frame";
            }
            EXPECT_GT(k.Coverage, kCoverageFloor) << cell.Name;
            EXPECT_LT(k.Coverage, kCoverageCeiling) << cell.Name;
            EXPECT_GT(k.Energy, kShippedEnergyFloor) << cell.Name;
            EXPECT_LT(k.Energy, kShippedEnergyCeiling) << cell.Name;
        }
    }

    // =========================================================================
    // Criterion 2, temporal: the stochastic coat, frozen mid-stride, settles
    // under the temporal resolve; the same frames with no history do not.
    // =========================================================================
    TEST_F(GroomAnimalsAcceptanceEvidenceTest, TheResolveSettlesTheCoat)
    {
        SetPath(RenderingPath::Forward);
        (void)PlayFromStart(30);
        const View view{ m_LongCoat.Position + glm::vec3(2.2f, 1.7f, 1.2f), m_LongCoat.Position + glm::vec3(0.0f, 1.5f, 0.7f),
                         40.0f };

        const auto sequence = [&](f32 feedback)
        {
            SetTaa(feedback);
            ColdHistory();
            std::vector<u8> px;
            Capture("", view, px, 16);
            std::vector<std::vector<f32>> luma;
            for (u32 i = 0; i < 12; ++i)
            {
                Capture("", view, px, 1);
                std::vector<f32> l(static_cast<sizet>(kWidth) * kHeight);
                for (sizet p = 0; p < l.size(); ++p)
                {
                    l[p] = (0.2126f * px[p * 4] + 0.7152f * px[p * 4 + 1] + 0.0722f * px[p * 4 + 2]) / 255.0f;
                }
                luma.push_back(std::move(l));
            }
            return TemporalSequenceMetrics::MeasureShimmer(luma);
        };
        const auto resolved = sequence(0.9f);
        const auto raw = sequence(0.0f);
        SetTaa(0.9f);
        std::printf("[groom-animals] shimmer resolved=%.5f (peak %.3f, px %u) noHistory=%.5f (peak %.3f, px %u)\n",
                    resolved.MeanFrameDelta, resolved.PeakPixelDelta, resolved.ComparedPixels, raw.MeanFrameDelta,
                    raw.PeakPixelDelta, raw.ComparedPixels);
        ASSERT_GT(resolved.ComparedPixels, 0u);
        ASSERT_GT(raw.ComparedPixels, 0u);
        EXPECT_LT(resolved.MeanFrameDelta, raw.MeanFrameDelta * 0.5) << "the resolve must at least halve the shimmer";
    }

    // =========================================================================
    // Criterion 3: cost across a herd. More horses sharing the short coat's
    // cooked assets; the scheduler (#1258) on and off; GPU pass time, strands
    // and resident geometry recorded against the named GPU.
    // =========================================================================
    TEST_F(GroomAnimalsAcceptanceEvidenceTest, CostScalesAcrossAHerd)
    {
        // Constrains even three coats; the 12-animal case exercises larger-herd shedding.
        constexpr f32 kHerdBudgetUnits = 1200.0f;
        SetPath(RenderingPath::Forward);
        const View view{ glm::vec3(0.0f, 4.0f, 14.0f), glm::vec3(0.0f, 1.0f, -8.0f), 55.0f };

        // The hero is the short-coated subject; the herd stands behind it.
        m_ShortCoat.Coat.AddComponent<AnimalBudgetComponent>().m_Role = static_cast<u8>(AnimalRole::Hero);
        m_LongCoat.Coat.AddComponent<AnimalBudgetComponent>().m_Role = static_cast<u8>(AnimalRole::Featured);
        m_Human.Coat.AddComponent<AnimalBudgetComponent>().m_Role = static_cast<u8>(AnimalRole::Featured);

        std::string report;
        const auto gpuName = []()
        {
            const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            return std::string(renderer != nullptr ? renderer : "unknown");
        }();
        report += "# GroomAnimalsAcceptanceEvidenceTest.CostScalesAcrossAHerd (#1223)\n";
        report += "# GPU: " + gpuName + "\n";
        report += "# Release test binary, 1280x720, Forward, TAA on. GroomPass GPU ms is the mean of VALID samples\n";
        report += "# (GPUPassTimerPool, #1337) over 20 frames. Cache MiB is the strand geometry the pass holds.\n";
        report += "# The 1200-unit budget also constrains three animals when scheduling is on; off is full density.\n";
        report += "herd  scheduler  groomsDrawn  strandsDrawn  guidesSimulated  cacheMiB  groomPassMs  validSamples  wallMsPerFrame  cpuBuildMs  uploadMiB\n";

        std::vector<Entity> herd;
        u32 strandsOffAt9 = 0;
        u32 strandsOnAt9 = 0;
        GroomStrandBuildSettings heroOff{}, heroOn{};
        for (u32 extra : { 0u, 4u, 9u })
        {
            while (herd.size() < extra)
            {
                const u32 i = static_cast<u32>(herd.size());
                const glm::vec3 position(-6.0f + 3.0f * static_cast<f32>(i % 5u), 0.0f,
                                         -7.0f - 5.0f * static_cast<f32>(i / 5u));
                Entity body = MakeBody("Herd" + std::to_string(i), m_ShortCoat.Model, HorsePath(), position, "Walk",
                                       0.13f * static_cast<f32>(i));
                // The coat entity is offset by the same delta as the body, so
                // groom-from-body is the matrix the binding was cooked with and
                // the one cooked binding serves every horse of this topology.
                Entity coat = MakeCoat("HerdCoat" + std::to_string(i), m_ShortCoat.GroomHandle, m_ShortCoat.BindingHandle,
                                       body, Subject::Horse, position - m_ShortCoat.Position, m_ShortCoat.Grown.Strands);
                coat.AddComponent<AnimalBudgetComponent>().m_Role = static_cast<u8>(AnimalRole::Background);
                herd.push_back(coat);
            }
            for (bool scheduler : { false, true })
            {
                auto& settings = Renderer3D::GetRendererSettings();
                settings.AnimalSchedulingEnabled = scheduler;
                settings.AnimalProtectHero = true;
                settings.AnimalHoldFrames = 0u;
                // A budget that binds even for three animals. At the engine default the
                // whole herd fits and both arms are the same frame, which
                // measures nothing about the scheduler.
                settings.AnimalFrameBudgetUnits = kHerdBudgetUnits;
                (void)PlayFromStart(30); // includes the warm-up: the first frames build every cache
                const glm::vec3 d = glm::normalize(view.Target - view.Eye);
                EditorCamera camera(view.Fov, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 400.0f);
                camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
                camera.SetPose(view.Eye, std::atan2(d.x, -d.z), std::asin(std::clamp(-d.y, -1.0f, 1.0f)));

                f64 gpuSum = 0.0;
                u32 gpuValid = 0;
                // #1427: where a bound coat's frame goes, split into the CPU
                // work that prepares its geometry and the bytes that work
                // sends to the GPU. Summed per frame, reported as a mean.
                f64 buildMsSum = 0.0;
                f64 uploadMiBSum = 0.0;
                const auto t0 = std::chrono::steady_clock::now();
                for (u32 f = 0; f < 20; ++f)
                {
                    RunFrames(1, 1.0f / 60.0f); // runtime: the herd moves and is simulated
                    buildMsSum += static_cast<f64>(PassStats().DeformedBuildMicroseconds) / 1000.0;
                    uploadMiBSum += static_cast<f64>(PassStats().DeformedUploadBytes) / (1024.0 * 1024.0);
                    for (const auto& timing : GPUPassTimerPool::GetInstance().GetLastFrameTimings().Passes)
                    {
                        if (timing.Name == "GroomPass" && timing.IsValid())
                        {
                            gpuSum += timing.Sample.GpuMs;
                            ++gpuValid;
                        }
                    }
                }
                const f64 wallMs =
                    std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20.0;
                const GroomRenderStats st = PassStats();
                const GroomStrandRequest* hero = RequestFor(m_ShortCoat);
                ASSERT_NE(hero, nullptr);
                const AnimalSchedulerStats& sched = GetScene().GetAnimalSchedulerStats();
                std::printf("[groom-animals] scheduler %s herd %u: considered=%u coarsened=%u atDesired=%u\n",
                            scheduler ? "on" : "off", extra + 3u, sched.AnimalsConsidered, sched.AnimalsCoarsened,
                            sched.AnimalsAtDesired);

                if (extra == 9u)
                {
                    (scheduler ? strandsOnAt9 : strandsOffAt9) = st.StrandsDrawn;
                    (scheduler ? heroOn : heroOff) = hero->Build;
                }
                char row[256];
                std::snprintf(row, sizeof(row), "%4u  %9s  %11u  %12u  %15u  %8.1f  %11.3f  %12u  %14.1f  %10.2f  %9.1f\n",
                              extra + 3u, scheduler ? "on" : "off", st.GroomsDrawn, st.StrandsDrawn,
                              st.GuidesSimulated, static_cast<f64>(st.CachedBytes) / (1024.0 * 1024.0),
                              gpuValid > 0 ? gpuSum / static_cast<f64>(gpuValid) : -1.0, gpuValid, wallMs,
                              buildMsSum / 20.0, uploadMiBSum / 20.0);
                report += row;
                std::printf("[groom-animals] cost %s", row);
                std::fflush(stdout);
                EXPECT_EQ(st.GroomsDrawn, extra + 3u);
                EXPECT_GT(gpuValid, 0u) << "the GroomPass must be timed on this box";
            }
            std::vector<u8> px;
            ColdHistory();
            Capture("GroomAnimalsHerd_GL_Forward_" + std::to_string(extra + 3u), view, px);
        }
        Renderer3D::GetRendererSettings().AnimalSchedulingEnabled = false;

        const char* exportFlag = std::getenv("OLO_GROOM_ANIMALS_COST_EXPORT");
        const fs::path reportPath = exportFlag != nullptr && std::string_view(exportFlag) == "1"
                                        ? fs::path("assets") / "tests" / "visual" / "GroomAnimals_Cost.txt"
                                        : TempDir("cost-report") / "GroomAnimals_Cost.txt";
        std::ofstream(reportPath) << report;
        EXPECT_LT(strandsOnAt9, strandsOffAt9) << "#1258: the scheduler must shed strand work from a 12-animal herd";
        EXPECT_TRUE(heroOn == heroOff) << "#1258: while leaving the hero's coat exactly as it was";
    }

    // =========================================================================
    // Criterion 3, flows: the cooked groom and binding survive the loose
    // import path and the asset-pack path byte-for-byte, and a scene drawing
    // the IMPORTED assets draws all three coats.
    // =========================================================================
    TEST_F(GroomAnimalsAcceptanceEvidenceTest, LooseAndPackedGroomsAreTheAssetsThatWereCooked)
    {
        auto editorAssets = Project::GetAssetManager().As<EditorAssetManager>();
        ASSERT_TRUE(editorAssets) << "the loose path needs the editor asset manager";
        const fs::path assetsDir = Project::GetAssetDirectory();

        for (SubjectRig* subject : Subjects())
        {
            SCOPED_TRACE(subject->Name);
            std::string reason;
            std::vector<u8> groomBytes, bindingBytes;
            ASSERT_TRUE(GroomSerializer::EncodeToBytes(*subject->Grown.Groom, groomBytes, reason)) << reason;
            ASSERT_TRUE(GroomBindingSerializer::EncodeToBytes(*subject->Binding, bindingBytes, reason)) << reason;

            // Loose: the files on disk, imported by the editor asset manager.
            const fs::path groomPath = assetsDir / (subject->Name + ".ologroom");
            const fs::path bindingPath = assetsDir / (subject->Name + ".ologroombinding");
            std::ofstream(groomPath, std::ios::binary)
                .write(reinterpret_cast<const char*>(groomBytes.data()), static_cast<std::streamsize>(groomBytes.size()));
            std::ofstream(bindingPath, std::ios::binary)
                .write(reinterpret_cast<const char*>(bindingBytes.data()),
                       static_cast<std::streamsize>(bindingBytes.size()));
            const AssetHandle looseGroom = editorAssets->ImportAsset(groomPath);
            const AssetHandle looseBinding = editorAssets->ImportAsset(bindingPath);
            ASSERT_NE(static_cast<u64>(looseGroom), 0u);
            ASSERT_NE(static_cast<u64>(looseBinding), 0u);
            auto loadedGroom = AssetManager::GetAsset<GroomAsset>(looseGroom);
            auto loadedBinding = AssetManager::GetAsset<GroomBindingAsset>(looseBinding);
            ASSERT_TRUE(loadedGroom);
            ASSERT_TRUE(loadedBinding);
            std::vector<u8> again;
            ASSERT_TRUE(GroomSerializer::EncodeToBytes(*loadedGroom, again, reason)) << reason;
            EXPECT_EQ(again, groomBytes) << "loose groom";
            ASSERT_TRUE(GroomBindingSerializer::EncodeToBytes(*loadedBinding, again, reason)) << reason;
            EXPECT_EQ(again, bindingBytes) << "loose binding";

            // Packed: through the same serializers the runtime's pack reader uses.
            const fs::path packPath = m_ProjectDir / (subject->Name + ".pack");
            AssetSerializationInfo groomInfo{}, bindingInfo{};
            {
                FileStreamWriter writer(packPath);
                ASSERT_TRUE(writer.IsStreamGood());
                ASSERT_TRUE(GroomSerializer{}.SerializeToAssetPack(looseGroom, writer, groomInfo));
                ASSERT_TRUE(GroomBindingSerializer{}.SerializeToAssetPack(looseBinding, writer, bindingInfo));
            }
            FileStreamReader reader(packPath);
            ASSERT_TRUE(reader.IsStreamGood());
            AssetPackFile::AssetInfo gi{};
            gi.Handle = looseGroom;
            gi.PackedOffset = groomInfo.Offset;
            gi.PackedSize = groomInfo.Size;
            gi.Type = AssetType::Groom;
            reader.SetStreamPosition(gi.PackedOffset);
            auto packedGroom = GroomSerializer{}.DeserializeFromAssetPack(reader, gi).As<GroomAsset>();
            AssetPackFile::AssetInfo bi{};
            bi.Handle = looseBinding;
            bi.PackedOffset = bindingInfo.Offset;
            bi.PackedSize = bindingInfo.Size;
            bi.Type = AssetType::GroomBinding;
            reader.SetStreamPosition(bi.PackedOffset);
            auto packedBinding = GroomBindingSerializer{}.DeserializeFromAssetPack(reader, bi).As<GroomBindingAsset>();
            ASSERT_TRUE(packedGroom);
            ASSERT_TRUE(packedBinding);
            ASSERT_TRUE(GroomSerializer::EncodeToBytes(*packedGroom, again, reason)) << reason;
            EXPECT_EQ(again, groomBytes) << "packed groom";
            ASSERT_TRUE(GroomBindingSerializer::EncodeToBytes(*packedBinding, again, reason)) << reason;
            EXPECT_EQ(again, bindingBytes) << "packed binding";

            // The scene now draws the PACK-LOADED assets.
            const AssetHandle pg = AssetManager::AddMemoryOnlyAsset<GroomAsset>(packedGroom);
            const AssetHandle pb = AssetManager::AddMemoryOnlyAsset<GroomBindingAsset>(packedBinding);
            subject->Coat.GetComponent<GroomComponent>().m_Groom = pg;
            subject->Coat.GetComponent<GroomBindingComponent>().m_Binding = pb;
        }

        // The body side of the flow: every body is loaded AGAIN, now from the
        // warm .omesh cache the first import wrote, and each cooked binding
        // must still accept it. Before #1223 fixed the mesh optimizer, a warm
        // load came back with rotated triangle corners and a moved topology
        // hash, and the editor drew the long coat at its bind pose while the
        // horse walked (AnimatedModelCacheSurfaceTest pins the mesh half).
        //
        // BOTH loads are forced: the cache is dropped first, so one reload is
        // cold and the next is warm. Otherwise, on any tree whose cache already
        // exists, the fixture's own load and the reload are both warm and this
        // passes with the fix reverted.
        for (SubjectRig* subject : Subjects())
        {
            const fs::path modelPath = subject == &m_Human ? HeadPath() : HorsePath();
            MeshCache::InvalidateCache(modelPath);
            MeshCache::InvalidateCache(modelPath, AnimatedModel::kCachePrefix);
            for (const char* which : { "cold", "warm" })
            {
                const Ref<AnimatedModel> reloaded = Ref<AnimatedModel>::Create(modelPath.string());
                ASSERT_TRUE(reloaded);
                ASSERT_FALSE(reloaded->GetMeshes().empty());
                const MeshSource& body = *reloaded->GetMeshes().front();
                const GroomSurfaceView view = MakeSurfaceView(body, reloaded->GetSkeleton().Raw());
                EXPECT_EQ(subject->Binding->CheckCompatibility(GroomBindingBuilder::SignGroom(*subject->Grown.Groom),
                                                               GroomBindingBuilder::SignTarget(view)),
                          GroomBindingRejectReason::None)
                    << subject->Name << ": the binding must accept a " << which << " load of its own body";
            }
        }

        SetPath(RenderingPath::Forward);
        const MotionResult motion = PlayFromStart(45);
        EXPECT_EQ(motion.Last.GroomsDrawn, 3u);
        EXPECT_EQ(motion.MinGroomsDeformed, 3u) << "the packed bindings attach to the live bodies";
        EXPECT_EQ(motion.MaxBindingRefused, 0u);
        for (SubjectRig* subject : Subjects())
        {
            const u32 coat = CoatPixels(*subject, ViewOf(*subject), "GroomAnimalsPacked_GL_Forward_" + subject->Tag, "");
            ASSERT_FALSE(HasFatalFailure());
            EXPECT_GT(Fraction(coat), 0.01) << subject->Tag << ": the packed coat draws";
        }
    }
    // =========================================================================
    // Not a test: the generator of the LIVE subject. With
    // OLO_GROOM_ANIMALS_EXPORT=1 it cooks the three coats and their bindings
    // into SandboxProject/Assets/Grooms/Animals/, registers them through the
    // Sandbox project's own EditorAssetManager (which writes AssetRegistry.oar
    // -- a binary file nothing else can author), and writes
    // Scenes/GroomAnimals.olo with SceneSerializer, so every key in it comes
    // from the reference generator rather than from a hand-typed guess.
    // =========================================================================
    TEST_F(GroomAnimalsAcceptanceEvidenceTest, ExportsTheLiveScene)
    {
        const char* flag = std::getenv("OLO_GROOM_ANIMALS_EXPORT");
        if (flag == nullptr || flag[0] != '1')
        {
            GTEST_SKIP() << "set OLO_GROOM_ANIMALS_EXPORT=1 to regenerate SandboxProject/Assets/Scenes/GroomAnimals.olo";
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

        const AssetHandle colorMap = assets->ImportAsset(assetDir / "Models" / "Horse" / "HorseAlbedo.jpg");
        ASSERT_NE(static_cast<u64>(colorMap), 0u);

        for (SubjectRig* subject : Subjects())
        {
            std::string reason;
            std::vector<u8> groomBytes, bindingBytes;
            ASSERT_TRUE(GroomSerializer::EncodeToBytes(*subject->Grown.Groom, groomBytes, reason)) << reason;
            ASSERT_TRUE(GroomBindingSerializer::EncodeToBytes(*subject->Binding, bindingBytes, reason)) << reason;
            const fs::path groomPath = groomDir / (subject->Name + ".ologroom");
            const fs::path bindingPath = groomDir / (subject->Name + ".ologroombinding");
            std::ofstream(groomPath, std::ios::binary)
                .write(reinterpret_cast<const char*>(groomBytes.data()), static_cast<std::streamsize>(groomBytes.size()));
            std::ofstream(bindingPath, std::ios::binary)
                .write(reinterpret_cast<const char*>(bindingBytes.data()),
                       static_cast<std::streamsize>(bindingBytes.size()));
            const AssetHandle groom = assets->ImportAsset(groomPath);
            const AssetHandle binding = assets->ImportAsset(bindingPath);
            ASSERT_NE(static_cast<u64>(groom), 0u) << groomPath.string();
            ASSERT_NE(static_cast<u64>(binding), 0u) << bindingPath.string();
            subject->Coat.GetComponent<GroomComponent>().m_Groom = groom;
            subject->Coat.GetComponent<GroomBindingComponent>().m_Binding = binding;
            if (subject != &m_Human)
            {
                subject->Coat.GetComponent<GroomCoatComponent>().m_ColorMap = colorMap;
            }
            std::printf("[groom-animals] exported %s: groom %llu (%zu bytes), binding %llu (%zu bytes)\n",
                        subject->Name.c_str(), static_cast<unsigned long long>(static_cast<u64>(groom)), groomBytes.size(),
                        static_cast<unsigned long long>(static_cast<u64>(binding)), bindingBytes.size());
        }
        ASSERT_TRUE(assets->SerializeAssetRegistry());

        // The bodies' materials come from their models: the scene loader
        // re-populates them on load, exactly as the other animated-model scenes
        // rely on. Serialising the import-time copy would write its absolute,
        // machine-specific texture paths into a committed scene.
        for (SubjectRig* subject : Subjects())
        {
            if (subject->Body.HasComponent<MaterialComponent>())
            {
                subject->Body.RemoveComponent<MaterialComponent>();
            }
        }

        const fs::path scenePath = assetDir / "Scenes" / "GroomAnimals.olo";
        SceneSerializer(GetSceneRef()).Serialize(scenePath);
        ASSERT_TRUE(fs::exists(scenePath));
        std::printf("[groom-animals] wrote %s\n", scenePath.string().c_str());

        // Leave a SCRATCH project active, never the Sandbox one: every later
        // fixture in this process takes BuildScene's "project already active"
        // branch, and would otherwise write its cooked test assets into
        // SandboxProject/Assets and its registry.
        const fs::path scratch = TempDir("after-export");
        fs::create_directories(scratch / "Assets", ec);
        {
            std::ofstream proj(scratch / "Scratch.oloproj");
            proj << "Project:\n"
                    "  Name: GroomAnimalsScratch\n"
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
