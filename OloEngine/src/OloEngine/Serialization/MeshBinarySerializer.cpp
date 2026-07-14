#include "OloEnginePCH.h"
#include "MeshBinarySerializer.h"
#include "MeshBinaryFormat.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Serialization/ImportedMaterialCodec.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/MorphTargets/MorphTarget.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine
{
    // ========================================================================
    // Internal helpers
    // ========================================================================

    namespace
    {
        // ── Validation helpers ──

        // Checks every component in the matrix array for finite values.
        // On failure, logs the first offending element and replaces the matrix
        // with identity so downstream code doesn't propagate NaN/Inf.
        void ValidateMat4Array(std::vector<glm::mat4>& arr, const char* arrayName,
                               const std::filesystem::path& path)
        {
            for (sizet i = 0; i < arr.size(); ++i)
            {
                auto& m = arr[i];
                for (int c = 0; c < 4; ++c)
                {
                    for (int r = 0; r < 4; ++r)
                    {
                        if (!std::isfinite(m[c][r]))
                        {
                            OLO_CORE_WARN("MeshBinarySerializer::Read: Non-finite value in {} "
                                          "at bone {} element [{},{}] in '{}', replacing with identity",
                                          arrayName, i, c, r, path.string());
                            m = glm::mat4(1.0f);
                            goto next_matrix; // skip remaining elements of this matrix
                        }
                    }
                }
            next_matrix:;
            }
        }

        // Returns true if the stream position hasn't exceeded the section boundary.
        bool VerifySectionBoundary(std::istream& payload, u64 seekBase, u64 sectionEnd,
                                   const char* sectionName, const std::filesystem::path& path)
        {
            if (auto const pos = static_cast<u64>(payload.tellg()) - seekBase; pos > sectionEnd)
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Read: {} section read past boundary "
                               "(pos={}, sectionEnd={}) in '{}'",
                               sectionName, pos, sectionEnd, path.string());
                return false;
            }
            return true;
        }

        // Stream I/O helpers — accept std::ostream / std::istream so they work
        // with both file streams and in-memory stringstreams.
        bool WriteBytes(std::ostream& out, const void* data, sizet size)
        {
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            return !out.fail();
        }

        bool ReadBytes(std::istream& in, void* data, sizet size)
        {
            in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
            if (in.gcount() != static_cast<std::streamsize>(size))
            {
                // Zero-fill the unread tail so callers never see uninitialised memory
                auto bytesRead = static_cast<sizet>(in.gcount());
                std::memset(static_cast<u8*>(data) + bytesRead, 0, size - bytesRead);
                OLO_CORE_ERROR("ReadBytes: short read ({} of {} bytes)", in.gcount(), size);
                in.setstate(std::ios::failbit);
                return false;
            }
            return true;
        }

        bool WriteString(std::ostream& out, const std::string& str)
        {
            constexpr u32 MAX_STRING_LENGTH = 65536;
            auto len = static_cast<u32>(str.size());
            if (len > MAX_STRING_LENGTH)
            {
                OLO_CORE_ERROR("WriteString: string length {} exceeds max {}", len, MAX_STRING_LENGTH);
                return false;
            }
            WriteBytes(out, &len, sizeof(u32));
            if (len > 0)
            {
                WriteBytes(out, str.data(), len);
            }
            return true;
        }

        std::string ReadString(std::istream& in)
        {
            u32 len = 0;
            ReadBytes(in, &len, sizeof(u32));
            if (len == 0)
            {
                return {};
            }
            // Guard against corrupt length values — mesh string fields (bone/node
            // names, paths) should never exceed 64 KiB.
            if (constexpr u32 MAX_STRING_LENGTH = 65536; len > MAX_STRING_LENGTH)
            {
                OLO_CORE_ERROR("ReadString: suspicious string length {} (max {}), stream likely corrupt", len, MAX_STRING_LENGTH);
                in.setstate(std::ios::failbit);
                return {};
            }
            std::string result(len, '\0');
            ReadBytes(in, result.data(), len);
            return result;
        }

        u64 StreamPos(std::ostream& out)
        {
            return static_cast<u64>(out.tellp());
        }

        u64 StreamPos(std::istream& in)
        {
            return static_cast<u64>(in.tellg());
        }

        // ── zlib compression ────────────────────────────────────────

        // Compress a byte buffer using zlib deflate (level 6 = good balance).
        // Returns empty vector on failure.
        std::vector<u8> ZlibCompress(const void* data, sizet size)
        {
            if (!data || size == 0)
            {
                return {};
            }

            auto bound = ::compressBound(static_cast<uLong>(size));
            std::vector<u8> compressed(bound);
            uLongf compressedSize = bound;

            if (auto ret = ::compress2(compressed.data(), &compressedSize,
                                       static_cast<const Bytef*>(data),
                                       static_cast<uLong>(size), 6);
                ret != Z_OK)
            {
                OLO_CORE_ERROR("ZlibCompress: compress2 failed (error {})", ret);
                return {};
            }

            compressed.resize(compressedSize);
            return compressed;
        }

        // Decompress a zlib-compressed buffer. Caller must provide the
        // exact uncompressed size (stored in the file header).
        std::vector<u8> ZlibDecompress(const void* compressedData, sizet compressedSize, sizet uncompressedSize)
        {
            if (!compressedData || compressedSize == 0 || uncompressedSize == 0)
            {
                return {};
            }

            // Guard against corrupt headers claiming unreasonable sizes
            if (constexpr sizet MAX_UNCOMPRESSED_SIZE = 512u * 1024u * 1024u; uncompressedSize > MAX_UNCOMPRESSED_SIZE) // 512 MiB
            {
                OLO_CORE_ERROR("ZlibDecompress: claimed uncompressed size {} exceeds limit {}", uncompressedSize, MAX_UNCOMPRESSED_SIZE);
                return {};
            }

            std::vector<u8> decompressed(uncompressedSize);
            auto destLen = static_cast<uLongf>(uncompressedSize);

            if (auto ret = ::uncompress(decompressed.data(), &destLen,
                                        static_cast<const Bytef*>(compressedData),
                                        static_cast<uLong>(compressedSize));
                ret != Z_OK)
            {
                OLO_CORE_ERROR("ZlibDecompress: uncompress failed (error {})", ret);
                return {};
            }

            decompressed.resize(destLen);
            return decompressed;
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
            std::error_code ec;
            std::filesystem::create_directories(parentDir, ec);
            if (ec)
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Write: Failed to create directory '{}': {}",
                               parentDir.string(), ec.message());
                return false;
            }
        }

        const auto& vertices = meshSource.GetVertices();
        const auto& indices = meshSource.GetIndices();
        const auto& submeshes = meshSource.GetSubmeshes();
        const auto& materials = meshSource.GetMaterials();

        // ── Validate counts against format caps before writing ──
        if (auto const vc = static_cast<u32>(vertices.Num()); vc > OMeshFormat::MaxVertexCount)
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Write: VertexCount ({}) exceeds cap ({}) for '{}'",
                           vc, OMeshFormat::MaxVertexCount, path.string());
            return false;
        }
        if (auto const ic = static_cast<u32>(indices.Num()); ic > OMeshFormat::MaxIndexCount)
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Write: IndexCount ({}) exceeds cap ({}) for '{}'",
                           ic, OMeshFormat::MaxIndexCount, path.string());
            return false;
        }
        if (auto const sc = static_cast<u32>(submeshes.Num()); sc > OMeshFormat::MaxSubmeshCount)
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Write: SubmeshCount ({}) exceeds cap ({}) for '{}'",
                           sc, OMeshFormat::MaxSubmeshCount, path.string());
            return false;
        }
        if (auto const mc = static_cast<u32>(materials.Num()); mc > OMeshFormat::MaxMaterialCount)
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Write: MaterialCount ({}) exceeds cap ({}) for '{}'",
                           mc, OMeshFormat::MaxMaterialCount, path.string());
            return false;
        }

        // ── Write payload (SectionDirectory + all sections) to memory ──
        std::ostringstream payload(std::ios::binary);

        OMeshFormat::SectionDirectory directory;

        // Reserve space for directory (will be patched)
        WriteBytes(payload, &directory, sizeof(directory));

        // ── Geometry Section ──
        {
            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Geometry))].Offset = StreamPos(payload);

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

            WriteBytes(payload, &geo, sizeof(geo));
            if (!encodedVB.Data.empty())
            {
                WriteBytes(payload, encodedVB.Data.data(), encodedVB.Data.size());
            }
            if (!encodedIB.Data.empty())
            {
                WriteBytes(payload, encodedIB.Data.data(), encodedIB.Data.size());
            }
            if (!encodedShadow.Data.empty())
            {
                WriteBytes(payload, encodedShadow.Data.data(), encodedShadow.Data.size());
            }

            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Geometry))].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Geometry))].Offset;
        }

        // ── Submesh Section ──
        {
            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Submeshes))].Offset = StreamPos(payload);

            OMeshFormat::SubmeshHeader subHeader;
            subHeader.SubmeshCount = static_cast<u32>(submeshes.Num());
            WriteBytes(payload, &subHeader, sizeof(subHeader));

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

                WriteBytes(payload, &entry, sizeof(entry));
                if (!WriteString(payload, sub.m_NodeName) || !WriteString(payload, sub.m_MeshName))
                {
                    return false;
                }
            }

            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Submeshes))].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Submeshes))].Offset;
        }

        // ── Material Section ──
        if (!materials.IsEmpty())
        {
            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Materials))].Offset = StreamPos(payload);

            OMeshFormat::MaterialHeader matHeader;
            matHeader.MaterialCount = static_cast<u32>(materials.Num());
            WriteBytes(payload, &matHeader, sizeof(matHeader));

            for (const auto& [index, handle] : materials)
            {
                OMeshFormat::MaterialEntry entry;
                entry.Index = index;
                entry.Handle = static_cast<u64>(handle);
                WriteBytes(payload, &entry, sizeof(entry));
            }

            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Materials))].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Materials))].Offset;
        }

        // ── Skeleton Section ──
        if (meshSource.HasSkeleton())
        {
            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Skeleton))].Offset = StreamPos(payload);

            const auto* skeleton = meshSource.GetSkeleton();
            auto boneCount = static_cast<u32>(skeleton->m_BoneNames.size());

            if (boneCount > OMeshFormat::MaxBoneCount)
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Write: BoneCount ({}) exceeds cap ({}) for '{}'",
                               boneCount, OMeshFormat::MaxBoneCount, path.string());
                return false;
            }

            // Validate skeleton array sizes — reject mismatches
            auto const validateSize = [&boneCount, &path](sizet actual, const char* name) -> bool
            {
                if (static_cast<u32>(actual) != boneCount)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Write: Skeleton {} size ({}) != boneCount ({})",
                                   name, actual, boneCount);
                    return false;
                }
                return true;
            };

            if (!validateSize(skeleton->m_ParentIndices.size(), "ParentIndices") ||
                !validateSize(skeleton->m_LocalTransforms.size(), "LocalTransforms") ||
                !validateSize(skeleton->m_GlobalTransforms.size(), "GlobalTransforms") ||
                !validateSize(skeleton->m_BindPoseMatrices.size(), "BindPoseMatrices") ||
                !validateSize(skeleton->m_InverseBindPoses.size(), "InverseBindPoses") ||
                !validateSize(skeleton->m_BindPoseLocalTransforms.size(), "BindPoseLocalTransforms") ||
                !validateSize(skeleton->m_BonePreTransforms.size(), "BonePreTransforms"))
            {
                return false;
            }

            OMeshFormat::SkeletonHeader skelHeader;
            skelHeader.BoneCount = boneCount;
            WriteBytes(payload, &skelHeader, sizeof(skelHeader));

            // Parent indices
            WriteBytes(payload, skeleton->m_ParentIndices.data(), boneCount * sizeof(i32));

            // Transform arrays (6 arrays of mat4)
            auto const writeMat4Array = [&payload, &boneCount](const std::vector<glm::mat4>& arr)
            {
                WriteBytes(payload, arr.data(), boneCount * sizeof(f32) * 16);
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
                if (!WriteString(payload, skeleton->m_BoneNames[j]))
                {
                    return false;
                }
            }

            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Skeleton))].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Skeleton))].Offset;
        }

        // ── BoneInfluence Section ──
        if (meshSource.HasBoneInfluences())
        {
            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::BoneInfluences))].Offset = StreamPos(payload);

            const auto& boneInfluences = meshSource.GetBoneInfluences();
            auto influenceCount = static_cast<u32>(boneInfluences.Num());
            auto const vertCount = static_cast<u32>(vertices.Num());

            if (influenceCount != vertCount)
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Write: BoneInfluence count ({}) != vertex count ({}) for '{}'",
                               influenceCount, vertCount, path.string());
                return false;
            }

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

            WriteBytes(payload, &biHeader, sizeof(biHeader));
            if (!encoded.Data.empty())
            {
                WriteBytes(payload, encoded.Data.data(), encoded.Data.size());
            }

            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::BoneInfluences))].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::BoneInfluences))].Offset;
        }

        // ── BoneInfo Section ──
        {
            const auto& boneInfo = meshSource.GetBoneInfo();
            if (!boneInfo.IsEmpty())
            {
                directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::BoneInfo))].Offset = StreamPos(payload);

                OMeshFormat::BoneInfoHeader biHeader;
                biHeader.BoneInfoCount = static_cast<u32>(boneInfo.Num());
                WriteBytes(payload, &biHeader, sizeof(biHeader));

                for (i32 i = 0; i < boneInfo.Num(); ++i)
                {
                    OMeshFormat::BoneInfoEntry entry{};
                    std::memcpy(entry.InverseBindPose, &boneInfo[i].m_InverseBindPose[0][0], sizeof(f32) * 16);
                    entry.BoneIndex = boneInfo[i].m_BoneIndex;
                    WriteBytes(payload, &entry, sizeof(entry));
                }

                directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::BoneInfo))].Size =
                    StreamPos(payload) - directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::BoneInfo))].Offset;
            }
        }

        // ── MorphTarget Section ──
        if (meshSource.HasMorphTargets())
        {
            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::MorphTargets))].Offset = StreamPos(payload);

            const auto& morphTargets = meshSource.GetMorphTargets();
            auto targetCount = morphTargets->GetTargetCount();
            auto vertCount = morphTargets->GetVertexCount();

            if (targetCount > OMeshFormat::MaxMorphTargetCount)
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Write: MorphTargetCount ({}) exceeds cap ({}) for '{}'",
                               targetCount, OMeshFormat::MaxMorphTargetCount, path.string());
                return false;
            }

            OMeshFormat::MorphTargetHeader mtHeader;
            mtHeader.TargetCount = targetCount;
            mtHeader.VertexCount = vertCount;
            WriteBytes(payload, &mtHeader, sizeof(mtHeader));

            for (u32 t = 0; t < targetCount; ++t)
            {
                const auto& target = morphTargets->Targets[t];

                OMeshFormat::MorphTargetEntry entry{};
                // If IsSparse but no sparse entries, fall through to dense to avoid reader ambiguity
                entry.SparseEntryCount = (target.IsSparse && !target.SparseVertices.empty())
                                             ? static_cast<u32>(target.SparseVertices.size())
                                             : 0;
                WriteBytes(payload, &entry, sizeof(entry));

                if (!WriteString(payload, target.Name))
                {
                    return false;
                }

                if (entry.SparseEntryCount > 0)
                {
                    for (const auto& sparse : target.SparseVertices)
                    {
                        WriteBytes(payload, &sparse.VertexIndex, sizeof(u32));
                        WriteBytes(payload, &sparse.Delta, sizeof(MorphTargetVertex));
                    }
                }
                else
                {
                    // Dense path: target vertex count must match exactly.
                    if (!target.Vertices.empty() && static_cast<u32>(target.Vertices.size()) != vertCount)
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Write: Dense morph target '{}' vertex count ({}) "
                                       "does not match expected vertCount ({})",
                                       target.Name, target.Vertices.size(), vertCount);
                        return false;
                    }
                    // Write dense data or all zeroes if empty.
                    if (!target.Vertices.empty())
                    {
                        WriteBytes(payload, target.Vertices.data(), vertCount * sizeof(MorphTargetVertex));
                    }
                    else
                    {
                        MorphTargetVertex zero{};
                        for (u32 p = 0; p < vertCount; ++p)
                        {
                            WriteBytes(payload, &zero, sizeof(MorphTargetVertex));
                        }
                    }
                }
            }

            directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::MorphTargets))].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::MorphTargets))].Offset;
        }

        // ── VirtualMesh Section (v2+, optional) — cooked OVGM DAG blob ──
        if (meshSource.HasVirtualMeshBlob())
        {
            const auto& blob = meshSource.GetVirtualMeshBlob();
            if (blob.size() > OMeshFormat::MaxVirtualMeshBlobSize)
            {
                OLO_CORE_WARN("MeshBinarySerializer::Write: VirtualMesh blob ({} bytes) exceeds cap ({}) — "
                              "skipping the section for '{}'",
                              blob.size(), OMeshFormat::MaxVirtualMeshBlobSize, path.string());
            }
            else
            {
                auto& entry = directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::VirtualMesh))];
                entry.Offset = StreamPos(payload);

                OMeshFormat::VirtualMeshHeader vmHeader;
                vmHeader.BlobSize = blob.size();
                WriteBytes(payload, &vmHeader, sizeof(vmHeader));
                WriteBytes(payload, blob.data(), blob.size());

                entry.Size = StreamPos(payload) - entry.Offset;
            }
        }

        // ── ImportedMaterials Section (v4+, optional) ──
        // The materials the mesh was imported with (issue #629). Without them a warm
        // cache load has to re-run a full Assimp import of the source file purely to
        // rebuild materials — and the asset-pack cook, which reads a MeshSource, had
        // no materials to ship at all, so a packed game rendered flat grey.
        if (!meshSource.GetImportedMaterials().empty())
        {
            std::vector<u8> const blob = ImportedMaterialCodec::EncodeMaterials(meshSource.GetImportedMaterials());
            if (blob.size() > ImportedMaterialCodec::MaxBlobSize)
            {
                OLO_CORE_WARN("MeshBinarySerializer::Write: imported-material blob ({} bytes) exceeds cap ({}) — "
                              "skipping the section for '{}'",
                              blob.size(), ImportedMaterialCodec::MaxBlobSize, path.string());
            }
            else if (!blob.empty())
            {
                auto& entry = directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::ImportedMaterials))];
                entry.Offset = StreamPos(payload);

                OMeshFormat::ImportedMaterialsHeader imHeader;
                imHeader.BlobSize = blob.size();
                WriteBytes(payload, &imHeader, sizeof(imHeader));
                WriteBytes(payload, blob.data(), blob.size());

                entry.Size = StreamPos(payload) - entry.Offset;
            }
        }

        // ── Patch section directory at start of payload ──
        payload.seekp(0);
        WriteBytes(payload, &directory, sizeof(directory));

        // ── Compress payload and write to file ──
        auto payloadStr = payload.str();
        auto const uncompressedSize = payloadStr.size();

        auto compressed = ZlibCompress(payloadStr.data(), uncompressedSize);
        if (compressed.empty())
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Write: Failed to compress payload for '{}'", path.string());
            return false;
        }

        OMeshFormat::FileHeader header;
        header.Flags = OMeshFormat::FlagCompressed;
        if (meshSource.IsPreOptimized())
        {
            header.Flags |= OMeshFormat::FlagPreOptimized;
        }
        header.SourceTimestamp = sourceTimestamp;
        header.UncompressedPayloadSize = uncompressedSize;
        header.Checksum = Hash::CRC32(compressed.data(), compressed.size());
        header.TotalFileSize = sizeof(OMeshFormat::FileHeader) + compressed.size();

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Write: Failed to open '{}' for writing", path.string());
            return false;
        }

        WriteBytes(out, &header, sizeof(header));
        WriteBytes(out, compressed.data(), compressed.size());
        out.close();

        if (out.fail())
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Write: I/O error while writing '{}'", path.string());
            return false;
        }

        OLO_CORE_TRACE("MeshBinarySerializer::Write: Wrote '{}' ({} bytes, compressed from {}, {} verts, {} indices)",
                       path.filename().string(), header.TotalFileSize, uncompressedSize,
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
        if (!ReadBytes(in, &header, sizeof(header)))
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to read header from '{}'", path.string());
            return nullptr;
        }

        if (header.Magic != OMeshFormat::MagicNumber)
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Read: Invalid magic number in '{}'", path.string());
            return nullptr;
        }
        // Range check, not equality (docs/agent-rules/binary-format-versioning.md):
        // a v1 file simply lacks the appended sections; anything newer than this
        // build (or older than the floor) is rejected.
        if (header.Version < OMeshFormat::MinSupportedVersion || header.Version > OMeshFormat::CurrentVersion)
        {
            OLO_CORE_WARN("MeshBinarySerializer::Read: Unsupported version in '{}' (got {}, supported {}..{})",
                          path.string(), header.Version, OMeshFormat::MinSupportedVersion,
                          OMeshFormat::CurrentVersion);
            return nullptr;
        }

        // ── Obtain the payload stream (decompress if needed) ──
        // We use a unique_ptr to keep the istringstream alive for the compressed path,
        // while using the file stream directly for uncompressed (legacy) files.
        std::unique_ptr<std::istringstream> decompressedStream;
        std::istream* payloadStream = &in;
        u64 actualPayloadSize = 0;

        // For the legacy uncompressed path, section offsets in the directory
        // are relative to the start of the payload (right after the file
        // header). Record the current stream position so seekg() calls can
        // adjust by this base offset.
        auto const payloadBase = static_cast<u64>(in.tellg());

        if (header.Flags & OMeshFormat::FlagCompressed)
        {
            if (header.TotalFileSize <= sizeof(OMeshFormat::FileHeader))
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Read: TotalFileSize ({}) too small for compressed payload in '{}'",
                               header.TotalFileSize, path.string());
                return nullptr;
            }

            // Guard against absurd allocation sizes from corrupt files (256 MiB limit)
            constexpr u64 MAX_COMPRESSED_SIZE = 256u * 1024u * 1024u;
            auto const compressedSize = header.TotalFileSize - sizeof(OMeshFormat::FileHeader);
            if (compressedSize > MAX_COMPRESSED_SIZE)
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Read: compressed payload size {} exceeds limit in '{}'",
                               compressedSize, path.string());
                return nullptr;
            }

            std::vector<u8> compressedData(compressedSize);
            if (!ReadBytes(in, compressedData.data(), compressedSize))
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to read compressed payload from '{}'", path.string());
                return nullptr;
            }

            // Validate CRC32 checksum (always computed by Write, never legitimately zero)
            if (auto computed = Hash::CRC32(compressedData.data(), compressedData.size());
                computed != header.Checksum)
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Read: Checksum mismatch in '{}' (expected 0x{:08X}, got 0x{:08X})",
                               path.string(), header.Checksum, computed);
                return nullptr;
            }

            auto decompressed = ZlibDecompress(compressedData.data(), compressedData.size(),
                                               header.UncompressedPayloadSize);
            if (decompressed.empty())
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to decompress payload in '{}'", path.string());
                return nullptr;
            }

            decompressedStream = std::make_unique<std::istringstream>(
                std::string(reinterpret_cast<const char*>(decompressed.data()), decompressed.size()),
                std::ios::binary);
            payloadStream = decompressedStream.get();
            actualPayloadSize = decompressed.size();
        }

        auto& payload = *payloadStream;

        // For the uncompressed (legacy) path the file stream is positioned
        // right after the header, so absolute seek positions need the base
        // offset added.  For the compressed path the decompressed stream
        // starts at 0 — base is 0.
        u64 const seekBase = decompressedStream ? 0 : payloadBase;

        // For the legacy (uncompressed) path, compute payload size from the file header
        if (actualPayloadSize == 0)
        {
            actualPayloadSize = header.TotalFileSize > sizeof(OMeshFormat::FileHeader)
                                    ? header.TotalFileSize - sizeof(OMeshFormat::FileHeader)
                                    : 0;
        }

        // Read the section directory, sized by the FILE's version — a v1 file
        // carries 7 entries; the appended entries stay zeroed (Size == 0), so
        // every later "is the section present" check naturally skips them.
        u16 const fileSectionCount = OMeshFormat::SectionCountForVersion(header.Version);
        OMeshFormat::SectionDirectory directory;
        if (!ReadBytes(payload, directory.Sections.data(),
                       static_cast<u64>(fileSectionCount) * sizeof(OMeshFormat::SectionEntry)))
        {
            OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to read section directory from '{}'", path.string());
            return nullptr;
        }

        // Validate all section entries against payload bounds
        u64 const directoryEnd = static_cast<u64>(fileSectionCount) * sizeof(OMeshFormat::SectionEntry);
        struct ValidRange
        {
            u64 Start;
            u64 End;
        };
        std::array<ValidRange, OMeshFormat::kSectionCount> validatedRanges{};
        u16 rangeCount = 0;

        for (u16 s = 0; s < OMeshFormat::kSectionCount; ++s)
        {
            const auto& sec = directory.Sections[s];
            if (sec.Size == 0)
            {
                continue;
            }
            if (sec.Offset < directoryEnd)
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Read: Section {} overlaps directory table "
                               "(Offset={}, directoryEnd={}) in '{}'",
                               s, sec.Offset, directoryEnd, path.string());
                return nullptr;
            }
            if (sec.Offset > actualPayloadSize || sec.Size > actualPayloadSize - sec.Offset)
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Read: Section {} out of bounds "
                               "(Offset={}, Size={}, PayloadLen={}) in '{}'",
                               s, sec.Offset, sec.Size, actualPayloadSize, path.string());
                return nullptr;
            }
            // Check for overlap with previously validated sections
            u64 const newStart = sec.Offset;
            u64 const newEnd = sec.Offset + sec.Size;
            for (u16 r = 0; r < rangeCount; ++r)
            {
                if (newStart < validatedRanges[r].End && newEnd > validatedRanges[r].Start)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: Section {} [{}, {}) overlaps a previous section in '{}'",
                                   s, newStart, newEnd, path.string());
                    return nullptr;
                }
            }
            validatedRanges[rangeCount++] = { newStart, newEnd };
        }

        TArray<Vertex> vertices;
        TArray<u32> indices;
        TArray<u32> shadowIndices;

        // ── Geometry Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Geometry))];
            if (sec.Size > 0)
            {
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));
                auto const sectionEnd = sec.Offset + sec.Size;

                OMeshFormat::GeometryHeader geo;
                ReadBytes(payload, &geo, sizeof(geo));

                // Validate counts against safety caps
                if (geo.VertexCount > OMeshFormat::MaxVertexCount || geo.IndexCount > OMeshFormat::MaxIndexCount || geo.ShadowIndexCount > OMeshFormat::MaxIndexCount)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: Geometry counts exceed safety limits in '{}'", path.string());
                    return nullptr;
                }
                if (geo.EncodedVertexSize > OMeshFormat::MaxEncodedSize || geo.EncodedIndexSize > OMeshFormat::MaxEncodedSize || geo.EncodedShadowIndexSize > OMeshFormat::MaxEncodedSize)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: Encoded sizes exceed safety limits in '{}'", path.string());
                    return nullptr;
                }

                // Verify all encoded data fits within the declared section
                {
                    auto const totalNeeded = sizeof(OMeshFormat::GeometryHeader) + geo.EncodedVertexSize + geo.EncodedIndexSize + geo.EncodedShadowIndexSize;
                    if (totalNeeded > sec.Size)
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Read: Geometry data ({} bytes) exceeds section size ({}) in '{}'",
                                       totalNeeded, sec.Size, path.string());
                        return nullptr;
                    }
                }

                // Reject mismatched count/encoded-size pairs
                if ((geo.VertexCount > 0) != (geo.EncodedVertexSize > 0) ||
                    (geo.IndexCount > 0) != (geo.EncodedIndexSize > 0) ||
                    (geo.ShadowIndexCount > 0) != (geo.EncodedShadowIndexSize > 0))
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: Count/encoded-size mismatch in geometry section of '{}'", path.string());
                    return nullptr;
                }

                // Validate vertex stride matches current Vertex struct.
                if (geo.VertexStride != sizeof(Vertex))
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: Vertex stride mismatch (file {}, expected {}) in '{}'",
                                   geo.VertexStride, sizeof(Vertex), path.string());
                    return nullptr;
                }

                // Decode vertices
                if (geo.VertexCount > 0 && geo.EncodedVertexSize > 0)
                {
                    EncodedMeshBuffer encoded;
                    encoded.Data.resize(static_cast<sizet>(geo.EncodedVertexSize));
                    encoded.OriginalSize = geo.VertexCount * geo.VertexStride;
                    ReadBytes(payload, encoded.Data.data(), encoded.Data.size());

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
                    ReadBytes(payload, encoded.Data.data(), encoded.Data.size());

                    indices.SetNum(static_cast<i32>(geo.IndexCount));
                    if (!MeshOptimization::DecodeIndexBuffer(indices.GetData(), geo.IndexCount, encoded))
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to decode index buffer");
                        return nullptr;
                    }

                    // Validate decoded index range against vertex count
                    for (u32 idx = 0; idx < geo.IndexCount; ++idx)
                    {
                        if (indices[static_cast<i32>(idx)] >= geo.VertexCount)
                        {
                            OLO_CORE_ERROR("MeshBinarySerializer::Read: Index {} out of range (value={}, vertexCount={}) in '{}'",
                                           idx, indices[static_cast<i32>(idx)], geo.VertexCount, path.string());
                            return nullptr;
                        }
                    }
                }

                // Decode shadow indices
                if (geo.ShadowIndexCount > 0 && geo.EncodedShadowIndexSize > 0)
                {
                    EncodedMeshBuffer encoded;
                    encoded.Data.resize(static_cast<sizet>(geo.EncodedShadowIndexSize));
                    encoded.OriginalSize = geo.ShadowIndexCount * sizeof(u32);
                    ReadBytes(payload, encoded.Data.data(), encoded.Data.size());

                    shadowIndices.SetNum(static_cast<i32>(geo.ShadowIndexCount));
                    if (!MeshOptimization::DecodeIndexBuffer(shadowIndices.GetData(), geo.ShadowIndexCount, encoded))
                    {
                        OLO_CORE_WARN("MeshBinarySerializer::Read: Failed to decode shadow indices (non-fatal)");
                        shadowIndices.Empty();
                    }
                    else
                    {
                        // Validate shadow index range against vertex count
                        for (u32 si = 0; si < geo.ShadowIndexCount; ++si)
                        {
                            if (shadowIndices[static_cast<i32>(si)] >= geo.VertexCount)
                            {
                                OLO_CORE_WARN("MeshBinarySerializer::Read: Shadow index {} out of range (value={}, vertexCount={}) — discarding shadow indices",
                                              si, shadowIndices[static_cast<i32>(si)], geo.VertexCount);
                                shadowIndices.Empty();
                                break;
                            }
                        }
                    }
                }

                // Verify stream didn't read past the declared section boundary
                if (!VerifySectionBoundary(payload, seekBase, sectionEnd, "Geometry", path))
                {
                    return nullptr;
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
            const auto& sec = directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Submeshes))];
            if (sec.Size > 0)
            {
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));

                OMeshFormat::SubmeshHeader subHeader;
                ReadBytes(payload, &subHeader, sizeof(subHeader));

                if (subHeader.SubmeshCount > OMeshFormat::MaxSubmeshCount)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: SubmeshCount ({}) exceeds safety limit in '{}'",
                                   subHeader.SubmeshCount, path.string());
                    return nullptr;
                }

                for (u32 i = 0; i < subHeader.SubmeshCount; ++i)
                {
                    OMeshFormat::SubmeshEntry entry{};
                    ReadBytes(payload, &entry, sizeof(entry));

                    // Validate all float components for NaN/Inf
                    {
                        bool valid = true;
                        for (int f = 0; f < 16; ++f)
                        {
                            if (!std::isfinite(entry.Transform[f]) || !std::isfinite(entry.LocalTransform[f]))
                            {
                                valid = false;
                                break;
                            }
                        }
                        for (int a = 0; a < 3 && valid; ++a)
                        {
                            if (!std::isfinite(entry.BoundsMin[a]) || !std::isfinite(entry.BoundsMax[a]) ||
                                entry.BoundsMin[a] > entry.BoundsMax[a])
                            {
                                valid = false;
                            }
                        }
                        if (!valid)
                        {
                            OLO_CORE_ERROR("MeshBinarySerializer::Read: Submesh {} has non-finite or inverted bounds in '{}'",
                                           i, path.string());
                            return nullptr;
                        }
                    }

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
                    sub.m_NodeName = ReadString(payload);
                    sub.m_MeshName = ReadString(payload);

                    // Validate submesh ranges against decoded buffer sizes
                    auto vertexCount = static_cast<u32>(meshSource->GetVertices().Num());
                    auto indexCount = static_cast<u32>(meshSource->GetIndices().Num());
                    if (sub.m_BaseVertex > vertexCount || sub.m_VertexCount > vertexCount - sub.m_BaseVertex || sub.m_BaseIndex > indexCount || sub.m_IndexCount > indexCount - sub.m_BaseIndex)
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Read: Submesh {} has out-of-range bounds in '{}'",
                                       i, path.string());
                        return nullptr;
                    }

                    meshSource->AddSubmesh(sub);
                    // AddSubmesh() invalidates the authored bounds via
                    // CalculateSubmeshBounds(), which expands skinned meshes
                    // using vertex-only positions — an approximation that
                    // discards the DCC-authored bounds we just read back.
                    // Restore them so round-trip preserves disk state.
                    auto& submeshes = meshSource->GetSubmeshes();
                    submeshes[submeshes.Num() - 1].m_BoundingBox = sub.m_BoundingBox;
                }

                if (!VerifySectionBoundary(payload, seekBase, sec.Offset + sec.Size, "Submesh", path))
                {
                    return nullptr;
                }
            }
        }

        // ── Material Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Materials))];
            if (sec.Size > 0)
            {
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));

                OMeshFormat::MaterialHeader matHeader;
                ReadBytes(payload, &matHeader, sizeof(matHeader));

                if (matHeader.MaterialCount > OMeshFormat::MaxMaterialCount)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: MaterialCount ({}) exceeds safety limit in '{}'",
                                   matHeader.MaterialCount, path.string());
                    return nullptr;
                }

                for (u32 i = 0; i < matHeader.MaterialCount; ++i)
                {
                    OMeshFormat::MaterialEntry entry;
                    ReadBytes(payload, &entry, sizeof(entry));
                    meshSource->SetMaterial(entry.Index, AssetHandle{ entry.Handle });
                }

                if (!VerifySectionBoundary(payload, seekBase, sec.Offset + sec.Size, "Material", path))
                {
                    return nullptr;
                }
            }
        }

        // ── Skeleton Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::Skeleton))];
            if (sec.Size > 0)
            {
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));

                OMeshFormat::SkeletonHeader skelHeader;
                ReadBytes(payload, &skelHeader, sizeof(skelHeader));
                u32 const boneCount = skelHeader.BoneCount;

                if (boneCount > OMeshFormat::MaxBoneCount)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: BoneCount ({}) exceeds safety limit in '{}'",
                                   boneCount, path.string());
                    return nullptr;
                }

                auto skeleton = Ref<Skeleton>::Create(static_cast<sizet>(boneCount));

                // Parent indices
                ReadBytes(payload, skeleton->m_ParentIndices.data(), boneCount * sizeof(i32));

                // Validate parent indices: each must be -1 (root) or a valid bone index
                for (u32 j = 0; j < boneCount; ++j)
                {
                    auto const parent = skeleton->m_ParentIndices[j];
                    if (parent < -1 || parent >= static_cast<i32>(boneCount))
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Read: Invalid parent index {} for bone {} in '{}'",
                                       parent, j, path.string());
                        return nullptr;
                    }
                }

                // Transform arrays — read then validate for NaN/Inf
                auto const readMat4Array = [&payload, &boneCount, &path](std::vector<glm::mat4>& arr, const char* name)
                {
                    arr.resize(boneCount);
                    ReadBytes(payload, arr.data(), boneCount * sizeof(f32) * 16);
                    ValidateMat4Array(arr, name, path);
                };

                readMat4Array(skeleton->m_LocalTransforms, "LocalTransforms");
                readMat4Array(skeleton->m_GlobalTransforms, "GlobalTransforms");
                readMat4Array(skeleton->m_BindPoseMatrices, "BindPoseMatrices");
                readMat4Array(skeleton->m_InverseBindPoses, "InverseBindPoses");
                readMat4Array(skeleton->m_BindPoseLocalTransforms, "BindPoseLocalTransforms");
                readMat4Array(skeleton->m_BonePreTransforms, "BonePreTransforms");

                // Bone names
                skeleton->m_BoneNames.resize(boneCount);
                for (u32 j = 0; j < boneCount; ++j)
                {
                    skeleton->m_BoneNames[j] = ReadString(payload);
                }

                // Compute bind-pose FinalBoneMatrices from cached data.
                // InverseBindPoses already have meshNodeGlobal correction baked in,
                // so Global * InvBind produces the correct bind-pose transform.
                skeleton->m_FinalBoneMatrices.resize(boneCount);
                for (u32 j = 0; j < boneCount; ++j)
                {
                    skeleton->m_FinalBoneMatrices[j] = skeleton->m_GlobalTransforms[j] * skeleton->m_InverseBindPoses[j];
                }

                meshSource->SetSkeleton(skeleton);

                if (!VerifySectionBoundary(payload, seekBase, sec.Offset + sec.Size, "Skeleton", path))
                {
                    return nullptr;
                }
            }
        }

        // ── BoneInfluence Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::BoneInfluences))];
            if (sec.Size > 0)
            {
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));

                OMeshFormat::BoneInfluenceHeader biHeader;
                ReadBytes(payload, &biHeader, sizeof(biHeader));

                if (biHeader.InfluenceCount > OMeshFormat::MaxVertexCount || biHeader.EncodedSize > OMeshFormat::MaxEncodedSize)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: BoneInfluence counts exceed safety limits in '{}'", path.string());
                    return nullptr;
                }

                // Validate bone influence stride matches current struct.
                if (biHeader.InfluenceStride != sizeof(BoneInfluence))
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: BoneInfluence stride mismatch (file {}, expected {}) in '{}'",
                                   biHeader.InfluenceStride, sizeof(BoneInfluence), path.string());
                    return nullptr;
                }

                // Bone influences should be 1:1 with vertices
                if (auto vCount = static_cast<u32>(meshSource->GetVertices().Num());
                    biHeader.InfluenceCount != vCount)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: BoneInfluence count ({}) != vertex count ({}) in '{}'",
                                   biHeader.InfluenceCount, vCount, path.string());
                    return nullptr;
                }

                if ((biHeader.InfluenceCount > 0) != (biHeader.EncodedSize > 0))
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: BoneInfluence count/encoded size mismatch "
                                   "(count={}, encodedSize={}) in '{}'",
                                   biHeader.InfluenceCount, biHeader.EncodedSize, path.string());
                    return nullptr;
                }

                if (biHeader.InfluenceCount > 0 && biHeader.EncodedSize > 0)
                {
                    EncodedMeshBuffer encoded;
                    encoded.Data.resize(static_cast<sizet>(biHeader.EncodedSize));
                    encoded.OriginalSize = biHeader.InfluenceCount * biHeader.InfluenceStride;
                    ReadBytes(payload, encoded.Data.data(), encoded.Data.size());

                    auto& boneInfluences = meshSource->GetBoneInfluences();
                    boneInfluences.SetNum(static_cast<i32>(biHeader.InfluenceCount));
                    if (!MeshOptimization::DecodeVertexBuffer(boneInfluences.GetData(),
                                                              biHeader.InfluenceCount, biHeader.InfluenceStride, encoded))
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to decode bone influences in '{}'", path.string());
                        return nullptr;
                    }
                }

                if (!VerifySectionBoundary(payload, seekBase, sec.Offset + sec.Size, "BoneInfluence", path))
                {
                    return nullptr;
                }
            }
        }

        // ── BoneInfo Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::BoneInfo))];
            if (sec.Size > 0)
            {
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));

                OMeshFormat::BoneInfoHeader biHeader;
                ReadBytes(payload, &biHeader, sizeof(biHeader));

                if (biHeader.BoneInfoCount > OMeshFormat::MaxBoneCount)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: BoneInfoCount ({}) exceeds safety limit in '{}'",
                                   biHeader.BoneInfoCount, path.string());
                    return nullptr;
                }

                if (biHeader.BoneInfoCount > 0 && !meshSource->HasSkeleton())
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: BoneInfo section has {} entries but no skeleton was loaded in '{}'",
                                   biHeader.BoneInfoCount, path.string());
                    return nullptr;
                }

                auto& boneInfo = meshSource->GetBoneInfo();
                auto const skeletonBoneCount = meshSource->HasSkeleton()
                                                   ? static_cast<u32>(meshSource->GetSkeleton()->m_BoneNames.size())
                                                   : u32(0);
                for (u32 i = 0; i < biHeader.BoneInfoCount; ++i)
                {
                    OMeshFormat::BoneInfoEntry entry{};
                    ReadBytes(payload, &entry, sizeof(entry));

                    // Validate BoneIndex against skeleton bone count
                    if (skeletonBoneCount > 0 && entry.BoneIndex >= skeletonBoneCount)
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Read: BoneInfo {} has BoneIndex {} >= boneCount {} in '{}'",
                                       i, entry.BoneIndex, skeletonBoneCount, path.string());
                        return nullptr;
                    }

                    BoneInfo info;
                    std::memcpy(&info.m_InverseBindPose[0][0], entry.InverseBindPose, sizeof(f32) * 16);

                    // Validate InverseBindPose for NaN/Inf
                    bool valid = true;
                    for (int c = 0; c < 4 && valid; ++c)
                    {
                        for (int r = 0; r < 4 && valid; ++r)
                        {
                            if (!std::isfinite(info.m_InverseBindPose[c][r]))
                            {
                                OLO_CORE_WARN("MeshBinarySerializer::Read: Non-finite InverseBindPose in BoneInfo {} "
                                              "element [{},{}] in '{}', replacing with identity",
                                              i, c, r, path.string());
                                info.m_InverseBindPose = glm::mat4(1.0f);
                                valid = false;
                            }
                        }
                    }

                    info.m_BoneIndex = entry.BoneIndex;
                    boneInfo.Add(info);
                }

                if (!VerifySectionBoundary(payload, seekBase, sec.Offset + sec.Size, "BoneInfo", path))
                {
                    return nullptr;
                }
            }
        }

        // ── MorphTarget Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::MorphTargets))];
            if (sec.Size > 0)
            {
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));

                OMeshFormat::MorphTargetHeader mtHeader;
                ReadBytes(payload, &mtHeader, sizeof(mtHeader));

                if (mtHeader.TargetCount > OMeshFormat::MaxMorphTargetCount || mtHeader.VertexCount > OMeshFormat::MaxVertexCount)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: MorphTarget counts exceed safety limits in '{}'", path.string());
                    return nullptr;
                }

                // Validate morph target vertex count against the decoded mesh
                if (auto meshVertCount = static_cast<u32>(meshSource->GetVertices().Num());
                    mtHeader.VertexCount != meshVertCount)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: MorphTarget VertexCount ({}) does not match mesh vertex count ({}) in '{}'",
                                   mtHeader.VertexCount, meshVertCount, path.string());
                    return nullptr;
                }

                auto morphTargetSet = Ref<MorphTargetSet>::Create();

                for (u32 t = 0; t < mtHeader.TargetCount; ++t)
                {
                    OMeshFormat::MorphTargetEntry entry{};
                    ReadBytes(payload, &entry, sizeof(entry));

                    MorphTarget target;
                    target.Name = ReadString(payload);

                    if (entry.SparseEntryCount > 0)
                    {
                        if (entry.SparseEntryCount > OMeshFormat::MaxVertexCount)
                        {
                            OLO_CORE_ERROR("MeshBinarySerializer::Read: SparseEntryCount ({}) exceeds safety limit in '{}'",
                                           entry.SparseEntryCount, path.string());
                            return nullptr;
                        }

                        target.IsSparse = true;
                        target.SparseVertices.resize(entry.SparseEntryCount);
                        for (u32 s = 0; s < entry.SparseEntryCount; ++s)
                        {
                            ReadBytes(payload, &target.SparseVertices[s].VertexIndex, sizeof(u32));
                            ReadBytes(payload, &target.SparseVertices[s].Delta, sizeof(MorphTargetVertex));

                            // Validate sparse vertex index against mesh vertex count
                            if (auto meshVertCount = static_cast<u32>(meshSource->GetVertices().Num());
                                target.SparseVertices[s].VertexIndex >= meshVertCount)
                            {
                                OLO_CORE_ERROR("MeshBinarySerializer::Read: Sparse morph vertex index {} out of range "
                                               "(value={}, meshVertexCount={}) in '{}'",
                                               s, target.SparseVertices[s].VertexIndex, meshVertCount, path.string());
                                return nullptr;
                            }
                            // Validate delta floats are finite
                            const auto& d = target.SparseVertices[s].Delta;
                            if (!std::isfinite(d.DeltaPosition.x) || !std::isfinite(d.DeltaPosition.y) || !std::isfinite(d.DeltaPosition.z) ||
                                !std::isfinite(d.DeltaNormal.x) || !std::isfinite(d.DeltaNormal.y) || !std::isfinite(d.DeltaNormal.z) ||
                                !std::isfinite(d.DeltaTangent.x) || !std::isfinite(d.DeltaTangent.y) || !std::isfinite(d.DeltaTangent.z))
                            {
                                OLO_CORE_ERROR("MeshBinarySerializer::Read: Non-finite morph delta at sparse entry {} in '{}'",
                                               s, path.string());
                                return nullptr;
                            }
                        }
                    }
                    else
                    {
                        target.IsSparse = false;
                        target.Vertices.resize(mtHeader.VertexCount);
                        if (mtHeader.VertexCount > 0)
                        {
                            ReadBytes(payload, target.Vertices.data(),
                                      mtHeader.VertexCount * sizeof(MorphTargetVertex));
                            // Validate dense delta floats are finite
                            for (u32 v = 0; v < mtHeader.VertexCount; ++v)
                            {
                                const auto& d = target.Vertices[v];
                                if (!std::isfinite(d.DeltaPosition.x) || !std::isfinite(d.DeltaPosition.y) || !std::isfinite(d.DeltaPosition.z) ||
                                    !std::isfinite(d.DeltaNormal.x) || !std::isfinite(d.DeltaNormal.y) || !std::isfinite(d.DeltaNormal.z) ||
                                    !std::isfinite(d.DeltaTangent.x) || !std::isfinite(d.DeltaTangent.y) || !std::isfinite(d.DeltaTangent.z))
                                {
                                    OLO_CORE_ERROR("MeshBinarySerializer::Read: Non-finite morph delta at vertex {} in '{}'",
                                                   v, path.string());
                                    return nullptr;
                                }
                            }
                        }
                    }

                    morphTargetSet->AddTarget(std::move(target));
                }

                meshSource->SetMorphTargets(morphTargetSet);

                if (!VerifySectionBoundary(payload, seekBase, sec.Offset + sec.Size, "MorphTarget", path))
                {
                    return nullptr;
                }
            }
        }

        // ── VirtualMesh Section (v2+, optional) — cooked OVGM DAG blob ──
        // Carried verbatim; the OVGM blob is self-validating (own magic, caps,
        // cross-reference checks) when VirtualMeshRegistry deserializes it, so
        // only the container framing is validated here.
        {
            const auto& sec = directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::VirtualMesh))];
            if (sec.Size > 0)
            {
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));

                OMeshFormat::VirtualMeshHeader vmHeader;
                ReadBytes(payload, &vmHeader, sizeof(vmHeader));

                if (vmHeader.BlobSize > OMeshFormat::MaxVirtualMeshBlobSize ||
                    vmHeader.BlobSize != sec.Size - sizeof(vmHeader))
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: VirtualMesh blob size {} inconsistent with "
                                   "section size {} in '{}'",
                                   vmHeader.BlobSize, sec.Size, path.string());
                    return nullptr;
                }

                std::vector<u8> blob(vmHeader.BlobSize);
                if (vmHeader.BlobSize > 0)
                {
                    if (!ReadBytes(payload, blob.data(), vmHeader.BlobSize))
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to read VirtualMesh blob from '{}'",
                                       path.string());
                        return nullptr;
                    }
                    meshSource->SetVirtualMeshBlob(std::move(blob));
                }

                if (!VerifySectionBoundary(payload, seekBase, sec.Offset + sec.Size, "VirtualMesh", path))
                {
                    return nullptr;
                }
            }
        }

        // ── ImportedMaterials Section (v4+, optional) ──
        // A v1-v3 file has no such directory entry (its Size stays 0), so this is
        // skipped without touching the stream — the version-gating discipline in
        // docs/agent-rules/binary-format-versioning.md. A malformed table costs the
        // materials, not the mesh: warn and continue with none, exactly as if the
        // section were absent.
        {
            const auto& sec = directory.Sections[static_cast<u16>(std::to_underlying(OMeshFormat::SectionType::ImportedMaterials))];
            if (sec.Size > 0)
            {
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));

                OMeshFormat::ImportedMaterialsHeader imHeader;
                ReadBytes(payload, &imHeader, sizeof(imHeader));

                if (imHeader.BlobSize > ImportedMaterialCodec::MaxBlobSize ||
                    imHeader.BlobSize != sec.Size - sizeof(imHeader))
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: imported-material blob size {} inconsistent with "
                                   "section size {} in '{}'",
                                   imHeader.BlobSize, sec.Size, path.string());
                    return nullptr;
                }

                std::vector<u8> blob(static_cast<sizet>(imHeader.BlobSize));
                if (imHeader.BlobSize > 0)
                {
                    if (!ReadBytes(payload, blob.data(), imHeader.BlobSize))
                    {
                        OLO_CORE_ERROR("MeshBinarySerializer::Read: Failed to read the imported-material blob from '{}'",
                                       path.string());
                        return nullptr;
                    }

                    std::vector<Ref<Material>> materials;
                    if (ImportedMaterialCodec::DecodeMaterials(blob, materials))
                    {
                        meshSource->SetImportedMaterials(std::move(materials));
                    }
                    else
                    {
                        OLO_CORE_WARN("MeshBinarySerializer::Read: imported-material table in '{}' is malformed; "
                                      "the mesh loads without materials",
                                      path.string());
                    }
                }

                if (!VerifySectionBoundary(payload, seekBase, sec.Offset + sec.Size, "ImportedMaterials", path))
                {
                    return nullptr;
                }
            }
        }

        OLO_CORE_TRACE("MeshBinarySerializer::Read: Loaded '{}' ({} verts, {} indices, {} submeshes)",
                       path.filename().string(),
                       meshSource->GetVertices().Num(),
                       meshSource->GetIndices().Num(),
                       meshSource->GetSubmeshes().Num());

        // Restore pre-optimized flag from the stored header flags
        meshSource->SetPreOptimized((header.Flags & OMeshFormat::FlagPreOptimized) != 0);

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
        if (!ReadBytes(in, &header, sizeof(header)))
        {
            return false;
        }

        // Deliberately STRICT here (unlike Read's range check): this gates the
        // disk-cache validity test, so an older-version cache reads as stale
        // and re-imports once — upgrading it to the current version (and
        // gaining the v2 VirtualMesh cook) — while Read() still accepts any
        // supported version for non-cache consumers.
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
            std::error_code ec;
            std::filesystem::create_directories(parentDir, ec);
            if (ec)
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Write: Failed to create directory '{}': {}",
                               parentDir.string(), ec.message());
                return false;
            }
        }

        // ── Write payload (clip directory + clip data) to memory ──
        std::ostringstream payload(std::ios::binary);

        auto clipCount = static_cast<u32>(clips.size());
        WriteBytes(payload, &clipCount, sizeof(u32));

        // Reserve space for clip directory (will be patched)
        auto directoryOffset = StreamPos(payload);
        std::vector<OAnimFormat::ClipDirectoryEntry> directory(clipCount);
        WriteBytes(payload, directory.data(), clipCount * sizeof(OAnimFormat::ClipDirectoryEntry));

        // Write each clip
        for (u32 i = 0; i < clipCount; ++i)
        {
            const auto& clip = clips[i];
            if (!clip)
            {
                continue;
            }

            directory[i].Offset = StreamPos(payload);

            OAnimFormat::ClipHeader clipHeader;
            clipHeader.Duration = clip->Duration;
            clipHeader.BoneChannelCount = static_cast<u32>(clip->BoneAnimations.size());
            clipHeader.MorphKeyframeCount = static_cast<u32>(clip->MorphKeyframes.size());
            WriteBytes(payload, &clipHeader, sizeof(clipHeader));

            if (!WriteString(payload, clip->Name))
            {
                return false;
            }

            // Write bone channels
            for (const auto& boneAnim : clip->BoneAnimations)
            {
                OAnimFormat::BoneChannelHeader chanHeader;
                chanHeader.PositionKeyCount = static_cast<u32>(boneAnim.PositionKeys.size());
                chanHeader.RotationKeyCount = static_cast<u32>(boneAnim.RotationKeys.size());
                chanHeader.ScaleKeyCount = static_cast<u32>(boneAnim.ScaleKeys.size());
                WriteBytes(payload, &chanHeader, sizeof(chanHeader));

                if (!WriteString(payload, boneAnim.BoneName))
                {
                    return false;
                }

                // Position keys
                for (const auto& key : boneAnim.PositionKeys)
                {
                    OAnimFormat::PositionKey pk{};
                    pk.Time = key.Time;
                    pk.Position[0] = key.Position.x;
                    pk.Position[1] = key.Position.y;
                    pk.Position[2] = key.Position.z;
                    WriteBytes(payload, &pk, sizeof(pk));
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
                    WriteBytes(payload, &rk, sizeof(rk));
                }

                // Scale keys
                for (const auto& key : boneAnim.ScaleKeys)
                {
                    OAnimFormat::ScaleKey sk{};
                    sk.Time = key.Time;
                    sk.Scale[0] = key.Scale.x;
                    sk.Scale[1] = key.Scale.y;
                    sk.Scale[2] = key.Scale.z;
                    WriteBytes(payload, &sk, sizeof(sk));
                }
            }

            // Write morph keyframes
            for (const auto& mk : clip->MorphKeyframes)
            {
                OAnimFormat::MorphKeyframe mkData{};
                mkData.Time = mk.Time;
                mkData.Weight = mk.Weight;
                WriteBytes(payload, &mkData, sizeof(mkData));
                if (!WriteString(payload, mk.TargetName))
                {
                    return false;
                }
            }

            directory[i].Size = StreamPos(payload) - directory[i].Offset;
        }

        // Patch clip directory
        payload.seekp(static_cast<std::streamoff>(directoryOffset));
        WriteBytes(payload, directory.data(), clipCount * sizeof(OAnimFormat::ClipDirectoryEntry));

        // ── Compress payload and write to file ──
        auto payloadStr = payload.str();
        auto const uncompressedSize = payloadStr.size();

        auto compressed = ZlibCompress(payloadStr.data(), uncompressedSize);
        if (compressed.empty())
        {
            OLO_CORE_ERROR("AnimationBinarySerializer::Write: Failed to compress payload for '{}'", path.string());
            return false;
        }

        OAnimFormat::FileHeader header;
        header.Flags = OAnimFormat::FlagCompressed;
        header.SourceTimestamp = sourceTimestamp;
        header.UncompressedPayloadSize = uncompressedSize;
        header.Checksum = Hash::CRC32(compressed.data(), compressed.size());
        header.TotalFileSize = sizeof(OAnimFormat::FileHeader) + compressed.size();

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            OLO_CORE_ERROR("AnimationBinarySerializer::Write: Failed to open '{}' for writing", path.string());
            return false;
        }

        WriteBytes(out, &header, sizeof(header));
        WriteBytes(out, compressed.data(), compressed.size());
        out.close();

        if (out.fail())
        {
            OLO_CORE_ERROR("AnimationBinarySerializer::Write: I/O error while writing '{}'", path.string());
            return false;
        }

        OLO_CORE_TRACE("AnimationBinarySerializer::Write: Wrote '{}' ({} clips, {} bytes, compressed from {})",
                       path.filename().string(), clipCount, header.TotalFileSize, uncompressedSize);

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
        if (!ReadBytes(in, &header, sizeof(header)))
        {
            OLO_CORE_ERROR("AnimationBinarySerializer::Read: Failed to read header from '{}'", path.string());
            return {};
        }

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

        // ── Obtain the payload stream (decompress if needed) ──
        std::unique_ptr<std::istringstream> decompressedStream;
        std::istream* payloadStream = &in;
        u64 actualPayloadSize = 0;

        if (header.Flags & OAnimFormat::FlagCompressed)
        {
            if (header.TotalFileSize < sizeof(OAnimFormat::FileHeader))
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: TotalFileSize ({}) is smaller than header in '{}'",
                               header.TotalFileSize, path.string());
                return {};
            }
            constexpr u64 MAX_COMPRESSED_SIZE = 256u * 1024u * 1024u; // 256 MiB
            auto const compressedSize = header.TotalFileSize - sizeof(OAnimFormat::FileHeader);
            if (compressedSize > MAX_COMPRESSED_SIZE)
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: compressed payload size {} exceeds limit in '{}'",
                               compressedSize, path.string());
                return {};
            }
            std::vector<u8> compressedData(compressedSize);
            if (!ReadBytes(in, compressedData.data(), compressedSize))
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: Failed to read compressed payload from '{}'", path.string());
                return {};
            }

            // Validate CRC32 checksum (always computed by Write, never legitimately zero)
            if (auto computed = Hash::CRC32(compressedData.data(), compressedData.size());
                computed != header.Checksum)
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: Checksum mismatch in '{}' (expected 0x{:08X}, got 0x{:08X})",
                               path.string(), header.Checksum, computed);
                return {};
            }

            auto decompressed = ZlibDecompress(compressedData.data(), compressedData.size(),
                                               header.UncompressedPayloadSize);
            if (decompressed.empty())
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: Failed to decompress payload in '{}'", path.string());
                return {};
            }

            decompressedStream = std::make_unique<std::istringstream>(
                std::string(reinterpret_cast<const char*>(decompressed.data()), decompressed.size()),
                std::ios::binary);
            payloadStream = decompressedStream.get();
            actualPayloadSize = decompressed.size();
        }

        auto& payload = *payloadStream;

        // For uncompressed payloads, payload data starts after the file header
        // in the original stream. For decompressed payloads, the stream starts
        // at offset 0. Track this base so directory seeks are correct.
        auto const payloadBase = decompressedStream ? sizet(0) : sizeof(OAnimFormat::FileHeader);

        // For the legacy (uncompressed) path, compute payload size from the file header
        if (actualPayloadSize == 0)
        {
            actualPayloadSize = header.TotalFileSize > sizeof(OAnimFormat::FileHeader)
                                    ? header.TotalFileSize - sizeof(OAnimFormat::FileHeader)
                                    : 0;
        }

        u32 clipCount = 0;
        if (!ReadBytes(payload, &clipCount, sizeof(u32)))
        {
            OLO_CORE_ERROR("AnimationBinarySerializer::Read: Failed to read clip count from '{}'", path.string());
            return {};
        }

        if (clipCount > OAnimFormat::MaxClipCount)
        {
            OLO_CORE_ERROR("AnimationBinarySerializer::Read: ClipCount ({}) exceeds safety limit in '{}'",
                           clipCount, path.string());
            return {};
        }

        std::vector<OAnimFormat::ClipDirectoryEntry> directory(clipCount);
        if (!ReadBytes(payload, directory.data(), clipCount * sizeof(OAnimFormat::ClipDirectoryEntry)))
        {
            OLO_CORE_ERROR("AnimationBinarySerializer::Read: Failed to read clip directory from '{}'", path.string());
            return {};
        }

        // Validate clip directory entries: bounds, post-directory start, non-overlap
        u64 const clipDirectoryEnd = sizeof(u32) + clipCount * sizeof(OAnimFormat::ClipDirectoryEntry);
        struct ClipRange
        {
            u64 Start;
            u64 End;
        };
        std::vector<ClipRange> validatedClipRanges;
        validatedClipRanges.reserve(clipCount);

        for (u32 i = 0; i < clipCount; ++i)
        {
            if (directory[i].Size == 0)
            {
                continue;
            }
            if (directory[i].Offset < clipDirectoryEnd)
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: Clip {} overlaps directory table "
                               "(Offset={}, directoryEnd={}) in '{}'",
                               i, directory[i].Offset, clipDirectoryEnd, path.string());
                return {};
            }
            if (directory[i].Offset > actualPayloadSize || directory[i].Size > actualPayloadSize - directory[i].Offset)
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: Clip {} directory entry out of bounds "
                               "(Offset={}, Size={}, PayloadLen={}) in '{}'",
                               i, directory[i].Offset, directory[i].Size, actualPayloadSize, path.string());
                return {};
            }
            u64 const cStart = directory[i].Offset;
            u64 const cEnd = directory[i].Offset + directory[i].Size;
            for (const auto& [Start, End] : validatedClipRanges)
            {
                if (cStart < End && cEnd > Start)
                {
                    OLO_CORE_ERROR("AnimationBinarySerializer::Read: Clip {} [{}, {}) overlaps a previous clip in '{}'",
                                   i, cStart, cEnd, path.string());
                    return {};
                }
            }
            validatedClipRanges.push_back({ cStart, cEnd });
        }

        std::vector<Ref<AnimationClip>> clips;
        clips.reserve(clipCount);

        for (u32 i = 0; i < clipCount; ++i)
        {
            if (directory[i].Size == 0)
            {
                continue;
            }

            // Bounds already validated above; seek directly.
            payload.seekg(static_cast<std::streamoff>(payloadBase + directory[i].Offset));

            auto const clipEnd = static_cast<std::streamoff>(payloadBase + directory[i].Offset + directory[i].Size);

            // Helper: verify that 'neededBytes' won't exceed the clip boundary.
            auto const ensureClipRemaining = [&payload, &clipEnd, &i, &path](sizet neededBytes, const char* context) -> bool
            {
                if (!payload.good() || payload.tellg() < 0)
                {
                    OLO_CORE_ERROR("AnimationBinarySerializer::Read: Stream in bad state "
                                   "at {} in clip {} of '{}'",
                                   context, i, path.string());
                    return false;
                }
                if (payload.tellg() + static_cast<std::streamoff>(neededBytes) > clipEnd)
                {
                    OLO_CORE_ERROR("AnimationBinarySerializer::Read: Clip {} would read {} bytes past boundary "
                                   "at {} in '{}'",
                                   i, neededBytes, context, path.string());
                    return false;
                }
                return true;
            };

            if (!ensureClipRemaining(sizeof(OAnimFormat::ClipHeader), "ClipHeader"))
            {
                return {};
            }
            OAnimFormat::ClipHeader clipHeader;
            ReadBytes(payload, &clipHeader, sizeof(clipHeader));

            if (clipHeader.BoneChannelCount > OAnimFormat::MaxBoneChannelCount)
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: BoneChannelCount ({}) exceeds safety limit in clip {} of '{}'",
                               clipHeader.BoneChannelCount, i, path.string());
                return {};
            }
            if (clipHeader.MorphKeyframeCount > OAnimFormat::MaxMorphKeyframeCount)
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: MorphKeyframeCount ({}) exceeds safety limit in clip {} of '{}'",
                               clipHeader.MorphKeyframeCount, i, path.string());
                return {};
            }

            auto clip = Ref<AnimationClip>::Create();
            if (!ensureClipRemaining(sizeof(u32), "clip Name string"))
            {
                return {};
            }
            clip->Name = ReadString(payload);
            clip->Duration = clipHeader.Duration;

            // Validate clip duration
            if (!std::isfinite(clip->Duration))
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: Non-finite clip Duration in clip {} of '{}'",
                               i, path.string());
                return {};
            }

            // Read bone channels
            clip->BoneAnimations.resize(clipHeader.BoneChannelCount);
            for (u32 c = 0; c < clipHeader.BoneChannelCount; ++c)
            {
                if (!ensureClipRemaining(sizeof(OAnimFormat::BoneChannelHeader), "BoneChannelHeader"))
                {
                    return {};
                }
                OAnimFormat::BoneChannelHeader chanHeader;
                ReadBytes(payload, &chanHeader, sizeof(chanHeader));

                if (chanHeader.PositionKeyCount > OAnimFormat::MaxKeyCount ||
                    chanHeader.RotationKeyCount > OAnimFormat::MaxKeyCount ||
                    chanHeader.ScaleKeyCount > OAnimFormat::MaxKeyCount)
                {
                    OLO_CORE_ERROR("AnimationBinarySerializer::Read: Key counts exceed safety limit in clip {} channel {} of '{}'",
                                   i, c, path.string());
                    return {};
                }

                auto& boneAnim = clip->BoneAnimations[c];
                if (!ensureClipRemaining(sizeof(u32), "BoneName string"))
                {
                    return {};
                }
                boneAnim.BoneName = ReadString(payload);

                // Position keys
                if (!ensureClipRemaining(chanHeader.PositionKeyCount * sizeof(OAnimFormat::PositionKey), "PositionKeys"))
                {
                    return {};
                }
                boneAnim.PositionKeys.resize(chanHeader.PositionKeyCount);
                for (u32 k = 0; k < chanHeader.PositionKeyCount; ++k)
                {
                    OAnimFormat::PositionKey pk;
                    ReadBytes(payload, &pk, sizeof(pk));
                    if (!std::isfinite(pk.Time) || !std::isfinite(pk.Position[0]) ||
                        !std::isfinite(pk.Position[1]) || !std::isfinite(pk.Position[2]))
                    {
                        OLO_CORE_ERROR("AnimationBinarySerializer::Read: Non-finite position key in clip {} channel {} key {} of '{}'",
                                       i, c, k, path.string());
                        return {};
                    }
                    boneAnim.PositionKeys[k].Time = pk.Time;
                    boneAnim.PositionKeys[k].Position = { pk.Position[0], pk.Position[1], pk.Position[2] };
                }

                // Rotation keys
                if (!ensureClipRemaining(chanHeader.RotationKeyCount * sizeof(OAnimFormat::RotationKey), "RotationKeys"))
                {
                    return {};
                }
                boneAnim.RotationKeys.resize(chanHeader.RotationKeyCount);
                for (u32 k = 0; k < chanHeader.RotationKeyCount; ++k)
                {
                    OAnimFormat::RotationKey rk;
                    ReadBytes(payload, &rk, sizeof(rk));
                    if (!std::isfinite(rk.Time) || !std::isfinite(rk.Rotation[0]) ||
                        !std::isfinite(rk.Rotation[1]) || !std::isfinite(rk.Rotation[2]) ||
                        !std::isfinite(rk.Rotation[3]))
                    {
                        OLO_CORE_ERROR("AnimationBinarySerializer::Read: Non-finite rotation key in clip {} channel {} key {} of '{}'",
                                       i, c, k, path.string());
                        return {};
                    }
                    boneAnim.RotationKeys[k].Time = rk.Time;
                    boneAnim.RotationKeys[k].Rotation = glm::quat(rk.Rotation[0], rk.Rotation[1], rk.Rotation[2], rk.Rotation[3]);
                }

                // Scale keys
                if (!ensureClipRemaining(chanHeader.ScaleKeyCount * sizeof(OAnimFormat::ScaleKey), "ScaleKeys"))
                {
                    return {};
                }
                boneAnim.ScaleKeys.resize(chanHeader.ScaleKeyCount);
                for (u32 k = 0; k < chanHeader.ScaleKeyCount; ++k)
                {
                    OAnimFormat::ScaleKey sk;
                    ReadBytes(payload, &sk, sizeof(sk));
                    if (!std::isfinite(sk.Time) || !std::isfinite(sk.Scale[0]) ||
                        !std::isfinite(sk.Scale[1]) || !std::isfinite(sk.Scale[2]))
                    {
                        OLO_CORE_ERROR("AnimationBinarySerializer::Read: Non-finite scale key in clip {} channel {} key {} of '{}'",
                                       i, c, k, path.string());
                        return {};
                    }
                    boneAnim.ScaleKeys[k].Time = sk.Time;
                    boneAnim.ScaleKeys[k].Scale = { sk.Scale[0], sk.Scale[1], sk.Scale[2] };
                }
            }

            // Read morph keyframes
            if (!ensureClipRemaining(clipHeader.MorphKeyframeCount * sizeof(OAnimFormat::MorphKeyframe), "MorphKeyframes"))
            {
                return {};
            }
            clip->MorphKeyframes.resize(clipHeader.MorphKeyframeCount);
            for (u32 m = 0; m < clipHeader.MorphKeyframeCount; ++m)
            {
                OAnimFormat::MorphKeyframe mkData;
                ReadBytes(payload, &mkData, sizeof(mkData));
                if (!std::isfinite(mkData.Time) || !std::isfinite(mkData.Weight))
                {
                    OLO_CORE_ERROR("AnimationBinarySerializer::Read: Non-finite morph keyframe in clip {} morph {} of '{}'",
                                   i, m, path.string());
                    return {};
                }
                clip->MorphKeyframes[m].Time = mkData.Time;
                clip->MorphKeyframes[m].Weight = mkData.Weight;
                if (!ensureClipRemaining(sizeof(u32), "MorphTarget TargetName string"))
                {
                    return {};
                }
                clip->MorphKeyframes[m].TargetName = ReadString(payload);
            }

            // Verify we haven't read past the clip's declared size boundary
            if (payload.tellg() > clipEnd)
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: Clip {} consumed {} bytes past its declared size ({}) in '{}'",
                               i, static_cast<i64>(payload.tellg() - clipEnd), directory[i].Size, path.string());
                return {};
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
        if (!ReadBytes(in, &header, sizeof(header)))
        {
            return false;
        }

        if (header.Magic != OAnimFormat::MagicNumber || header.Version != OAnimFormat::CurrentVersion)
        {
            return false;
        }

        outSourceTimestamp = header.SourceTimestamp;
        return true;
    }

} // namespace OloEngine
