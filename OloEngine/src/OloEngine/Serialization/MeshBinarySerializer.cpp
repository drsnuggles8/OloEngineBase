#include "OloEnginePCH.h"
#include "MeshBinarySerializer.h"
#include "MeshBinaryFormat.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/MorphTargets/MorphTarget.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <sstream>

namespace OloEngine
{
    // ========================================================================
    // Internal helpers
    // ========================================================================

    namespace
    {
        // Stream I/O helpers — accept std::ostream / std::istream so they work
        // with both file streams and in-memory stringstreams.
        void WriteBytes(std::ostream& out, const void* data, sizet size)
        {
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }

        void ReadBytes(std::istream& in, void* data, sizet size)
        {
            in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
            if (in.gcount() != static_cast<std::streamsize>(size))
            {
                // Zero-fill the unread tail so callers never see uninitialised memory
                auto bytesRead = static_cast<sizet>(in.gcount());
                std::memset(static_cast<u8*>(data) + bytesRead, 0, size - bytesRead);
                OLO_CORE_ERROR("ReadBytes: short read ({} of {} bytes)", in.gcount(), size);
                in.setstate(std::ios::failbit);
            }
        }

        void WriteString(std::ostream& out, const std::string& str)
        {
            auto len = static_cast<u32>(str.size());
            WriteBytes(out, &len, sizeof(u32));
            if (len > 0)
            {
                WriteBytes(out, str.data(), len);
            }
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
            constexpr u32 MAX_STRING_LENGTH = 65536;
            if (len > MAX_STRING_LENGTH)
            {
                OLO_CORE_ERROR("ReadString: suspicious string length {} (max {}), stream likely corrupt", len, MAX_STRING_LENGTH);
                in.setstate(std::ios::failbit);
                return {};
            }
            std::string result(len, '\0');
            ReadBytes(in, result.data(), len);
            return result;
        }

        void WriteMat4(std::ostream& out, const glm::mat4& m)
        {
            WriteBytes(out, &m[0][0], sizeof(f32) * 16);
        }

        glm::mat4 ReadMat4(std::istream& in)
        {
            glm::mat4 m(1.0f);
            ReadBytes(in, &m[0][0], sizeof(f32) * 16);
            return m;
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
            constexpr sizet MAX_UNCOMPRESSED_SIZE = 512u * 1024u * 1024u; // 512 MiB
            if (uncompressedSize > MAX_UNCOMPRESSED_SIZE)
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
            std::filesystem::create_directories(parentDir);
        }

        const auto& vertices = meshSource.GetVertices();
        const auto& indices = meshSource.GetIndices();
        const auto& submeshes = meshSource.GetSubmeshes();
        const auto& materials = meshSource.GetMaterials();

        // ── Write payload (SectionDirectory + all sections) to memory ──
        std::ostringstream payload(std::ios::binary);

        OMeshFormat::SectionDirectory directory;

        // Reserve space for directory (will be patched)
        WriteBytes(payload, &directory, sizeof(directory));

