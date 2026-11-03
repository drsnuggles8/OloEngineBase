#include "OloEnginePCH.h"
#include "OloEngine/Renderer/AssimpSmoothNormals.h"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/mesh.h>
#include <assimp/scene.h>

#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Grows a per-vertex array from `oldCount` entries by one per copied vertex, each
        // repeating its source's entry. Assimp frees these arrays with delete[].
        template<typename T>
        void AppendVertexCopies(T*& data, u32 oldCount, const std::vector<u32>& copySources)
        {
            if (!data)
                return;
            T* grown = new T[oldCount + copySources.size()];
            std::copy(data, data + oldCount, grown);
            for (sizet i = 0; i < copySources.size(); ++i)
                grown[oldCount + i] = data[copySources[i]];
            delete[] data;
            data = grown;
        }

        [[nodiscard]] bool SameNormal(const aiVector3D& a, const aiVector3D& b)
        {
            return (a - b).SquareLength() < 1e-10f;
        }

        struct PositionHash
        {
            sizet operator()(const std::array<f32, 3>& p) const noexcept
            {
                sizet seed = std::hash<f32>{}(p[0]);
                seed ^= std::hash<f32>{}(p[1]) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
                seed ^= std::hash<f32>{}(p[2]) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
                return seed;
            }
        };
    } // namespace

    void GenerateMissingSmoothNormals(aiMesh& mesh, f32 maxSmoothingAngleDegrees)
    {
        if (mesh.mNormals || mesh.mNumVertices == 0 ||
            (mesh.mPrimitiveTypes & (aiPrimitiveType_TRIANGLE | aiPrimitiveType_POLYGON)) == 0)
        {
            return;
        }

        const u32 vertexCount = mesh.mNumVertices;
        // Assimp's limit semantics: from 175 degrees up, every corner at a position shares.
        const f32 cosLimit = (maxSmoothingAngleDegrees >= 175.0f) ? -2.0f : std::cos(glm::radians(maxSmoothingAngleDegrees));

        // Face normals as Assimp computes them, from the first, second and last corner.
        // A point, a line or a face indexing past the vertex array gets none.
        std::vector<aiVector3D> faceNormals(mesh.mNumFaces);
        std::vector<bool> faceHasNormal(mesh.mNumFaces, false);
        for (u32 f = 0; f < mesh.mNumFaces; ++f)
        {
            const aiFace& face = mesh.mFaces[f];
            if (face.mNumIndices < 3 ||
                !std::all_of(face.mIndices, face.mIndices + face.mNumIndices, [&](u32 i) { return i < vertexCount; }))
            {
                continue;
            }
            const aiVector3D& v1 = mesh.mVertices[face.mIndices[0]];
            const aiVector3D& v2 = mesh.mVertices[face.mIndices[1]];
            const aiVector3D& v3 = mesh.mVertices[face.mIndices[face.mNumIndices - 1]];
            faceNormals[f] = ((v2 - v1) ^ (v3 - v1)).NormalizeSafe();
            faceHasNormal[f] = true;
        }

        // The faces with a corner at each vertex position. Coincident vertices (a UV seam,
        // a split the source authored) smooth together, as in Assimp.
        // Flat arrays (faces of position p are positionFaces[positionStart[p] ..
        // positionStart[p + 1]]), since a normal-less scan can have millions of vertices.
        std::unordered_map<std::array<f32, 3>, u32, PositionHash> positionIds;
        positionIds.reserve(vertexCount);
        std::vector<u32> positionOf(vertexCount);
        for (u32 v = 0; v < vertexCount; ++v)
        {
            const aiVector3D& p = mesh.mVertices[v];
            positionOf[v] = positionIds.try_emplace({ p.x, p.y, p.z }, static_cast<u32>(positionIds.size())).first->second;
        }
        std::vector<u32> positionStart(positionIds.size() + 1, 0);
        for (u32 f = 0; f < mesh.mNumFaces; ++f)
        {
            if (!faceHasNormal[f])
                continue;
            const aiFace& face = mesh.mFaces[f];
            for (u32 k = 0; k < face.mNumIndices; ++k)
                ++positionStart[positionOf[face.mIndices[k]] + 1];
        }
        for (sizet p = 1; p < positionStart.size(); ++p)
            positionStart[p] += positionStart[p - 1];
        std::vector<u32> positionFaces(positionStart.back());
        std::vector<u32> positionFill(positionStart.begin(), positionStart.end() - 1);
        for (u32 f = 0; f < mesh.mNumFaces; ++f)
        {
            if (!faceHasNormal[f])
                continue;
            const aiFace& face = mesh.mFaces[f];
            for (u32 k = 0; k < face.mNumIndices; ++k)
                positionFaces[positionFill[positionOf[face.mIndices[k]]]++] = f;
        }

        // Each corner's normal; a vertex whose corners disagree is copied once per extra
        // normal and those corners re-pointed at the copies. A vertex no face reaches keeps
        // a NaN normal, which is what Assimp gives it.
        std::vector<aiVector3D> normals(vertexCount, aiVector3D(std::numeric_limits<f32>::quiet_NaN()));
        std::vector<bool> assigned(vertexCount, false);
        std::vector<u32> copySources;
        std::unordered_map<u32, std::vector<u32>> copiesOf;
        for (u32 f = 0; f < mesh.mNumFaces; ++f)
        {
            if (!faceHasNormal[f])
                continue;
            aiFace& face = mesh.mFaces[f];
            for (u32 k = 0; k < face.mNumIndices; ++k)
            {
                const u32 vertex = face.mIndices[k];
                aiVector3D sum;
                const u32 position = positionOf[vertex];
                for (u32 i = positionStart[position]; i < positionStart[position + 1]; ++i)
                {
                    const u32 other = positionFaces[i];
                    if (other == f || faceNormals[other] * faceNormals[f] >= cosLimit)
                        sum += faceNormals[other];
                }
                const aiVector3D normal = sum.NormalizeSafe();

                if (!assigned[vertex])
                {
                    normals[vertex] = normal;
                    assigned[vertex] = true;
                    continue;
                }
                if (SameNormal(normals[vertex], normal))
                    continue;
                std::vector<u32>& copies = copiesOf[vertex];
                const auto copy = std::ranges::find_if(copies, [&](u32 c) { return SameNormal(normals[c], normal); });
                if (copy != copies.end())
                {
                    face.mIndices[k] = *copy;
                    continue;
                }
                const u32 added = vertexCount + static_cast<u32>(copySources.size());
                copySources.push_back(vertex);
                copies.push_back(added);
                normals.push_back(normal);
                face.mIndices[k] = added;
            }
        }

        if (!copySources.empty())
        {
            const u32 grownCount = vertexCount + static_cast<u32>(copySources.size());
            AppendVertexCopies(mesh.mVertices, vertexCount, copySources);
            AppendVertexCopies(mesh.mTangents, vertexCount, copySources);
            AppendVertexCopies(mesh.mBitangents, vertexCount, copySources);
            for (auto*& colors : mesh.mColors)
                AppendVertexCopies(colors, vertexCount, copySources);
            for (auto*& uvs : mesh.mTextureCoords)
                AppendVertexCopies(uvs, vertexCount, copySources);

            for (u32 a = 0; a < mesh.mNumAnimMeshes; ++a)
            {
                aiAnimMesh& target = *mesh.mAnimMeshes[a];
                if (target.mNumVertices != vertexCount)
                    continue;
                AppendVertexCopies(target.mVertices, vertexCount, copySources);
                AppendVertexCopies(target.mNormals, vertexCount, copySources);
                AppendVertexCopies(target.mTangents, vertexCount, copySources);
                AppendVertexCopies(target.mBitangents, vertexCount, copySources);
                for (auto*& colors : target.mColors)
                    AppendVertexCopies(colors, vertexCount, copySources);
                for (auto*& uvs : target.mTextureCoords)
                    AppendVertexCopies(uvs, vertexCount, copySources);
                target.mNumVertices = grownCount;
            }

            for (u32 b = 0; b < mesh.mNumBones; ++b)
            {
                aiBone& bone = *mesh.mBones[b];
                std::vector<aiVertexWeight> weights(bone.mWeights, bone.mWeights + bone.mNumWeights);
                for (u32 w = 0; w < bone.mNumWeights; ++w)
                {
                    const aiVertexWeight weight = bone.mWeights[w];
                    const auto copies = copiesOf.find(weight.mVertexId);
                    if (copies == copiesOf.end())
                        continue;
                    for (u32 copy : copies->second)
                        weights.emplace_back(copy, weight.mWeight);
                }
                if (weights.size() == bone.mNumWeights)
                    continue;
                delete[] bone.mWeights;
                bone.mWeights = new aiVertexWeight[weights.size()];
                std::ranges::copy(weights, bone.mWeights);
                bone.mNumWeights = static_cast<u32>(weights.size());
            }

            mesh.mNumVertices = grownCount;
        }

        mesh.mNormals = new aiVector3D[normals.size()];
        std::ranges::copy(normals, mesh.mNormals);
    }

    void GenerateMissingSmoothNormals(Assimp::Importer& importer)
    {
        // The importer owns the scene and its post-processing steps edit it in place;
        // this is one more such step, between ReadFile and ApplyPostProcessing.
        auto* scene = const_cast<aiScene*>(importer.GetScene());
        if (!scene)
            return;
        const f32 maxAngle = importer.GetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, 175.0f);
        for (u32 i = 0; i < scene->mNumMeshes; ++i)
            GenerateMissingSmoothNormals(*scene->mMeshes[i], maxAngle);
    }

    const aiScene* ReadSceneWithSmoothNormals(Assimp::Importer& importer, const std::string& path, u32 flags)
    {
        if (!importer.ReadFile(path, 0))
            return nullptr;
        GenerateMissingSmoothNormals(importer);
        return importer.ApplyPostProcessing(flags);
    }
} // namespace OloEngine
