#include "OloEnginePCH.h"
#include "MeshBinarySerializer.h"
#include "MeshBinaryFormat.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/MorphTargets/MorphTarget.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <cstring>
#include <fstream>

namespace OloEngine
{
    // ========================================================================
    // Internal helpers
    // ========================================================================

    namespace
    {
        void WriteBytes(std::ofstream& out, const void* data, sizet size)
        {
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }

        void ReadBytes(std::ifstream& in, void* data, sizet size)
        {
            in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
        }

        void WriteString(std::ofstream& out, const std::string& str)
        {
            auto len = static_cast<u32>(str.size());
            WriteBytes(out, &len, sizeof(u32));
            if (len > 0)
            {
                WriteBytes(out, str.data(), len);
            }
        }

        std::string ReadString(std::ifstream& in)
        {
            u32 len = 0;
            ReadBytes(in, &len, sizeof(u32));
            if (len == 0)
            {
                return {};
            }
            std::string result(len, '\0');
            ReadBytes(in, result.data(), len);
            return result;
        }

        void WriteMat4(std::ofstream& out, const glm::mat4& m)
        {
            WriteBytes(out, &m[0][0], sizeof(f32) * 16);
        }

        glm::mat4 ReadMat4(std::ifstream& in)
        {
            glm::mat4 m(1.0f);
            ReadBytes(in, &m[0][0], sizeof(f32) * 16);
            return m;
        }

        u64 StreamPos(std::ofstream& out)
        {
            return static_cast<u64>(out.tellp());
        }

        u64 StreamPos(std::ifstream& in)
        {
            return static_cast<u64>(in.tellg());
        }
    } // anonymous namespace

    // ========================================================================
    // MeshBinarySerializer::Write
    // ========================================================================