        // ── Geometry Section ──
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Geometry)].Offset = StreamPos(payload);

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

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Geometry)].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Geometry)].Offset;
        }

        // ── Submesh Section ──
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Submeshes)].Offset = StreamPos(payload);

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
                WriteString(payload, sub.m_NodeName);
                WriteString(payload, sub.m_MeshName);
            }

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Submeshes)].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Submeshes)].Offset;
        }

        // ── Material Section ──
        if (!materials.IsEmpty())
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Materials)].Offset = StreamPos(payload);

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

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Materials)].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Materials)].Offset;
        }

        // ── Skeleton Section ──
        if (meshSource.HasSkeleton())
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Skeleton)].Offset = StreamPos(payload);

            const auto* skeleton = meshSource.GetSkeleton();
            auto boneCount = static_cast<u32>(skeleton->m_BoneNames.size());

            if (skeleton->m_ParentIndices.size() < boneCount)
            {
                OLO_CORE_ERROR("MeshBinarySerializer::Write: ParentIndices size ({}) < BoneNames size ({})",
                               skeleton->m_ParentIndices.size(), boneCount);
                return false;
            }

            OMeshFormat::SkeletonHeader skelHeader;
            skelHeader.BoneCount = boneCount;
            WriteBytes(payload, &skelHeader, sizeof(skelHeader));

            // Parent indices
            WriteBytes(payload, skeleton->m_ParentIndices.data(), boneCount * sizeof(i32));

            // Transform arrays (7 arrays of mat4)
            auto const writeMat4Array = [&](const std::vector<glm::mat4>& arr)
            {
                if (arr.size() >= boneCount)
                {
                    WriteBytes(payload, arr.data(), boneCount * sizeof(glm::mat4));
                }
                else
                {
                    // Pad with identity if undersized
                    for (u32 j = 0; j < boneCount; ++j)
                    {
                        if (j < arr.size())
                        {
                            WriteMat4(payload, arr[j]);
                        }
                        else
                        {
                            glm::mat4 identity(1.0f);
                            WriteMat4(payload, identity);
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
                    WriteString(payload, skeleton->m_BoneNames[j]);
                }
                else
                {
                    WriteString(payload, {});
                }
            }

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Skeleton)].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Skeleton)].Offset;
        }

        // ── BoneInfluence Section ──
        if (meshSource.HasBoneInfluences())
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfluences)].Offset = StreamPos(payload);

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

            WriteBytes(payload, &biHeader, sizeof(biHeader));
            if (!encoded.Data.empty())
            {
                WriteBytes(payload, encoded.Data.data(), encoded.Data.size());
            }

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfluences)].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfluences)].Offset;
        }

        // ── BoneInfo Section ──
        {
            const auto& boneInfo = meshSource.GetBoneInfo();
            if (!boneInfo.IsEmpty())
            {
                directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfo)].Offset = StreamPos(payload);

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

                directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfo)].Size =
                    StreamPos(payload) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfo)].Offset;
            }
        }

        // ── MorphTarget Section ──
        if (meshSource.HasMorphTargets())
        {
            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::MorphTargets)].Offset = StreamPos(payload);

            const auto& morphTargets = meshSource.GetMorphTargets();
            auto targetCount = morphTargets->GetTargetCount();
            auto vertCount = morphTargets->GetVertexCount();

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

                WriteString(payload, target.Name);

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
                    if (!target.Vertices.empty())
                    {
                        WriteBytes(payload, target.Vertices.data(), target.Vertices.size() * sizeof(MorphTargetVertex));
                    }
                }
            }

            directory.Sections[static_cast<u16>(OMeshFormat::SectionType::MorphTargets)].Size =
                StreamPos(payload) - directory.Sections[static_cast<u16>(OMeshFormat::SectionType::MorphTargets)].Offset;
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

        // ── Obtain the payload stream (decompress if needed) ──
        // We use a unique_ptr to keep the istringstream alive for the compressed path,
        // while using the file stream directly for uncompressed (legacy) files.
        std::unique_ptr<std::istringstream> decompressedStream;
        std::istream* payloadStream = &in;

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
            ReadBytes(in, compressedData.data(), compressedSize);

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
        }

        auto& payload = *payloadStream;

        // For the uncompressed (legacy) path the file stream is positioned
        // right after the header, so absolute seek positions need the base
        // offset added.  For the compressed path the decompressed stream
        // starts at 0 — base is 0.
        u64 const seekBase = decompressedStream ? 0 : payloadBase;

        // Read section directory
        OMeshFormat::SectionDirectory directory;
        ReadBytes(payload, &directory, sizeof(directory));

        TArray<Vertex> vertices;
        TArray<u32> indices;
        TArray<u32> shadowIndices;

        // ── Geometry Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Geometry)];
            if (sec.Size > 0)
            {
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));

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

                    meshSource->AddSubmesh(sub);
                }
            }
        }

        // ── Material Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Materials)];
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
            }
        }

        // ── Skeleton Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::Skeleton)];
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

                // Transform arrays
                auto const readMat4Array = [&](std::vector<glm::mat4>& arr)
                {
                    arr.resize(boneCount);
                    ReadBytes(payload, arr.data(), boneCount * sizeof(glm::mat4));
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
            }
        }

        // ── BoneInfluence Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfluences)];
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
            }
        }

        // ── BoneInfo Section ──
        {
            const auto& sec = directory.Sections[static_cast<u16>(OMeshFormat::SectionType::BoneInfo)];
            if (sec.Size > 0)
            {
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));

                OMeshFormat::BoneInfoHeader biHeader;
                ReadBytes(payload, &biHeader, sizeof(biHeader));

                auto& boneInfo = meshSource->GetBoneInfo();
                for (u32 i = 0; i < biHeader.BoneInfoCount; ++i)
                {
                    OMeshFormat::BoneInfoEntry entry{};
                    ReadBytes(payload, &entry, sizeof(entry));

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
                payload.seekg(static_cast<std::streamoff>(seekBase + sec.Offset));

                OMeshFormat::MorphTargetHeader mtHeader;
                ReadBytes(payload, &mtHeader, sizeof(mtHeader));

                if (mtHeader.TargetCount > OMeshFormat::MaxMorphTargetCount || mtHeader.VertexCount > OMeshFormat::MaxVertexCount)
                {
                    OLO_CORE_ERROR("MeshBinarySerializer::Read: MorphTarget counts exceed safety limits in '{}'", path.string());
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
                        target.IsSparse = true;
                        target.SparseVertices.resize(entry.SparseEntryCount);
                        for (u32 s = 0; s < entry.SparseEntryCount; ++s)
                        {
                            ReadBytes(payload, &target.SparseVertices[s].VertexIndex, sizeof(u32));
                            ReadBytes(payload, &target.SparseVertices[s].Delta, sizeof(MorphTargetVertex));
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

            WriteString(payload, clip->Name);

            // Write bone channels
            for (const auto& boneAnim : clip->BoneAnimations)
            {
                OAnimFormat::BoneChannelHeader chanHeader;
                chanHeader.PositionKeyCount = static_cast<u32>(boneAnim.PositionKeys.size());
                chanHeader.RotationKeyCount = static_cast<u32>(boneAnim.RotationKeys.size());
                chanHeader.ScaleKeyCount = static_cast<u32>(boneAnim.ScaleKeys.size());
                WriteBytes(payload, &chanHeader, sizeof(chanHeader));

                WriteString(payload, boneAnim.BoneName);

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
                WriteString(payload, mk.TargetName);
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

        // ── Obtain the payload stream (decompress if needed) ──
        std::unique_ptr<std::istringstream> decompressedStream;
        std::istream* payloadStream = &in;

        if (header.Flags & OAnimFormat::FlagCompressed)
        {
            if (header.TotalFileSize < sizeof(OAnimFormat::FileHeader))
            {
                OLO_CORE_ERROR("AnimationBinarySerializer::Read: TotalFileSize ({}) is smaller than header in '{}'",
                               header.TotalFileSize, path.string());
                return {};
            }
            auto const compressedSize = header.TotalFileSize - sizeof(OAnimFormat::FileHeader);
            std::vector<u8> compressedData(compressedSize);
            ReadBytes(in, compressedData.data(), compressedSize);

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
        }

        auto& payload = *payloadStream;

        u32 clipCount = 0;
        ReadBytes(payload, &clipCount, sizeof(u32));

        std::vector<OAnimFormat::ClipDirectoryEntry> directory(clipCount);
        ReadBytes(payload, directory.data(), clipCount * sizeof(OAnimFormat::ClipDirectoryEntry));

        std::vector<Ref<AnimationClip>> clips;
        clips.reserve(clipCount);

        for (u32 i = 0; i < clipCount; ++i)
        {
            if (directory[i].Size == 0)
            {
                continue;
            }

            payload.seekg(static_cast<std::streamoff>(directory[i].Offset));

            OAnimFormat::ClipHeader clipHeader;
            ReadBytes(payload, &clipHeader, sizeof(clipHeader));

            auto clip = Ref<AnimationClip>::Create();
            clip->Name = ReadString(payload);
            clip->Duration = clipHeader.Duration;

            // Read bone channels
            clip->BoneAnimations.resize(clipHeader.BoneChannelCount);
            for (u32 c = 0; c < clipHeader.BoneChannelCount; ++c)
            {
                OAnimFormat::BoneChannelHeader chanHeader;
                ReadBytes(payload, &chanHeader, sizeof(chanHeader));

                auto& boneAnim = clip->BoneAnimations[c];
                boneAnim.BoneName = ReadString(payload);

                // Position keys
                boneAnim.PositionKeys.resize(chanHeader.PositionKeyCount);
                for (u32 k = 0; k < chanHeader.PositionKeyCount; ++k)
                {
                    OAnimFormat::PositionKey pk;
                    ReadBytes(payload, &pk, sizeof(pk));
                    boneAnim.PositionKeys[k].Time = pk.Time;
                    boneAnim.PositionKeys[k].Position = { pk.Position[0], pk.Position[1], pk.Position[2] };
                }

                // Rotation keys
                boneAnim.RotationKeys.resize(chanHeader.RotationKeyCount);
                for (u32 k = 0; k < chanHeader.RotationKeyCount; ++k)
                {
                    OAnimFormat::RotationKey rk;
                    ReadBytes(payload, &rk, sizeof(rk));
                    boneAnim.RotationKeys[k].Time = rk.Time;
                    boneAnim.RotationKeys[k].Rotation = glm::quat(rk.Rotation[0], rk.Rotation[1], rk.Rotation[2], rk.Rotation[3]);
                }

                // Scale keys
                boneAnim.ScaleKeys.resize(chanHeader.ScaleKeyCount);
                for (u32 k = 0; k < chanHeader.ScaleKeyCount; ++k)
                {
                    OAnimFormat::ScaleKey sk;
                    ReadBytes(payload, &sk, sizeof(sk));
                    boneAnim.ScaleKeys[k].Time = sk.Time;
                    boneAnim.ScaleKeys[k].Scale = { sk.Scale[0], sk.Scale[1], sk.Scale[2] };
                }
            }

            // Read morph keyframes
            clip->MorphKeyframes.resize(clipHeader.MorphKeyframeCount);
            for (u32 m = 0; m < clipHeader.MorphKeyframeCount; ++m)
            {
                OAnimFormat::MorphKeyframe mkData;
                ReadBytes(payload, &mkData, sizeof(mkData));
                clip->MorphKeyframes[m].Time = mkData.Time;
                clip->MorphKeyframes[m].Weight = mkData.Weight;
                clip->MorphKeyframes[m].TargetName = ReadString(payload);
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
