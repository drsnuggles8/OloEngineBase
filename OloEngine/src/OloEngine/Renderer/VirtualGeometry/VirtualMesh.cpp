#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"

// For VirtualMeshBuildConfig — the cook fingerprint in the blob header hashes its defaults, so
// changing a default (SimplifyRatio, MaxClusterTriangles, …) invalidates every cached DAG.
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace OloEngine
{
    // ── LOD error projection ───────────────────────────────────────

    f32 VirtualLODBounds::ProjectError(const glm::vec3& cameraPosition, f32 zNear, f32 projectionScale) const
    {
        if (Error >= std::numeric_limits<f32>::max())
        {
            return std::numeric_limits<f32>::infinity();
        }

        f32 const distance = glm::length(Center - cameraPosition);
        f32 const denominator = std::max(distance - Radius, zNear);
        return Error / denominator * (projectionScale * 0.5f);
    }

    // ── DAG cut selection ──────────────────────────────────────────

    bool VirtualMesh::IsClusterSelected(u32 clusterIndex, f32 errorThreshold) const
    {
        if (clusterIndex >= Clusters.size())
        {
            return false;
        }

        const VirtualCluster& cluster = Clusters[clusterIndex];
        if (cluster.GroupIndex < 0 || static_cast<sizet>(cluster.GroupIndex) >= Groups.size())
        {
            return false;
        }

        // 1. The coarser replacement of this cluster is not accurate enough yet.
        if (!(Groups[static_cast<sizet>(cluster.GroupIndex)].LODBounds.Error > errorThreshold))
        {
            return false;
        }

        // 2. This cluster itself is an acceptable simplification (or original geometry).
        if (cluster.RefinedGroup < 0)
        {
            return true;
        }
        if (static_cast<sizet>(cluster.RefinedGroup) >= Groups.size())
        {
            return false;
        }
        return Groups[static_cast<sizet>(cluster.RefinedGroup)].LODBounds.Error <= errorThreshold;
    }

    std::vector<u32> VirtualMesh::SelectClusters(f32 errorThreshold) const
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u32> selected;
        auto clusterCount = static_cast<u32>(Clusters.size());
        for (u32 i = 0; i < clusterCount; ++i)
        {
            if (IsClusterSelected(i, errorThreshold))
            {
                selected.push_back(i);
            }
        }
        return selected;
    }

    bool VirtualMesh::IsClusterSelectedProjected(u32 clusterIndex, const glm::vec3& cameraPosition,
                                                 f32 zNear, f32 projectionScale, f32 threshold) const
    {
        if (clusterIndex >= Clusters.size())
        {
            return false;
        }

        const VirtualCluster& cluster = Clusters[clusterIndex];
        if (cluster.GroupIndex < 0 || static_cast<sizet>(cluster.GroupIndex) >= Groups.size())
        {
            return false;
        }

        const VirtualLODBounds& parentBounds = Groups[static_cast<sizet>(cluster.GroupIndex)].LODBounds;
        if (!(parentBounds.ProjectError(cameraPosition, zNear, projectionScale) > threshold))
        {
            return false;
        }

        if (cluster.RefinedGroup < 0)
        {
            return true;
        }
        if (static_cast<sizet>(cluster.RefinedGroup) >= Groups.size())
        {
            return false;
        }
        const VirtualLODBounds& selfBounds = Groups[static_cast<sizet>(cluster.RefinedGroup)].LODBounds;
        return selfBounds.ProjectError(cameraPosition, zNear, projectionScale) <= threshold;
    }

    std::vector<u32> VirtualMesh::SelectClustersProjected(const glm::vec3& cameraPosition,
                                                          f32 zNear, f32 projectionScale, f32 threshold) const
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u32> selected;
        auto clusterCount = static_cast<u32>(Clusters.size());
        for (u32 i = 0; i < clusterCount; ++i)
        {
            if (IsClusterSelectedProjected(i, cameraPosition, zNear, projectionScale, threshold))
            {
                selected.push_back(i);
            }
        }
        return selected;
    }

    // ── Serialization ──────────────────────────────────────────────

    namespace VirtualMeshSerializer
    {
        namespace
        {
            constexpr u32 kMagic = 0x4D47564F; // 'OVGM' little-endian
            // v2 (issue #629): header carries the COOK IDENTITY — builder version + build-config
            // fingerprint — after the wire version. A v1 blob has no way to prove which builder
            // produced it, so it is rejected outright and re-cooked; see kVirtualMeshBuilderVersion.
            constexpr u32 kVersion = 2;
            constexpr sizet kHeaderSize = 11 * sizeof(u32);
            constexpr sizet kVertexWireSize = 8 * sizeof(f32); // px py pz nx ny nz u v
            constexpr sizet kClusterWireSize = 6 * sizeof(u32) + 11 * sizeof(f32);
            constexpr sizet kGroupWireSize = 3 * sizeof(u32) + 5 * sizeof(f32);

            // Allocation-bomb caps for hostile input; far above any realistic cook.
            constexpr u32 kMaxVertices = 64u * 1024 * 1024;
            constexpr u32 kMaxClusters = 16u * 1024 * 1024;
            constexpr u32 kMaxGroups = 16u * 1024 * 1024;
            constexpr u32 kMaxVertexRefs = 256u * 1024 * 1024;
            constexpr u32 kMaxTriangleBytes = 768u * 1024 * 1024;
            constexpr u32 kMaxClusterVertices = 256;  // local triangle indices are u8
            constexpr u32 kMaxClusterTriangles = 512; // meshoptimizer implementation limit

            class BlobWriter
            {
              public:
                explicit BlobWriter(sizet reserveBytes)
                {
                    m_Data.reserve(reserveBytes);
                }

                template<typename T>
                void Write(const T& value)
                {
                    static_assert(std::is_trivially_copyable_v<T>);
                    sizet const offset = m_Data.size();
                    m_Data.resize(offset + sizeof(T));
                    std::memcpy(m_Data.data() + offset, &value, sizeof(T));
                }

                void WriteBytes(const u8* bytes, sizet count)
                {
                    if (count > 0)
                    {
                        m_Data.insert(m_Data.end(), bytes, bytes + count);
                    }
                }

                [[nodiscard]] std::vector<u8> Take()
                {
                    return std::move(m_Data);
                }

              private:
                std::vector<u8> m_Data;
            };

            class BlobReader
            {
              public:
                explicit BlobReader(std::span<const u8> blob) : m_Blob(blob) {}

                template<typename T>
                [[nodiscard]] bool Read(T& out)
                {
                    static_assert(std::is_trivially_copyable_v<T>);
                    if (sizeof(T) > m_Blob.size() - m_Offset)
                    {
                        return false;
                    }
                    std::memcpy(&out, m_Blob.data() + m_Offset, sizeof(T));
                    m_Offset += sizeof(T);
                    return true;
                }

                [[nodiscard]] bool ReadBytes(u8* out, sizet count)
                {
                    if (count > m_Blob.size() - m_Offset)
                    {
                        return false;
                    }
                    if (count > 0)
                    {
                        std::memcpy(out, m_Blob.data() + m_Offset, count);
                        m_Offset += count;
                    }
                    return true;
                }

                [[nodiscard]] sizet Remaining() const
                {
                    return m_Blob.size() - m_Offset;
                }

              private:
                std::span<const u8> m_Blob;
                sizet m_Offset = 0;
            };

            [[nodiscard]] bool ReadFiniteF32(BlobReader& reader, f32& out)
            {
                return reader.Read(out) && std::isfinite(out);
            }

            [[nodiscard]] bool ReadFiniteVec3(BlobReader& reader, glm::vec3& out)
            {
                return ReadFiniteF32(reader, out.x) && ReadFiniteF32(reader, out.y) && ReadFiniteF32(reader, out.z);
            }

            [[nodiscard]] sizet ExpectedBlobSize(sizet vertexCount, sizet clusterCount, sizet groupCount,
                                                 sizet vertexRefCount, sizet triangleByteCount)
            {
                return kHeaderSize +
                       vertexCount * kVertexWireSize +
                       clusterCount * kClusterWireSize +
                       groupCount * kGroupWireSize +
                       vertexRefCount * sizeof(u32) +
                       triangleByteCount;
            }

            struct WireCounts
            {
                u32 VertexCount = 0;
                u32 ClusterCount = 0;
                u32 GroupCount = 0;
                u32 VertexRefCount = 0;
                u32 TriangleByteCount = 0;
                u32 LevelCount = 0;
                u32 SourceTriangleCount = 0;
            };

            // Every stage below treats the blob as hostile: read, then validate before use,
            // and return false on the first inconsistency.

            [[nodiscard]] bool ReadAndValidateHeader(BlobReader& reader, std::span<const u8> blob, WireCounts& counts)
            {
                u32 magic = 0;
                u32 version = 0;
                if (!reader.Read(magic) || magic != kMagic || !reader.Read(version) || version != kVersion)
                {
                    return false;
                }

                // Cook identity. A blob produced by a different builder — or with different
                // build-config defaults — is structurally valid but geometrically stale, and
                // nothing downstream would ever notice (the DAG carries its own vertex copy).
                // Reject it here; the registry falls back to a runtime build.
                u32 builderVersion = 0;
                u32 configFingerprint = 0;
                if (!reader.Read(builderVersion) || builderVersion != kVirtualMeshBuilderVersion ||
                    !reader.Read(configFingerprint) || configFingerprint != CurrentCookFingerprint())
                {
                    return false;
                }

                if (!reader.Read(counts.VertexCount) || !reader.Read(counts.ClusterCount) ||
                    !reader.Read(counts.GroupCount) || !reader.Read(counts.VertexRefCount) ||
                    !reader.Read(counts.TriangleByteCount) || !reader.Read(counts.LevelCount) ||
                    !reader.Read(counts.SourceTriangleCount))
                {
                    return false;
                }

                if (counts.VertexCount > kMaxVertices || counts.ClusterCount > kMaxClusters ||
                    counts.GroupCount > kMaxGroups || counts.VertexRefCount > kMaxVertexRefs ||
                    counts.TriangleByteCount > kMaxTriangleBytes || counts.LevelCount > kMaxVirtualMeshLevels)
                {
                    return false;
                }

                // A valid mesh has matching cluster/group presence and an exactly-sized payload.
                if ((counts.ClusterCount == 0) != (counts.GroupCount == 0))
                {
                    return false;
                }
                return blob.size() == ExpectedBlobSize(counts.VertexCount, counts.ClusterCount, counts.GroupCount,
                                                       counts.VertexRefCount, counts.TriangleByteCount);
            }

            [[nodiscard]] bool ReadVertices(BlobReader& reader, VirtualMesh& mesh, const WireCounts& counts)
            {
                mesh.Vertices.resize(counts.VertexCount);
                for (Vertex& vertex : mesh.Vertices)
                {
                    if (!ReadFiniteVec3(reader, vertex.Position) || !ReadFiniteVec3(reader, vertex.Normal) ||
                        !ReadFiniteF32(reader, vertex.TexCoord.x) || !ReadFiniteF32(reader, vertex.TexCoord.y))
                    {
                        return false;
                    }
                }
                return true;
            }

            [[nodiscard]] bool ReadAndValidateClusters(BlobReader& reader, VirtualMesh& mesh, const WireCounts& counts)
            {
                // Cluster vertex/triangle windows must tile their arrays contiguously in
                // emission order (exactly as the builder emits them). This rejects
                // overlapping-window blobs whose summed triangle counts would vastly exceed
                // the payload size — an amplification trap for any consumer that expands
                // per-cluster data.
                u64 runningVertexOffset = 0;
                u64 runningTriangleOffset = 0;

                mesh.Clusters.resize(counts.ClusterCount);
                for (VirtualCluster& cluster : mesh.Clusters)
                {
                    if (!reader.Read(cluster.VertexOffset) || !reader.Read(cluster.TriangleOffset) ||
                        !reader.Read(cluster.VertexCount) || !reader.Read(cluster.TriangleCount) ||
                        !reader.Read(cluster.GroupIndex) || !reader.Read(cluster.RefinedGroup) ||
                        !ReadFiniteVec3(reader, cluster.BoundsCenter) || !ReadFiniteF32(reader, cluster.BoundsRadius) ||
                        !ReadFiniteVec3(reader, cluster.ConeApex) || !ReadFiniteVec3(reader, cluster.ConeAxis) ||
                        !ReadFiniteF32(reader, cluster.ConeCutoff))
                    {
                        return false;
                    }

                    if (cluster.VertexCount == 0 || cluster.VertexCount > kMaxClusterVertices ||
                        cluster.TriangleCount == 0 || cluster.TriangleCount > kMaxClusterTriangles ||
                        cluster.BoundsRadius < 0.0f)
                    {
                        return false;
                    }
                    if (cluster.VertexOffset != runningVertexOffset || cluster.TriangleOffset != runningTriangleOffset)
                    {
                        return false;
                    }
                    runningVertexOffset += cluster.VertexCount;
                    runningTriangleOffset += static_cast<u64>(cluster.TriangleCount) * 3;

                    if (cluster.GroupIndex < 0 || static_cast<u32>(cluster.GroupIndex) >= counts.GroupCount ||
                        cluster.RefinedGroup < -1 ||
                        (cluster.RefinedGroup >= 0 && static_cast<u32>(cluster.RefinedGroup) >= counts.GroupCount))
                    {
                        return false;
                    }
                }

                return runningVertexOffset == counts.VertexRefCount && runningTriangleOffset == counts.TriangleByteCount;
            }

            [[nodiscard]] bool ReadAndValidateGroups(BlobReader& reader, VirtualMesh& mesh, const WireCounts& counts)
            {
                u64 runningFirstCluster = 0;
                mesh.Groups.resize(counts.GroupCount);
                for (VirtualClusterGroup& group : mesh.Groups)
                {
                    if (!reader.Read(group.Depth) || !reader.Read(group.FirstCluster) || !reader.Read(group.ClusterCount) ||
                        !ReadFiniteVec3(reader, group.LODBounds.Center) || !ReadFiniteF32(reader, group.LODBounds.Radius) ||
                        !ReadFiniteF32(reader, group.LODBounds.Error))
                    {
                        return false;
                    }

                    if (group.Depth >= counts.LevelCount || group.LODBounds.Radius < 0.0f || group.LODBounds.Error < 0.0f)
                    {
                        return false;
                    }
                    // Group member ranges must tile Clusters contiguously in emission order.
                    if (group.FirstCluster != runningFirstCluster || group.ClusterCount == 0)
                    {
                        return false;
                    }
                    runningFirstCluster += group.ClusterCount;
                }
                return runningFirstCluster == counts.ClusterCount;
            }

            // Cross-checks between clusters and groups: membership agreement plus the DAG
            // invariants the selection contract relies on (see VirtualMesh.h) — without
            // them a crafted blob makes SelectClusters return overlapping or holey cuts.
            [[nodiscard]] bool ValidateClusterGroupTopology(const VirtualMesh& mesh)
            {
                auto groupCount = mesh.Groups.size();
                for (sizet g = 0; g < groupCount; ++g)
                {
                    const VirtualClusterGroup& group = mesh.Groups[g];
                    sizet const end = static_cast<sizet>(group.FirstCluster) + group.ClusterCount;
                    for (sizet c = group.FirstCluster; c < end; ++c)
                    {
                        if (mesh.Clusters[c].GroupIndex != static_cast<i32>(g))
                        {
                            return false;
                        }
                    }
                }

                std::vector<bool> groupIsRefinedFrom(groupCount, false);
                for (const VirtualCluster& cluster : mesh.Clusters)
                {
                    if (cluster.RefinedGroup < 0)
                    {
                        continue;
                    }
                    const VirtualClusterGroup& refined = mesh.Groups[static_cast<sizet>(cluster.RefinedGroup)];
                    const VirtualClusterGroup& member = mesh.Groups[static_cast<sizet>(cluster.GroupIndex)];

                    // Refinement edges go strictly up the DAG (also rejects RefinedGroup ==
                    // GroupIndex), never leave a terminal group, and never decrease error.
                    if (refined.Depth >= member.Depth ||
                        refined.LODBounds.Error >= std::numeric_limits<f32>::max() ||
                        member.LODBounds.Error < refined.LODBounds.Error)
                    {
                        return false;
                    }
                    groupIsRefinedFrom[static_cast<sizet>(cluster.RefinedGroup)] = true;
                }

                // Terminal closure: a group that produced no refined clusters must be
                // terminal (FLT_MAX error), otherwise thresholds above its error select
                // nothing in its region and the cut has a hole.
                for (sizet g = 0; g < groupCount; ++g)
                {
                    if (!groupIsRefinedFrom[g] && mesh.Groups[g].LODBounds.Error < std::numeric_limits<f32>::max())
                    {
                        return false;
                    }
                }
                return true;
            }

            [[nodiscard]] bool ReadAndValidateGeometry(BlobReader& reader, VirtualMesh& mesh, const WireCounts& counts)
            {
                mesh.ClusterVertexRefs.resize(counts.VertexRefCount);
                if (!reader.ReadBytes(reinterpret_cast<u8*>(mesh.ClusterVertexRefs.data()),
                                      static_cast<sizet>(counts.VertexRefCount) * sizeof(u32)))
                {
                    return false;
                }
                for (u32 const ref : mesh.ClusterVertexRefs)
                {
                    if (ref >= counts.VertexCount)
                    {
                        return false;
                    }
                }

                mesh.ClusterTriangles.resize(counts.TriangleByteCount);
                if (!reader.ReadBytes(mesh.ClusterTriangles.data(), counts.TriangleByteCount))
                {
                    return false;
                }

                // Local triangle indices must stay inside their cluster's vertex window.
                // With the tiling check in ReadAndValidateClusters this scan is exactly
                // linear in the payload size.
                for (const VirtualCluster& cluster : mesh.Clusters)
                {
                    sizet const triangleEnd = static_cast<sizet>(cluster.TriangleOffset) + static_cast<sizet>(cluster.TriangleCount) * 3;
                    for (sizet t = cluster.TriangleOffset; t < triangleEnd; ++t)
                    {
                        if (mesh.ClusterTriangles[t] >= cluster.VertexCount)
                        {
                            return false;
                        }
                    }
                }

                return reader.Remaining() == 0;
            }
        } // namespace

        u32 CurrentCookFingerprint()
        {
            // FNV-1a over the builder version and each config field individually (never the
            // raw struct — padding bytes would make the fingerprint non-deterministic).
            constexpr VirtualMeshBuildConfig kDefaults{};
            u32 hash = 2166136261u;
            auto mix = [&hash](u32 word)
            {
                for (u32 byte = 0; byte < 4; ++byte)
                {
                    hash ^= (word >> (byte * 8)) & 0xFFu;
                    hash *= 16777619u;
                }
            };
            auto mixF32 = [&mix](f32 value)
            {
                u32 bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                mix(bits);
            };

            mix(kVirtualMeshBuilderVersion);
            mix(kDefaults.MaxClusterVertices);
            mix(kDefaults.MaxClusterTriangles);
            mix(kDefaults.MinClusterTriangles);
            mix(kDefaults.TargetGroupSize);
            mixF32(kDefaults.SimplifyRatio);
            mixF32(kDefaults.StuckThreshold);
            mixF32(kDefaults.ClusterSplitFactor);
            mix(kDefaults.MaxLevels);
            return hash;
        }

        std::vector<u8> SerializeToBlob(const VirtualMesh& mesh)
        {
            OLO_PROFILE_FUNCTION();

            BlobWriter writer(ExpectedBlobSize(mesh.Vertices.size(), mesh.Clusters.size(), mesh.Groups.size(),
                                               mesh.ClusterVertexRefs.size(), mesh.ClusterTriangles.size()));

            writer.Write(kMagic);
            writer.Write(kVersion);
            writer.Write(kVirtualMeshBuilderVersion);
            writer.Write(CurrentCookFingerprint());
            writer.Write(static_cast<u32>(mesh.Vertices.size()));
            writer.Write(static_cast<u32>(mesh.Clusters.size()));
            writer.Write(static_cast<u32>(mesh.Groups.size()));
            writer.Write(static_cast<u32>(mesh.ClusterVertexRefs.size()));
            writer.Write(static_cast<u32>(mesh.ClusterTriangles.size()));
            writer.Write(mesh.LevelCount);
            writer.Write(mesh.SourceTriangleCount);

            for (const Vertex& vertex : mesh.Vertices)
            {
                writer.Write(vertex.Position);
                writer.Write(vertex.Normal);
                writer.Write(vertex.TexCoord);
            }

            for (const VirtualCluster& cluster : mesh.Clusters)
            {
                writer.Write(cluster.VertexOffset);
                writer.Write(cluster.TriangleOffset);
                writer.Write(cluster.VertexCount);
                writer.Write(cluster.TriangleCount);
                writer.Write(cluster.GroupIndex);
                writer.Write(cluster.RefinedGroup);
                writer.Write(cluster.BoundsCenter);
                writer.Write(cluster.BoundsRadius);
                writer.Write(cluster.ConeApex);
                writer.Write(cluster.ConeAxis);
                writer.Write(cluster.ConeCutoff);
            }

            for (const VirtualClusterGroup& group : mesh.Groups)
            {
                writer.Write(group.Depth);
                writer.Write(group.FirstCluster);
                writer.Write(group.ClusterCount);
                writer.Write(group.LODBounds.Center);
                writer.Write(group.LODBounds.Radius);
                writer.Write(group.LODBounds.Error);
            }

            writer.WriteBytes(reinterpret_cast<const u8*>(mesh.ClusterVertexRefs.data()),
                              mesh.ClusterVertexRefs.size() * sizeof(u32));
            writer.WriteBytes(mesh.ClusterTriangles.data(), mesh.ClusterTriangles.size());

            return writer.Take();
        }

        bool DeserializeFromBlob(std::span<const u8> blob, VirtualMesh& out)
        {
            OLO_PROFILE_FUNCTION();

            BlobReader reader(blob);
            WireCounts counts;

            // Parse into a local mesh; `out` is only touched on full success so a failed
            // load can never leave a half-populated mesh behind.
            VirtualMesh mesh;
            if (!ReadAndValidateHeader(reader, blob, counts) ||
                !ReadVertices(reader, mesh, counts) ||
                !ReadAndValidateClusters(reader, mesh, counts) ||
                !ReadAndValidateGroups(reader, mesh, counts) ||
                !ValidateClusterGroupTopology(mesh) ||
                !ReadAndValidateGeometry(reader, mesh, counts))
            {
                return false;
            }

            mesh.LevelCount = counts.LevelCount;
            mesh.SourceTriangleCount = counts.SourceTriangleCount;

            out = std::move(mesh);
            return true;
        }

        // ── Set format ("OVGS"): a thin container over the single-mesh blob ──────────
        //
        // Layout: magic, version, partCount, then per part:
        //   submeshIndex (u32), materialIndex (u32), blobSize (u64), blobSize bytes of OVGM.
        // Every part is validated by the hardened OVGM reader above, so there is no second
        // geometry parser to keep in sync.

        namespace
        {
            constexpr u32 kSetMagic = 0x5347564F; // 'OVGS' little-endian
            // v2: matches the OVGM v2 bump (cook identity). Every part's OVGM header carries the
            // identity check, so this is really only a fast reject for a wholesale-stale set.
            constexpr u32 kSetVersion = 2;

            // A source mesh with more parts than this is corrupt, not merely large: the
            // builder rejects a mesh whose submesh count exceeds it, and no real asset comes
            // close (Sponza, the densest thing we ship, has a few hundred).
            constexpr u32 kMaxParts = 65'536;

            void AppendU32(std::vector<u8>& out, u32 value)
            {
                for (u32 byte = 0; byte < 4; ++byte)
                {
                    out.push_back(static_cast<u8>((value >> (byte * 8)) & 0xFFu));
                }
            }

            void AppendU64(std::vector<u8>& out, u64 value)
            {
                for (u32 byte = 0; byte < 8; ++byte)
                {
                    out.push_back(static_cast<u8>((value >> (byte * 8)) & 0xFFu));
                }
            }

            // Reads a little-endian value, bounds-checked against the blob. Advances `cursor`
            // only on success, so a truncated blob can never be partially consumed.
            template<typename T>
            [[nodiscard]] bool ReadLE(std::span<const u8> blob, sizet& cursor, T& out)
            {
                constexpr sizet kSize = sizeof(T);
                if (cursor + kSize > blob.size())
                {
                    return false;
                }
                T value = 0;
                for (sizet byte = 0; byte < kSize; ++byte)
                {
                    value |= static_cast<T>(blob[cursor + byte]) << (byte * 8);
                }
                cursor += kSize;
                out = value;
                return true;
            }
        } // namespace

        std::vector<u8> SerializeSetToBlob(const VirtualMeshSet& set)
        {
            std::vector<u8> out;
            AppendU32(out, kSetMagic);
            AppendU32(out, kSetVersion);
            AppendU32(out, static_cast<u32>(set.Parts.size()));

            for (const auto& part : set.Parts)
            {
                std::vector<u8> const partBlob = SerializeToBlob(part.Dag);
                AppendU32(out, part.SubmeshIndex);
                AppendU32(out, part.MaterialIndex);
                AppendU64(out, static_cast<u64>(partBlob.size()));
                out.insert(out.end(), partBlob.begin(), partBlob.end());
            }
            return out;
        }

        bool DeserializeSetFromBlob(std::span<const u8> blob, VirtualMeshSet& out)
        {
            sizet cursor = 0;
            u32 magic = 0;
            if (!ReadLE(blob, cursor, magic))
            {
                return false;
            }

            // Back-compat: a cook written before multi-submesh support is a bare single-mesh
            // blob. Read it as a one-part set rather than forcing a re-cook of every asset.
            if (magic == kMagic)
            {
                VirtualMeshPart part;
                if (!DeserializeFromBlob(blob, part.Dag))
                {
                    return false;
                }
                VirtualMeshSet parsed;
                parsed.Parts.push_back(std::move(part));
                out = std::move(parsed);
                return true;
            }

            if (magic != kSetMagic)
            {
                return false;
            }

            u32 version = 0;
            u32 partCount = 0;
            if (!ReadLE(blob, cursor, version) || version != kSetVersion ||
                !ReadLE(blob, cursor, partCount) || partCount == 0 || partCount > kMaxParts)
            {
                return false;
            }

            VirtualMeshSet parsed;
            parsed.Parts.reserve(partCount);
            for (u32 i = 0; i < partCount; ++i)
            {
                VirtualMeshPart part;
                u64 partSize = 0;
                if (!ReadLE(blob, cursor, part.SubmeshIndex) ||
                    !ReadLE(blob, cursor, part.MaterialIndex) ||
                    !ReadLE(blob, cursor, partSize))
                {
                    return false;
                }
                if (partSize == 0 || cursor + static_cast<sizet>(partSize) > blob.size())
                {
                    return false;
                }
                if (!DeserializeFromBlob(blob.subspan(cursor, static_cast<sizet>(partSize)), part.Dag))
                {
                    return false;
                }
                cursor += static_cast<sizet>(partSize);
                parsed.Parts.push_back(std::move(part));
            }

            // Exact-size: trailing bytes mean the blob is not what it claims to be.
            if (cursor != blob.size())
            {
                return false;
            }

            out = std::move(parsed);
            return true;
        }
    } // namespace VirtualMeshSerializer

    u32 VirtualMeshSet::TotalSourceTriangles() const
    {
        u32 total = 0;
        for (const auto& part : Parts)
        {
            total += part.Dag.SourceTriangleCount;
        }
        return total;
    }

    sizet VirtualMeshSet::TotalClusters() const
    {
        sizet total = 0;
        for (const auto& part : Parts)
        {
            total += part.Dag.Clusters.size();
        }
        return total;
    }
} // namespace OloEngine