    bool MeshBinarySerializer::Write(const std::filesystem::path& path, const MeshSource& meshSource, u64 sourceTimestamp)
    {
        OLO_PROFILE_FUNCTION();

        // Ensure parent directory exists
        if (auto const parentDir = path.parent_path(); !parentDir.empty())
        {
            std::filesystem::create_directories(parentDir);
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Write: Failed to open '{}' for writing", path.string());
            return false;
        }

        // Reserve space for header + directory (will be patched at the end)
        OMeshFormat::FileHeader header;
        header.SourceTimestamp = sourceTimestamp;

        OMeshFormat::SectionDirectory directory;

        auto const headerSize = sizeof(OMeshFormat::FileHeader) + sizeof(OMeshFormat::SectionDirectory);
        WriteBytes(out, &header, sizeof(header));
        WriteBytes(out, &directory, sizeof(directory));

        const auto& vertices = meshSource.GetVertices();
        const auto& indices = meshSource.GetIndices();
        const auto& submeshes = meshSource.GetSubmeshes();
        const auto& materials = meshSource.GetMaterials();

        // ── Geometry Section ──
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Geometry)].Offset = StreamPos(out);

            OMeshFormat::GeometryHeader geo;
            geo.VertexCount = static_cast<u32>(vertices.Num());
            geo.IndexCount = static_cast<u32>(indices.Num());
            geo.VertexStride = sizeof(Vertex);
            geo.ShadowIndexCount = static_cast<u32>(meshSource.GetShadowIndices().Num());

            // Encode vertex buffer
            EncodedMeshBuffer encodedVB;
            if (geo.VertexCount > 0)
            {
                encodedVB = MeshOptimization::EncodeVertexBuffer(
                    vertices.GetData(), geo.VertexCount, sizeof(Vertex));
            }
            geo.EncodedVertexSize = encodedVB.Data.size();

            // Encode index buffer
            EncodedMeshBuffer encodedIB;
            if (geo.IndexCount > 0)
            {
                encodedIB = MeshOptimization::EncodeIndexBuffer(
                    indices.GetData(), geo.IndexCount, geo.VertexCount);
            }
            geo.EncodedIndexSize = encodedIB.Data.size();

            // Encode shadow index buffer
            EncodedMeshBuffer encodedShadow;
            if (geo.ShadowIndexCount > 0)
            {
                encodedShadow = MeshOptimization::EncodeIndexBuffer(
                    meshSource.GetShadowIndices().GetData(), geo.ShadowIndexCount, geo.VertexCount);
            }
            geo.EncodedShadowIndexSize = encodedShadow.Data.size();

            WriteBytes(out, &geo, sizeof(geo));
            if (!encodedVB.Data.empty())
            {
                WriteBytes(out, encodedVB.Data.data(), encodedVB.Data.size());
            }
            if (!encodedIB.Data.empty())
            {
                WriteBytes(out, encodedIB.Data.data(), encodedIB.Data.size());
            }
            if (!encodedShadow.Data.empty())
            {
                WriteBytes(out, encodedShadow.Data.data(), encodedShadow.Data.size());
            }

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Geometry)].Size =
                StreamPos(out) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Geometry)].Offset;
        }

        // ── Submesh Section ──
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Submeshes)].Offset = StreamPos(out);

            OMeshFormat::SubmeshHeader subHeader;
            subHeader.SubmeshCount = static_cast<u32>(submeshes.Num());
            WriteBytes(out, &subHeader, sizeof(subHeader));

            for (i32 i = 0; i < submeshes.Num(); ++i)
            {
                const auto& sub = submeshes[i];

                OMeshFormat::SubmeshEntry entry{};
                std::memcpy(entry.Transform, &sub.m_Transform[0][0], sizeof(f32) * 16);
                std::memcpy(entry.LocalTransform, &sub.m_LocalTransform[0][0], sizeof(f32) * 16);
                entry.BoundsMin[0] = sub.m_BoundingBox.Min.x;
                entry.BoundsMin[1] = sub.m_BoundingBox.Min.y;
                entry.BoundsMin[2] = sub.m_BoundingBox.Min.z;
                entry.BoundsMax[0] = sub.m_BoundingBox.Max.x;
                entry.BoundsMax[1] = sub.m_BoundingBox.Max.y;
                entry.BoundsMax[2] = sub.m_BoundingBox.Max.z;
                entry.BaseVertex = sub.m_BaseVertex;
                entry.BaseIndex = sub.m_BaseIndex;
                entry.MaterialIndex = sub.m_MaterialIndex;
                entry.IndexCount = sub.m_IndexCount;
                entry.VertexCount = sub.m_VertexCount;
                entry.IsRigged = sub.m_IsRigged ? 1 : 0;

                WriteBytes(out, &entry, sizeof(entry));
                WriteString(out, sub.m_NodeName);
                WriteString(out, sub.m_MeshName);
            }

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Submeshes)].Size =
                StreamPos(out) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Submeshes)].Offset;
        }

        // ── Material Section ──
        if (!materials.IsEmpty())
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Materials)].Offset = StreamPos(out);

            OMeshFormat::MaterialHeader matHeader;
            matHeader.MaterialCount = static_cast<u32>(materials.Num());
            WriteBytes(out, &matHeader, sizeof(matHeader));

            for (const auto& [index, handle] : materials)
            {
                OMeshFormat::MaterialEntry entry;
                entry.Index = index;
                entry.Handle = static_cast<u64>(handle);
                WriteBytes(out, &entry, sizeof(entry));
            }

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Materials)].Size =
                StreamPos(out) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Materials)].Offset;
        }

        // ── Skeleton Section ──
        if (meshSource.HasSkeleton())
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Skeleton)].Offset = StreamPos(out);

            const auto* skeleton = meshSource.GetSkeleton();
            auto boneCount = static_cast<u32>(skeleton->m_BoneNames.size());

            OMeshFormat::SkeletonHeader skelHeader;
            skelHeader.BoneCount = boneCount;
            WriteBytes(out, &skelHeader, sizeof(skelHeader));

            // Parent indices
            WriteBytes(out, skeleton->m_ParentIndices.data(), boneCount * sizeof(i32));

            // Transform arrays (7 arrays of mat4)
            auto const writeMat4Array = [&](const std::vector<glm::mat4>& arr)
            {
                if (arr.size() >= boneCount)
                {
                    WriteBytes(out, arr.data(), boneCount * sizeof(glm::mat4));
                }
                else
                {
                    // Pad with identity if undersized
                    for (u32 j = 0; j < boneCount; ++j)
                    {
                        if (j < arr.size())
                        {
                            WriteMat4(out, arr[j]);
                        }
                        else
                        {
                            glm::mat4 identity(1.0f);
                            WriteMat4(out, identity);
                        }
                    }
                }
            };

            writeMat4Array(skeleton->m_LocalTransforms);
            writeMat4Array(skeleton->m_GlobalTransforms);
            writeMat4Array(skeleton->m_BindPoseMatrices);
            writeMat4Array(skeleton->m_InverseBindPoses);
            writeMat4Array(skeleton->m_BindPoseLocalTransforms);
            writeMat4Array(skeleton->m_BonePreTransforms);

            // Bone names (string table)
            for (u32 j = 0; j < boneCount; ++j)
            {
                if (j < skeleton->m_BoneNames.size())
                {
                    WriteString(out, skeleton->m_BoneNames[j]);
                }
                else
                {
                    WriteString(out, {});
                }
            }

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Skeleton)].Size =
                StreamPos(out) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Skeleton)].Offset;
        }

        // ── BoneInfluence Section ──
        if (meshSource.HasBoneInfluences())
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfluences)].Offset = StreamPos(out);

            const auto& boneInfluences = meshSource.GetBoneInfluences();
            auto influenceCount = static_cast<u32>(boneInfluences.Num());

            OMeshFormat::BoneInfluenceHeader biHeader;
            biHeader.InfluenceCount = influenceCount;
            biHeader.InfluenceStride = sizeof(BoneInfluence);

            EncodedMeshBuffer encoded;
            if (influenceCount > 0)
            {
                encoded = MeshOptimization::EncodeVertexBuffer(
                    boneInfluences.GetData(), influenceCount, sizeof(BoneInfluence));
            }
            biHeader.EncodedSize = encoded.Data.size();

            WriteBytes(out, &biHeader, sizeof(biHeader));
            if (!encoded.Data.empty())
            {
                WriteBytes(out, encoded.Data.data(), encoded.Data.size());
            }

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfluences)].Size =
                StreamPos(out) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfluences)].Offset;
        }

        // ── BoneInfo Section ──
        {
            const auto& boneInfo = meshSource.GetBoneInfo();
            if (!boneInfo.IsEmpty())
            {
                directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfo)].Offset = StreamPos(out);

                OMeshFormat::BoneInfoHeader biHeader;
                biHeader.BoneInfoCount = static_cast<u32>(boneInfo.Num());
                WriteBytes(out, &biHeader, sizeof(biHeader));

                for (i32 i = 0; i < boneInfo.Num(); ++i)
                {
                    OMeshFormat::BoneInfoEntry entry{};
                    std::memcpy(entry.InverseBindPose, &boneInfo[i].m_InverseBindPose[0][0], sizeof(f32) * 16);
                    entry.BoneIndex = boneInfo[i].m_BoneIndex;
                    WriteBytes(out, &entry, sizeof(entry));
                }

                directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfo)].Size =
                    StreamPos(out) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfo)].Offset;
            }
        }

        // ── MorphTarget Section ──
        if (meshSource.HasMorphTargets())
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::MorphTargets)].Offset = StreamPos(out);

            const auto& morphTargets = meshSource.GetMorphTargets();
            auto targetCount = morphTargets->GetTargetCount();
            auto vertCount = morphTargets->GetVertexCount();

            OMeshFormat::MorphTargetHeader mtHeader;
            mtHeader.TargetCount = targetCount;
            mtHeader.VertexCount = vertCount;
            WriteBytes(out, &mtHeader, sizeof(mtHeader));

            for (u32 t = 0; t < targetCount; ++t)
            {
                const auto& target = morphTargets->Targets[t];

                OMeshFormat::MorphTargetEntry entry{};
                entry.SparseEntryCount = target.IsSparse ? static_cast<u32>(target.SparseVertices.size()) : 0;
                WriteBytes(out, &entry, sizeof(entry));

                WriteString(out, target.Name);

                if (target.IsSparse)
                {
                    for (const auto& sparse : target.SparseVertices)
                    {
                        WriteBytes(out, &sparse.VertexIndex, sizeof(u32));
                        WriteBytes(out, &sparse.Delta, sizeof(MorphTargetVertex));
                    }
                }
                else
                {
                    if (!target.Vertices.empty())
                    {
                        WriteBytes(out, target.Vertices.data(), target.Vertices.size() * sizeof(MorphTargetVertex));
                    }
                }
            }

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::MorphTargets)].Size =
                StreamPos(out) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::MorphTargets)].Offset;
        }

        // Patch header with final file size
        header.TotalFileSize = StreamPos(out);

        // Write patched header + directory
        out.seekp(0);
        WriteBytes(out, &header, sizeof(header));
        WriteBytes(out, &directory, sizeof(directory));

        out.close();

        OLO_CORE_TRACE("MeshBinarySerializer::Write: Wrote '{}' ({} bytes, {} verts, {} indices)",
            path.filename().string(), header.TotalFileSize,
            vertices.Num(), indices.Num());

        return true;
    }

    // ========================================================================
    // MeshBinarySerializer::Read
    // ========================================================================

    Ref<MeshSource> MeshBinarySerializer::Read(const std::filesystem::path& path)
    {
        OLO_PROFILE_FUNCTION();

        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to open '{}'", path.string());
            return nullptr;
        }

        // Read header
        OMeshFormat::FileHeader header;
        ReadBytes(in, &header, sizeof(header));

        if (header.Magic != OMeshFormat::MagicNumber)
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Read: Invalid magic number in '{}'", path.string());
            return nullptr;
        }
        if (header.Version != OMeshFormat::CurrentVersion)
        {
            OLO_CORE_WARN("MeshBinarySerializer::Read: Version mismatch in '{}' (got {}, expected {})",
                path.string(), header.Version, OMeshFormat::CurrentVersion);
            return nullptr;
        }

        // Read section directory
        OMeshFormat::SectionDirectory directory;
        ReadBytes(in, &directory, sizeof(directory));

        TArray<Vertex> vertices;
        TArray<u32> indices;
        TArray<u32> shadowIndices;

        // ── Geometry Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Geometry)];
            if (sec.Size > 0)
            {
                in.seekg(static_cast<std::streamoff>(sec.Offset));

                OMeshFormat::GeometryHeader geo;
                ReadBytes(in, &geo, sizeof(geo));

                // Decode vertices
                if (geo.VertexCount > 0 && geo.EncodedVertexSize > 0)
                {
                    EncodedMeshBuffer encoded;
                    encoded.Data.resize(static_cast<sizet>(geo.EncodedVertexSize));
                    encoded.OriginalSize = geo.VertexCount * geo.VertexStride;
                    ReadBytes(in, encoded.Data.data(), encoded.Data.size());

                    vertices.SetNum(static_cast<i32>(geo.VertexCount));
                    if (!MeshOptimization::DecodeVertexBuffer(vertices.GetData(), geo.VertexCount, geo.VertexStride, encoded))
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to decode vertex buffer");
                        return nullptr;
                    }
                }

                // Decode indices
                if (geo.IndexCount > 0 && geo.EncodedIndexSize > 0)
                {
                    EncodedMeshBuffer encoded;
                    encoded.Data.resize(static_cast<sizet>(geo.EncodedIndexSize));
                    encoded.OriginalSize = geo.IndexCount * sizeof(u32);
                    ReadBytes(in, encoded.Data.data(), encoded.Data.size());

                    indices.SetNum(static_cast<i32>(geo.IndexCount));
                    if (!MeshOptimization::DecodeIndexBuffer(indices.GetData(), geo.IndexCount, encoded))
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to decode index buffer");
                        return nullptr;
                    }
                }

                // Decode shadow indices
                if (geo.ShadowIndexCount > 0 && geo.EncodedShadowIndexSize > 0)
                {
                    EncodedMeshBuffer encoded;
                    encoded.Data.resize(static_cast<sizet>(geo.EncodedShadowIndexSize));
                    encoded.OriginalSize = geo.ShadowIndexCount * sizeof(u32);
                    ReadBytes(in, encoded.Data.data(), encoded.Data.size());

                    shadowIndices.SetNum(static_cast<i32>(geo.ShadowIndexCount));
                    if (!MeshOptimization::DecodeIndexBuffer(shadowIndices.GetData(), geo.ShadowIndexCount, encoded))
                    {
                        OLO_CORE_WARN("MeshBinarySerializer::Read: Failed to decode shadow indices (non-fatal)");
                        shadowIndices.Empty();
                    }
                }
            }
        }

        auto meshSource = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));

        // Transfer shadow indices
        if (!shadowIndices.IsEmpty())
        {
            meshSource->GetShadowIndices() = MoveTemp(shadowIndices);
        }

        // ── Submesh Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Submeshes)];
            if (sec.Size > 0)
            {
                in.seekg(static_cast<std::streamoff>(sec.Offset));

                OMeshFormat::SubmeshHeader subHeader;
                ReadBytes(in, &subHeader, sizeof(subHeader));

                for (u32 i = 0; i < subHeader.SubmeshCount; ++i)
                {
                    OMeshFormat::SubmeshEntry entry{};
                    ReadBytes(in, &entry, sizeof(entry));

                    Submesh sub;
                    std::memcpy(&sub.m_Transform[0][0], entry.Transform, sizeof(f32) * 16);
                    std::memcpy(&sub.m_LocalTransform[0][0], entry.LocalTransform, sizeof(f32) * 16);
                    sub.m_BoundingBox.Min = { entry.BoundsMin[0], entry.BoundsMin[1], entry.BoundsMin[2] };
                    sub.m_BoundingBox.Max = { entry.BoundsMax[0], entry.BoundsMax[1], entry.BoundsMax[2] };
                    sub.m_BaseVertex = entry.BaseVertex;
                    sub.m_BaseIndex = entry.BaseIndex;
                    sub.m_MaterialIndex = entry.MaterialIndex;
                    sub.m_IndexCount = entry.IndexCount;
                    sub.m_VertexCount = entry.VertexCount;
                    sub.m_IsRigged = entry.IsRigged != 0;
                    sub.m_NodeName = ReadString(in);
                    sub.m_MeshName = ReadString(in);

                    meshSource->AddSubmesh(sub);
                }
            }
        }

        // ── Material Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Materials)];
            if (sec.Size > 0)
            {
                in.seekg(static_cast<std::streamoff>(sec.Offset));

                OMeshFormat::MaterialHeader matHeader;
                ReadBytes(in, &matHeader, sizeof(matHeader));

                for (u32 i = 0; i < matHeader.MaterialCount; ++i)
                {
                    OMeshFormat::MaterialEntry entry;
                    ReadBytes(in, &entry, sizeof(entry));
                    meshSource->SetMaterial(entry.Index, AssetHandle{ entry.Handle });
                }
            }
        }

        // ── Skeleton Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Skeleton)];
            if (sec.Size > 0)
            {
                in.seekg(static_cast<std::streamoff>(sec.Offset));

                OMeshFormat::SkeletonHeader skelHeader;
                ReadBytes(in, &skelHeader, sizeof(skelHeader));
                u32 const boneCount = skelHeader.BoneCount;

                auto skeleton = Ref<Skeleton>::Create(static_cast<sizet>(boneCount));

                // Parent indices
                ReadBytes(in, skeleton->m_ParentIndices.data(), boneCount * sizeof(i32));

                // Transform arrays
                auto const readMat4Array = [&](std::vector<glm::mat4>& arr)
                {
                    arr.resize(boneCount);
                    ReadBytes(in, arr.data(), boneCount * sizeof(glm::mat4));
                };

                readMat4Array(skeleton->m_LocalTransforms);
                readMat4Array(skeleton->m_GlobalTransforms);
                readMat4Array(skeleton->m_BindPoseMatrices);
                readMat4Array(skeleton->m_InverseBindPoses);
                readMat4Array(skeleton->m_BindPoseLocalTransforms);
                readMat4Array(skeleton->m_BonePreTransforms);

                // Bone names
                skeleton->m_BoneNames.resize(boneCount);
                for (u32 j = 0; j < boneCount; ++j)
                {
                    skeleton->m_BoneNames[j] = ReadString(in);
                }

                // FinalBoneMatrices initialized to identity by SkeletonData constructor
                skeleton->m_FinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));

                meshSource->SetSkeleton(skeleton);
            }
        }

        // ── BoneInfluence Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfluences)];
            if (sec.Size > 0)
            {
                in.seekg(static_cast<std::streamoff>(sec.Offset));

                OMeshFormat::BoneInfluenceHeader biHeader;
                ReadBytes(in, &biHeader, sizeof(biHeader));

                if (biHeader.InfluenceCount > 0 && biHeader.EncodedSize > 0)
                {
                    EncodedMeshBuffer encoded;
                    encoded.Data.resize(static_cast<sizet>(biHeader.EncodedSize));
                    encoded.OriginalSize = biHeader.InfluenceCount * biHeader.InfluenceStride;
                    ReadBytes(in, encoded.Data.data(), encoded.Data.size());

                    auto& boneInfluences = meshSource->GetBoneInfluences();
                    boneInfluences.SetNum(static_cast<i32>(biHeader.InfluenceCount));
                    if (!MeshOptimization::DecodeVertexBuffer(boneInfluences.GetData(),
                            biHeader.InfluenceCount, biHeader.InfluenceStride, encoded))
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to decode bone influences");
                    }
                }
            }
        }

        // ── BoneInfo Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfo)];
            if (sec.Size > 0)
            {
                in.seekg(static_cast<std::streamoff>(sec.Offset));

                OMeshFormat::BoneInfoHeader biHeader;
                ReadBytes(in, &biHeader, sizeof(biHeader));

                auto& boneInfo = meshSource->GetBoneInfo();
                for (u32 i = 0; i < biHeader.BoneInfoCount; ++i)
                {
                    OMeshFormat::BoneInfoEntry entry{};
                    ReadBytes(in, &entry, sizeof(entry));

                    BoneInfo info;
                    std::memcpy(&info.m_InverseBindPose[0][0], entry.InverseBindPose, sizeof(f32) * 16);
                    info.m_BoneIndex = entry.BoneIndex;
                    boneInfo.Add(info);
                }
            }
        }

        // ── MorphTarget Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::MorphTargets)];
            if (sec.Size > 0)
            {
                in.seekg(static_cast<std::streamoff>(sec.Offset));

                OMeshFormat::MorphTargetHeader mtHeader;
                ReadBytes(in, &mtHeader, sizeof(mtHeader));

                auto morphTargetSet = Ref<MorphTargetSet>::Create();

                for (u32 t = 0; t < mtHeader.TargetCount; ++t)
                {
                    OMeshFormat::MorphTargetEntry entry{};
                    ReadBytes(in, &entry, sizeof(entry));

                    MorphTarget target;
                    target.Name = ReadString(in);

                    if (entry.SparseEntryCount > 0)
                    {
                        target.IsSparse = true;
                        target.SparseVertices.resize(entry.SparseEntryCount);
                        for (u32 s = 0; s < entry.SparseEntryCount; ++s)
                        {
                            ReadBytes(in, &target.SparseVertices[s].VertexIndex, sizeof(u32));
                            ReadBytes(in, &target.SparseVertices[s].Delta, sizeof(MorphTargetVertex));
                        }
                    }
                    else
                    {
                        target.IsSparse = false;
                        target.Vertices.resize(mtHeader.VertexCount);
                        if (mtHeader.VertexCount > 0)
                        {
                            ReadBytes(in, target.Vertices.data(),
                                mtHeader.VertexCount * sizeof(MorphTargetVertex));
                        }
                    }

                    morphTargetSet->AddTarget(std::move(target));
                }

                meshSource->SetMorphTargets(morphTargetSet);
            }
        }

        OLO_CORE_TRACE("MeshBinarySerializer::Read: Loaded '{}' ({} verts, {} indices, {} submeshes)",
            path.filename().string(),
            meshSource->GetVertices().Num(),
            meshSource->GetIndices().Num(),
            meshSource->GetSubmeshes().Num());

        return meshSource;
    }

    // ========================================================================
    // MeshBinarySerializer::ReadTimestamp
    // ========================================================================

    bool MeshBinarySerializer::ReadTimestamp(const std::filesystem::path& path, u64& outSourceTimestamp)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
        {
            return false;
        }

        OMeshFormat::FileHeader header;
        ReadBytes(in, &header, sizeof(header));

        if (header.Magic != OMeshFormat::MagicNumber || header.Version != OMeshFormat::CurrentVersion)
        {
            return false;
        }

        outSourceTimestamp = header.SourceTimestamp;
        return true;
    }

    // ========================================================================
    // AnimationBinarySerializer::Write
    // ========================================================================

    bool AnimationBinarySerializer::Write(const std::filesystem::path& path,
        const std::vector<Ref<AnimationClip>>& clips, u64 sourceTimestamp)
    {
        OLO_PROFILE_FUNCTION();

        if (auto const parentDir = path.parent_path(); !parentDir.empty())
        {
            std::filesystem::create_directories(parentDir);
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            OLO_CORE_ERROR("AnimationBinarySerializer::Write: Failed to open '{}' for writing", path.string());
            return false;
        }

        OAnimFormat::FileHeader header;
        header.SourceTimestamp = sourceTimestamp;

        // Reserve space for header
        WriteBytes(out, &header, sizeof(header));

        auto clipCount = static_cast<u32>(clips.size());
        WriteBytes(out, &clipCount, sizeof(u32));

        // Reserve space for clip directory
        auto directoryOffset = StreamPos(out);
        std::vector<OAnimFormat::ClipDirectoryEntry> directory(clipCount);
        WriteBytes(out, directory.data(), clipCount * sizeof(OAnimFormat::ClipDirectoryEntry));

        // Write each clip
        for (u32 i = 0; i < clipCount; ++i)
        {
            const auto& clip = clips[i];
            if (!clip)
            {
                continue;
            }

            directory[i].Offset = StreamPos(out);

            OAnimFormat::ClipHeader clipHeader;
            clipHeader.Duration = clip->Duration;
            clipHeader.BoneChannelCount = static_cast<u32>(clip->BoneAnimations.size());
            clipHeader.MorphKeyframeCount = static_cast<u32>(clip->MorphKeyframes.size());
            WriteBytes(out, &clipHeader, sizeof(clipHeader));

            WriteString(out, clip->Name);

            // Write bone channels
            for (const auto& boneAnim : clip->BoneAnimations)
            {
                OAnimFormat::BoneChannelHeader chanHeader;
                chanHeader.PositionKeyCount = static_cast<u32>(boneAnim.PositionKeys.size());
                chanHeader.RotationKeyCount = static_cast<u32>(boneAnim.RotationKeys.size());
                chanHeader.ScaleKeyCount = static_cast<u32>(boneAnim.ScaleKeys.size());
                WriteBytes(out, &chanHeader, sizeof(chanHeader));

                WriteString(out, boneAnim.BoneName);

                // Position keys
                for (const auto& key : boneAnim.PositionKeys)
                {
                    OAnimFormat::PositionKey pk{};
                    pk.Time = key.Time;
                    pk.Position[0] = key.Position.x;
                    pk.Position[1] = key.Position.y;
                    pk.Position[2] = key.Position.z;
                    WriteBytes(out, &pk, sizeof(pk));
                }

                // Rotation keys
                for (const auto& key : boneAnim.RotationKeys)
                {
                    OAnimFormat::RotationKey rk{};
                    rk.Time = key.Time;
                    rk.Rotation[0] = key.Rotation.w;
                    rk.Rotation[1] = key.Rotation.x;
                    rk.Rotation[2] = key.Rotation.y;
                    rk.Rotation[3] = key.Rotation.z;
                    WriteBytes(out, &rk, sizeof(rk));
                }

                // Scale keys
                for (const auto& key : boneAnim.ScaleKeys)
                {
                    OAnimFormat::ScaleKey sk{};
                    sk.Time = key.Time;
                    sk.Scale[0] = key.Scale.x;
                    sk.Scale[1] = key.Scale.y;
                    sk.Scale[2] = key.Scale.z;
                    WriteBytes(out, &sk, sizeof(sk));
                }
            }

            // Write morph keyframes
            for (const auto& mk : clip->MorphKeyframes)
            {
                OAnimFormat::MorphKeyframe mkData{};
                mkData.Time = mk.Time;
                mkData.Weight = mk.Weight;
                WriteBytes(out, &mkData, sizeof(mkData));
                WriteString(out, mk.TargetName);
            }

            directory[i].Size = StreamPos(out) - directory[i].Offset;
        }

        // Patch header
        header.TotalFileSize = StreamPos(out);
        out.seekp(0);
        WriteBytes(out, &header, sizeof(header));

        // Patch directory
        out.seekp(static_cast<std::streamoff>(directoryOffset));
        WriteBytes(out, directory.data(), clipCount * sizeof(OAnimFormat::ClipDirectoryEntry));

        out.close();

        OLO_CORE_TRACE("AnimationBinarySerializer::Write: Wrote '{}' ({} clips, {} bytes)",
            path.filename().string(), clipCount, header.TotalFileSize);

        return true;
    }

    // ========================================================================
    // AnimationBinarySerializer::Read
    // ========================================================================

    std::vector<Ref<AnimationClip>> AnimationBinarySerializer::Read(const std::filesystem::path& path)
    {
        OLO_PROFILE_FUNCTION();

        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
        {
            OLO_CORE_ERROR("AnimationBinarySerializer::Read: Failed to open '{}'", path.string());
            return {};
        }

        OAnimFormat::FileHeader header;
        ReadBytes(in, &header, sizeof(header));

        if (header.Magic != OAnimFormat::MagicNumber)
        {
            OLO_CORE_ERROR("AnimationBinarySerializer::Read: Invalid magic number in '{}'", path.string());
            return {};
        }
        if (header.Version != OAnimFormat::CurrentVersion)
        {
            OLO_CORE_WARN("AnimationBinarySerializer::Read: Version mismatch in '{}' (got {}, expected {})",
                path.string(), header.Version, OAnimFormat::CurrentVersion);
            return {};
        }

        u32 clipCount = 0;
        ReadBytes(in, &clipCount, sizeof(u32));

        std::vector<OAnimFormat::ClipDirectoryEntry> directory(clipCount);
        ReadBytes(in, directory.data(), clipCount * sizeof(OAnimFormat::ClipDirectoryEntry));

        std::vector<Ref<AnimationClip>> clips;
        clips.reserve(clipCount);

        for (u32 i = 0; i < clipCount; ++i)
        {
            if (directory[i].Size == 0)
            {
                continue;
            }

            in.seekg(static_cast<std::streamoff>(directory[i].Offset));

            OAnimFormat::ClipHeader clipHeader;
            ReadBytes(in, &clipHeader, sizeof(clipHeader));

            auto clip = Ref<AnimationClip>::Create();
            clip->Name = ReadString(in);
            clip->Duration = clipHeader.Duration;

            // Read bone channels
            clip->BoneAnimations.resize(clipHeader.BoneChannelCount);
            for (u32 c = 0; c < clipHeader.BoneChannelCount; ++c)
            {
                OAnimFormat::BoneChannelHeader chanHeader;
                ReadBytes(in, &chanHeader, sizeof(chanHeader));

                auto& boneAnim = clip->BoneAnimations[c];
                boneAnim.BoneName = ReadString(in);

                // Position keys
                boneAnim.PositionKeys.resize(chanHeader.PositionKeyCount);
                for (u32 k = 0; k < chanHeader.PositionKeyCount; ++k)
                {
                    OAnimFormat::PositionKey pk;
                    ReadBytes(in, &pk, sizeof(pk));
                    boneAnim.PositionKeys[k].Time = pk.Time;
                    boneAnim.PositionKeys[k].Position = { pk.Position[0], pk.Position[1], pk.Position[2] };
                }

                // Rotation keys
                boneAnim.RotationKeys.resize(chanHeader.RotationKeyCount);
                for (u32 k = 0; k < chanHeader.RotationKeyCount; ++k)
                {
                    OAnimFormat::RotationKey rk;
                    ReadBytes(in, &rk, sizeof(rk));
                    boneAnim.RotationKeys[k].Time = rk.Time;
                    boneAnim.RotationKeys[k].Rotation = glm::quat(rk.Rotation[0], rk.Rotation[1], rk.Rotation[2], rk.Rotation[3]);
                }

                // Scale keys
                boneAnim.ScaleKeys.resize(chanHeader.ScaleKeyCount);
                for (u32 k = 0; k < chanHeader.ScaleKeyCount; ++k)
                {
                    OAnimFormat::ScaleKey sk;
                    ReadBytes(in, &sk, sizeof(sk));
                    boneAnim.ScaleKeys[k].Time = sk.Time;
                    boneAnim.ScaleKeys[k].Scale = { sk.Scale[0], sk.Scale[1], sk.Scale[2] };
                }
            }

            // Read morph keyframes
            clip->MorphKeyframes.resize(clipHeader.MorphKeyframeCount);
            for (u32 m = 0; m < clipHeader.MorphKeyframeCount; ++m)
            {
                OAnimFormat::MorphKeyframe mkData;
                ReadBytes(in, &mkData, sizeof(mkData));
                clip->MorphKeyframes[m].Time = mkData.Time;
                clip->MorphKeyframes[m].Weight = mkData.Weight;
                clip->MorphKeyframes[m].TargetName = ReadString(in);
            }

            clips.push_back(clip);
        }

        OLO_CORE_TRACE("AnimationBinarySerializer::Read: Loaded '{}' ({} clips)",
            path.filename().string(), clips.size());

        return clips;
    }

    // ========================================================================
    // AnimationBinarySerializer::ReadTimestamp
    // ========================================================================

    bool AnimationBinarySerializer::ReadTimestamp(const std::filesystem::path& path, u64& outSourceTimestamp)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
        {
            return false;
        }

        OAnimFormat::FileHeader header;
        ReadBytes(in, &header, sizeof(header));

        if (header.Magic != OAnimFormat::MagicNumber || header.Version != OAnimFormat::CurrentVersion)
        {
            return false;
        }

        outSourceTimestamp = header.SourceTimestamp;
        return true;
    }

} // namespace OloEngine
