// OLO_TEST_LAYER: integration
#include "OloEnginePCH.h"

#include "ImportedCorpusFixtures.h"

#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/MeshSource.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests::ImportedCorpus
{
    namespace fs = std::filesystem;

    fs::path EditorRoot()
    {
        return fs::path{ OLO_TEST_EDITOR_ROOT };
    }

    fs::path CorpusManifestPath()
    {
        return EditorRoot() / "assets/tests/imported-corpus/corpus.yaml";
    }

    fs::path ModelPath(const char* relativeToModels)
    {
        return EditorRoot() / "assets/models" / relativeToModels;
    }

    void MakeColdImport(const fs::path& path)
    {
        MeshCache::InvalidateCache(path);
        MeshCache::InvalidateCache(path, AnimatedModel::kCachePrefix);
    }

    // ── GltfBuilder ──────────────────────────────────────────────────────────────────

    u32 GltfBuilder::AddAccessor(const void* data, sizet bytes, u32 componentType, const char* type, u32 count,
                                 const std::string& extraJson)
    {
        const auto offset = m_Bin.size();
        const auto* begin = static_cast<const u8*>(data);
        m_Bin.insert(m_Bin.end(), begin, begin + bytes);
        while (m_Bin.size() % 4 != 0)
        {
            m_Bin.push_back(0u);
        }

        const auto view = static_cast<u32>(m_BufferViews.size());
        std::ostringstream bv;
        bv << "{\"buffer\":0,\"byteOffset\":" << offset << ",\"byteLength\":" << bytes << "}";
        m_BufferViews.push_back(bv.str());

        std::ostringstream acc;
        acc << "{\"bufferView\":" << view << ",\"componentType\":" << componentType << ",\"count\":" << count
            << ",\"type\":\"" << type << "\"" << extraJson << "}";
        m_Accessors.push_back(acc.str());
        return static_cast<u32>(m_Accessors.size() - 1);
    }

    u32 GltfBuilder::AddFloats(const std::vector<f32>& values, const char* type, bool withMinMax)
    {
        const std::string t = type;
        const u32 components = t == "SCALAR" ? 1u : t == "VEC2" ? 2u
                                                : t == "VEC3"   ? 3u
                                                : t == "VEC4"   ? 4u
                                                                : 16u;
        const auto count = static_cast<u32>(values.size() / components);

        std::string extra;
        if (withMinMax)
        {
            // glTF requires min/max on POSITION; Assimp tolerates their absence, but a
            // well-formed file keeps the test about the property under test.
            std::vector<f32> lo(components, std::numeric_limits<f32>::max());
            std::vector<f32> hi(components, std::numeric_limits<f32>::lowest());
            for (u32 i = 0; i < count; ++i)
            {
                for (u32 c = 0; c < components; ++c)
                {
                    lo[c] = std::min(lo[c], values[i * components + c]);
                    hi[c] = std::max(hi[c], values[i * components + c]);
                }
            }
            std::ostringstream mm;
            mm << ",\"min\":[";
            for (u32 c = 0; c < components; ++c)
            {
                mm << (c ? "," : "") << lo[c];
            }
            mm << "],\"max\":[";
            for (u32 c = 0; c < components; ++c)
            {
                mm << (c ? "," : "") << hi[c];
            }
            mm << "]";
            extra = mm.str();
        }
        return AddAccessor(values.data(), values.size() * sizeof(f32), 5126u, type, count, extra);
    }

    u32 GltfBuilder::AddU16(const std::vector<u16>& values, const char* type)
    {
        const std::string t = type;
        const u32 components = t == "SCALAR" ? 1u : t == "VEC4" ? 4u
                                                                : 1u;
        return AddAccessor(values.data(), values.size() * sizeof(u16), 5123u, type,
                           static_cast<u32>(values.size() / components));
    }

    fs::path GltfBuilder::Write(const fs::path& dir, const std::string& name, const std::string& bodyJson) const
    {
        std::error_code ec;
        fs::create_directories(dir, ec);

        const fs::path bin = dir / (name + ".bin");
        {
            std::ofstream out(bin, std::ios::binary);
            out.write(reinterpret_cast<const char*>(m_Bin.data()), static_cast<std::streamsize>(m_Bin.size()));
        }

        std::ostringstream json;
        json << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"OloEngine-Tests ImportedCorpus (issue #1350)\"},"
             << bodyJson << ",\"buffers\":[{\"uri\":\"" << name << ".bin\",\"byteLength\":" << m_Bin.size()
             << "}],\"bufferViews\":[";
        for (sizet i = 0; i < m_BufferViews.size(); ++i)
        {
            json << (i ? "," : "") << m_BufferViews[i];
        }
        json << "],\"accessors\":[";
        for (sizet i = 0; i < m_Accessors.size(); ++i)
        {
            json << (i ? "," : "") << m_Accessors[i];
        }
        json << "]}";

        const fs::path gltf = dir / (name + ".gltf");
        std::ofstream out(gltf);
        out << json.str();
        return gltf;
    }

    // ── Minimal case A ───────────────────────────────────────────────────────────────

    namespace
    {
        void AppendQuadXY(std::vector<f32>& positions, std::vector<f32>& normals, std::vector<u16>& indices,
                          f32 x0, f32 y0, f32 x1, f32 y1)
        {
            const auto base = static_cast<u16>(positions.size() / 3);
            for (const auto& [x, y] : std::array<std::pair<f32, f32>, 4>{ { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } } })
            {
                positions.insert(positions.end(), { x, y, 0.0f });
                normals.insert(normals.end(), { 0.0f, 0.0f, 1.0f });
            }
            for (u16 const i : { 0, 1, 2, 2, 3, 0 })
            {
                indices.push_back(static_cast<u16>(base + i));
            }
        }

        // Column-major 4x4, the glTF layout.
        void AppendMat4(std::vector<f32>& out, const glm::mat4& m)
        {
            for (int c = 0; c < 4; ++c)
            {
                for (int r = 0; r < 4; ++r)
                {
                    out.push_back(m[c][r]);
                }
            }
        }
    } // namespace

    fs::path AuthorRigidAttachmentFixture(const fs::path& dir)
    {
        GltfBuilder b;

        // Body: strip rows at y = 0, 1, 2 (x = -0.1, +0.1), then the unweighted flap.
        std::vector<f32> bodyPos;
        std::vector<f32> bodyNrm;
        std::vector<u16> bodyIdx;
        std::vector<u16> joints;
        std::vector<f32> weights;
        for (int row = 0; row < 3; ++row)
        {
            for (f32 const x : { -0.1f, 0.1f })
            {
                bodyPos.insert(bodyPos.end(), { x, static_cast<f32>(row), 0.0f });
                bodyNrm.insert(bodyNrm.end(), { 0.0f, 0.0f, 1.0f });
                joints.insert(joints.end(), { 0, 1, 0, 0 });
                if (row == 0)
                {
                    weights.insert(weights.end(), { 1.0f, 0.0f, 0.0f, 0.0f });
                }
                else if (row == 1)
                {
                    weights.insert(weights.end(), { 0.5f, 0.5f, 0.0f, 0.0f });
                }
                else
                {
                    weights.insert(weights.end(), { 0.0f, 1.0f, 0.0f, 0.0f });
                }
            }
        }
        for (u16 const i : { 0, 1, 3, 3, 2, 0, 2, 3, 5, 5, 4, 2 })
        {
            bodyIdx.push_back(i);
        }
        // The flap: four vertices with ALL-ZERO weights.
        AppendQuadXY(bodyPos, bodyNrm, bodyIdx, kFlapMinX, 0.0f, kFlapMaxX, kFlapMaxY);
        for (int v = 0; v < 4; ++v)
        {
            joints.insert(joints.end(), { 0, 0, 0, 0 });
            weights.insert(weights.end(), { 0.0f, 0.0f, 0.0f, 0.0f });
        }

        const u32 aBodyPos = b.AddFloats(bodyPos, "VEC3", true);
        const u32 aBodyNrm = b.AddFloats(bodyNrm, "VEC3");
        const u32 aBodyIdx = b.AddU16(bodyIdx, "SCALAR");
        const u32 aJoints = b.AddU16(joints, "VEC4");
        const u32 aWeights = b.AddFloats(weights, "VEC4");

        // Blade: a quad centred on the Blade node's origin.
        std::vector<f32> bladePos;
        std::vector<f32> bladeNrm;
        std::vector<u16> bladeIdx;
        AppendQuadXY(bladePos, bladeNrm, bladeIdx, -kBladeHalfSize, -kBladeHalfSize, kBladeHalfSize, kBladeHalfSize);
        const u32 aBladePos = b.AddFloats(bladePos, "VEC3", true);
        const u32 aBladeNrm = b.AddFloats(bladeNrm, "VEC3");
        const u32 aBladeIdx = b.AddU16(bladeIdx, "SCALAR");

        // Inverse bind matrices: Root at the origin, Arm at (0, 1, 0).
        std::vector<f32> ibm;
        AppendMat4(ibm, glm::mat4(1.0f));
        AppendMat4(ibm, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -kArmPivotY, 0.0f)));
        const u32 aIbm = b.AddFloats(ibm, "MAT4");

        // Animation: Arm rotates 0 -> 90 degrees about +Z.
        const f32 s = std::sin(glm::radians(45.0f));
        const u32 aTimes = b.AddFloats({ 0.0f, kArmClipSeconds }, "SCALAR", true);
        const u32 aRots = b.AddFloats({ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, s, s }, "VEC4");

        std::ostringstream body;
        body << "\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],"
             << "\"nodes\":["
             << "{\"name\":\"Body\",\"mesh\":0,\"skin\":0},"
             << "{\"name\":\"Root\",\"children\":[2]},"
             << "{\"name\":\"Arm\",\"translation\":[0," << kArmPivotY << ",0],\"children\":[3]},"
             << "{\"name\":\"Blade\",\"mesh\":1,\"translation\":[" << kBladeOffset << ",0,0]}],"
             << "\"meshes\":["
             << "{\"name\":\"Body\",\"primitives\":[{\"attributes\":{\"POSITION\":" << aBodyPos
             << ",\"NORMAL\":" << aBodyNrm << ",\"JOINTS_0\":" << aJoints << ",\"WEIGHTS_0\":" << aWeights
             << "},\"indices\":" << aBodyIdx << ",\"material\":0}]},"
             << "{\"name\":\"Blade\",\"primitives\":[{\"attributes\":{\"POSITION\":" << aBladePos
             << ",\"NORMAL\":" << aBladeNrm << "},\"indices\":" << aBladeIdx << ",\"material\":1}]}],"
             << "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":" << aIbm << ",\"skeleton\":1}],"
             << "\"animations\":[{\"name\":\"Swing\",\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,"
                "\"path\":\"rotation\"}}],\"samplers\":[{\"input\":"
             << aTimes << ",\"output\":" << aRots << ",\"interpolation\":\"LINEAR\"}]}],"
             << "\"materials\":["
             << "{\"name\":\"BodyMat\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.8,0.8,1],"
                "\"metallicFactor\":0,\"roughnessFactor\":0.6}},"
             << "{\"name\":\"BladeMat\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.9,0.2,0.2,1],"
                "\"metallicFactor\":0,\"roughnessFactor\":0.4}}]";

        return b.Write(dir, "RigidAttachment", body.str());
    }

    std::vector<glm::vec3> BladeRestCorners()
    {
        const glm::vec3 c{ kBladeOffset, kArmPivotY, 0.0f };
        return { c + glm::vec3(-kBladeHalfSize, -kBladeHalfSize, 0.0f), c + glm::vec3(kBladeHalfSize, -kBladeHalfSize, 0.0f),
                 c + glm::vec3(kBladeHalfSize, kBladeHalfSize, 0.0f), c + glm::vec3(-kBladeHalfSize, kBladeHalfSize, 0.0f) };
    }

    glm::vec3 RotateAboutArmPivot(const glm::vec3& restPoint, f32 radians)
    {
        const glm::vec3 pivot{ 0.0f, kArmPivotY, 0.0f };
        const glm::mat4 r = glm::rotate(glm::mat4(1.0f), radians, glm::vec3(0.0f, 0.0f, 1.0f));
        return pivot + glm::vec3(r * glm::vec4(restPoint - pivot, 1.0f));
    }

    // ── Minimal case B ───────────────────────────────────────────────────────────────

    const char* ToString(InfluenceCase c)
    {
        switch (c)
        {
            case InfluenceCase::Valid:
                return "Valid";
            case InfluenceCase::NaNWeight:
                return "NaNWeight";
            case InfluenceCase::NegativeWeight:
                return "NegativeWeight";
            case InfluenceCase::ZeroSum:
                return "ZeroSum";
            case InfluenceCase::Unnormalised:
                return "Unnormalised";
            case InfluenceCase::AllNaN:
                return "AllNaN";
            case InfluenceCase::InfiniteWeight:
                return "InfiniteWeight";
            case InfluenceCase::Count:
                break;
        }
        return "?";
    }

    f32 InfluenceCaseX(u32 caseIndex)
    {
        return static_cast<f32>(caseIndex) * 0.5f;
    }

    fs::path AuthorInvalidInfluenceFixture(const fs::path& dir)
    {
        constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
        constexpr f32 kInf = std::numeric_limits<f32>::infinity();
        const std::array<std::array<f32, 4>, kInfluenceCaseCount> caseWeights = { {
            { 1.0f, 0.0f, 0.0f, 0.0f },  // Valid
            { 1.0f, kNaN, 0.0f, 0.0f },  // NaNWeight
            { 1.5f, -0.5f, 0.0f, 0.0f }, // NegativeWeight
            { 0.0f, 0.0f, 0.0f, 0.0f },  // ZeroSum
            { 2.0f, 2.0f, 0.0f, 0.0f },  // Unnormalised
            { kNaN, 0.0f, 0.0f, 0.0f },  // AllNaN
            { kInf, 1.0f, 0.0f, 0.0f },  // InfiniteWeight
        } };

        GltfBuilder b;
        std::vector<f32> pos;
        std::vector<f32> nrm;
        std::vector<u16> idx;
        std::vector<u16> joints;
        std::vector<f32> weights;
        for (u32 c = 0; c < kInfluenceCaseCount; ++c)
        {
            const f32 x = InfluenceCaseX(c);
            AppendQuadXY(pos, nrm, idx, x, 0.0f, x + 0.3f, 1.0f);
            for (int v = 0; v < 4; ++v)
            {
                joints.insert(joints.end(), { 0, 1, 0, 0 });
                weights.insert(weights.end(), caseWeights[c].begin(), caseWeights[c].end());
            }
        }

        const u32 aPos = b.AddFloats(pos, "VEC3", true);
        const u32 aNrm = b.AddFloats(nrm, "VEC3");
        const u32 aIdx = b.AddU16(idx, "SCALAR");
        const u32 aJoints = b.AddU16(joints, "VEC4");
        const u32 aWeights = b.AddFloats(weights, "VEC4");

        std::vector<f32> ibm;
        AppendMat4(ibm, glm::mat4(1.0f));
        AppendMat4(ibm, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -kArmPivotY, 0.0f)));
        const u32 aIbm = b.AddFloats(ibm, "MAT4");

        std::ostringstream body;
        body << "\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],"
             << "\"nodes\":["
             << "{\"name\":\"Strip\",\"mesh\":0,\"skin\":0},"
             << "{\"name\":\"Root\",\"children\":[2]},"
             << "{\"name\":\"Arm\",\"translation\":[0," << kArmPivotY << ",0]}],"
             << "\"meshes\":[{\"name\":\"Strip\",\"primitives\":[{\"attributes\":{\"POSITION\":" << aPos
             << ",\"NORMAL\":" << aNrm << ",\"JOINTS_0\":" << aJoints << ",\"WEIGHTS_0\":" << aWeights
             << "},\"indices\":" << aIdx << "}]}],"
             << "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":" << aIbm << ",\"skeleton\":1}]";

        return b.Write(dir, "InvalidInfluences", body.str());
    }

    fs::path AuthorSeamFixture(const fs::path& dir)
    {
        GltfBuilder builder;
        const u32 leftPosition = builder.AddFloats({ -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                     0.0f, 1.0f, 0.0f },
                                                   "VEC3", true);
        const u32 rightPosition = builder.AddFloats({ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                                      0.0f, 1.0f, 0.0f },
                                                    "VEC3", true);
        const u32 leftNormal = builder.AddFloats({ 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                                                   0.0f, 0.0f, 1.0f },
                                                 "VEC3");
        const u32 rightNormal = builder.AddFloats({ 0.0f, 0.2f, 0.979796f, 0.0f, 0.2f, 0.979796f,
                                                    0.0f, 0.2f, 0.979796f },
                                                  "VEC3");
        const u32 leftUv = builder.AddFloats({ 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f }, "VEC2");
        const u32 rightUv = builder.AddFloats({ 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f }, "VEC2");
        const u32 indices = builder.AddU16({ 0, 1, 2 }, "SCALAR");

        std::ostringstream body;
        body << "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
             << "\"meshes\":[{\"primitives\":["
             << "{\"attributes\":{\"POSITION\":" << leftPosition << ",\"NORMAL\":" << leftNormal
             << ",\"TEXCOORD_0\":" << leftUv << "},\"indices\":" << indices << ",\"material\":0},"
             << "{\"attributes\":{\"POSITION\":" << rightPosition << ",\"NORMAL\":" << rightNormal
             << ",\"TEXCOORD_0\":" << rightUv << "},\"indices\":" << indices << ",\"material\":1}]}],"
             << "\"materials\":[{\"name\":\"SeamLeft\"},{\"name\":\"SeamRight\"}]";
        return builder.Write(dir, "UvNormalSeam", body.str());
    }

    // ── CPU skinning ─────────────────────────────────────────────────────────────────

    glm::vec3 SkinPosition(const MeshSource& source, u32 vertexIndex, const std::vector<glm::mat4>& finalBoneMatrices)
    {
        const glm::vec3 p = source.GetVertices()[static_cast<i32>(vertexIndex)].Position;
        const auto& influences = source.GetBoneInfluences();
        if (vertexIndex >= static_cast<u32>(influences.Num()))
        {
            return p;
        }
        const BoneInfluence& inf = influences[static_cast<i32>(vertexIndex)];

        // SkeletalDeformation.glsl: OloTotalBoneWeight is an explicit sum, OloVertexIsSkinned
        // is total > OLO_MIN_TOTAL_BONE_WEIGHT (0.001), an out-of-range ID contributes
        // nothing but its weight still counts in position.w.
        const f32 total = inf.m_Weights[0] + inf.m_Weights[1] + inf.m_Weights[2] + inf.m_Weights[3];
        if (!(total > 0.001f))
        {
            return p;
        }
        glm::mat4 skin(0.0f);
        for (int i = 0; i < 4; ++i)
        {
            if (inf.m_BoneIDs[i] < finalBoneMatrices.size())
            {
                skin += finalBoneMatrices[inf.m_BoneIDs[i]] * inf.m_Weights[i];
            }
        }
        const glm::vec4 out = skin * glm::vec4(p, 1.0f);
        return glm::vec3(out);
    }
} // namespace OloEngine::Tests::ImportedCorpus
