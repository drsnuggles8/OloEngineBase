#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Serialization/GroomBinaryFormat.h"
#include "OloEngine/Serialization/ZlibSection.h"

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine
{
    // The format constants exist so a reader TU need not include the asset
    // header. They must not drift from the asset's own limits.
    static_assert(OloGroomFormat::MaxCurveCount == GroomLimits::MaxCurveCount);
    static_assert(OloGroomFormat::MaxPointCount == GroomLimits::MaxPointCount);
    static_assert(OloGroomFormat::MaxGroupCount == GroomLimits::MaxGroupCount);

    namespace
    {
        void AppendBytes(std::vector<u8>& out, const void* data, sizet size)
        {
            const auto* bytes = static_cast<const u8*>(data);
            out.insert(out.end(), bytes, bytes + size);
        }

        void AppendSection(std::vector<u8>& out, OloGroomFormat::SectionType section, const void* data, u64 byteCount)
        {
            OloGroomFormat::SectionFrame frame;
            frame.SectionId = std::to_underlying(section);
            frame.ByteCount = byteCount;
            AppendBytes(out, &frame, sizeof(frame));
            if (byteCount != 0)
            {
                AppendBytes(out, data, static_cast<sizet>(byteCount));
            }
        }

        // Bounds-checked sequential reader over the decompressed payload —
        // identical shape to VolumeSerializer's / LightmapSerializer's.
        struct BufferReader
        {
            const u8* Data = nullptr;
            sizet Size = 0;
            sizet Pos = 0;

            [[nodiscard]] bool Read(void* dst, sizet count)
            {
                if (count > Size - Pos)
                {
                    return false;
                }
                std::memcpy(dst, Data + Pos, count);
                Pos += count;
                return true;
            }

            [[nodiscard]] sizet Remaining() const
            {
                return Size - Pos;
            }
        };

        // Reads one framed section header and checks it is the expected section
        // with the expected byte count, AND that that many bytes are actually
        // present — sizing an allocation from a header-supplied count without
        // that second check is how a corrupt file turns into an OOM.
        [[nodiscard]] bool ExpectSection(BufferReader& reader, OloGroomFormat::SectionType section, u64 expectedBytes,
                                         std::string& outReason)
        {
            OloGroomFormat::SectionFrame frame;
            if (!reader.Read(&frame, sizeof(frame)))
            {
                outReason = std::format("truncated before section {}", std::to_underlying(section));
                return false;
            }
            if (frame.SectionId != std::to_underlying(section))
            {
                outReason = std::format("expected section {} but found section {}",
                                        std::to_underlying(section), frame.SectionId);
                return false;
            }
            if (frame.ByteCount != expectedBytes)
            {
                outReason = std::format("section {} claims {} bytes but the counts in the Info section imply {}",
                                        std::to_underlying(section), frame.ByteCount, expectedBytes);
                return false;
            }
            if (frame.ByteCount > reader.Remaining())
            {
                outReason = std::format("section {} claims {} bytes but only {} payload bytes remain — "
                                        "corrupt or hostile header, refusing to allocate",
                                        std::to_underlying(section), frame.ByteCount, reader.Remaining());
                return false;
            }
            return true;
        }

        // Validates the section frame FIRST, then sizes the destination, then
        // reads. The ORDER is the point: ExpectSection is what bounds the count
        // against the bytes actually present, so resizing before it let a
        // CRC-valid file whose Info section declared PointCount = 256'000'000
        // allocate ~3 GiB before the truncation was ever detected — and
        // std::bad_alloc is not something any caller of DecodeFromBytes
        // catches. Six sections shared that shape; one helper makes the wrong
        // order impossible to write rather than something to re-check.
        template<typename T>
        [[nodiscard]] bool ReadArraySection(BufferReader& reader, OloGroomFormat::SectionType section,
                                            std::vector<T>& out, sizet count, std::string_view label,
                                            std::string& outReason)
        {
            const u64 byteCount = static_cast<u64>(count) * sizeof(T);
            if (!ExpectSection(reader, section, byteCount, outReason))
            {
                outReason = std::string(label) + " section is missing or malformed: " + outReason;
                return false;
            }
            out.resize(count);
            if (count != 0 && !reader.Read(out.data(), static_cast<sizet>(byteCount)))
            {
                outReason = std::string(label) + " section is truncated";
                return false;
            }
            return true;
        }

        // Reads a u32-length-prefixed string, bounded by both the format's
        // string cap and the bytes actually left in the payload.
        [[nodiscard]] bool ReadString(BufferReader& reader, std::string& out, std::string& outReason,
                                      std::string_view what)
        {
            u32 length = 0;
            if (!reader.Read(&length, sizeof(length)))
            {
                outReason = std::format("truncated before the length of {}", what);
                return false;
            }
            if (length > OloGroomFormat::MaxStringLength)
            {
                outReason = std::format("{} is {} bytes, above the format cap {}",
                                        what, length, OloGroomFormat::MaxStringLength);
                return false;
            }
            if (length > reader.Remaining())
            {
                outReason = std::format("{} claims {} bytes but only {} payload bytes remain",
                                        what, length, reader.Remaining());
                return false;
            }
            out.resize(length);
            if (length != 0 && !reader.Read(out.data(), length))
            {
                outReason = std::format("truncated inside {}", what);
                return false;
            }
            return true;
        }
    } // anonymous namespace

    // ========================================================================
    // Byte-stream encode / decode — the single source of the .ologroom layout.
    // The standalone file and the asset-pack record carry identical bytes.
    // ========================================================================

    bool GroomSerializer::EncodeToBytes(const GroomAsset& groom, std::vector<u8>& outBytes, std::string& outReason)
    {
        // The writer refuses to produce a file the reader would reject, so a
        // corrupt .ologroom can only ever come from outside this process.
        if (!groom.Validate(outReason))
        {
            return false;
        }

        const u32 curveCount = groom.GetCurveCount();
        const u32 pointCount = groom.GetPointCount();
        const u32 groupCount = groom.GetGroupCount();

        const u64 offsetBytes = static_cast<u64>(curveCount + 1) * sizeof(u32);
        const u64 pointBytes = static_cast<u64>(pointCount) * sizeof(f32) * 3u;
        const u64 widthBytes = static_cast<u64>(pointCount) * sizeof(f32);
        const u64 rootUVBytes = static_cast<u64>(curveCount) * sizeof(f32) * 2u;
        const u64 groupIdBytes = static_cast<u64>(curveCount) * sizeof(u16);
        const u64 flagBytes = static_cast<u64>(curveCount) * sizeof(u8);

        u64 groupNameBytes = sizeof(u32); // the count
        for (const auto& name : groom.GetGroupNames())
        {
            groupNameBytes += sizeof(u32) + name.size();
        }
        const u64 provenanceBytes = sizeof(OloGroomFormat::ProvenanceHeader) +
                                    groom.GetProvenance().SourcePath.size() +
                                    groom.GetProvenance().SourceFormat.size();

        const u64 payloadSize = (static_cast<u64>(OloGroomFormat::kSectionCount) * sizeof(OloGroomFormat::SectionFrame)) +
                                sizeof(OloGroomFormat::InfoSection) + offsetBytes + pointBytes + widthBytes +
                                rootUVBytes + groupIdBytes + flagBytes + groupNameBytes + provenanceBytes;
        if (payloadSize > OloGroomFormat::MaxUncompressedPayloadSize)
        {
            outReason = std::format("cooked payload size {} exceeds the format cap {}",
                                    payloadSize, OloGroomFormat::MaxUncompressedPayloadSize);
            return false;
        }

        std::vector<u8> payload;
        payload.reserve(static_cast<sizet>(payloadSize));

        // ── Section 0: Info ──
        {
            OloGroomFormat::InfoSection info;
            info.CurveCount = curveCount;
            info.PointCount = pointCount;
            info.GroupCount = groupCount;
            info.GuideCount = groom.GetGuideCount();
            info.BoundsMin[0] = groom.GetBoundsMin().x;
            info.BoundsMin[1] = groom.GetBoundsMin().y;
            info.BoundsMin[2] = groom.GetBoundsMin().z;
            info.BoundsMax[0] = groom.GetBoundsMax().x;
            info.BoundsMax[1] = groom.GetBoundsMax().y;
            info.BoundsMax[2] = groom.GetBoundsMax().z;
            info.Basis = std::to_underlying(groom.GetBasis());
            AppendSection(payload, OloGroomFormat::SectionType::Info, &info, sizeof(info));
        }

        // ── Sections 1-6: the flat curve arrays ──
        AppendSection(payload, OloGroomFormat::SectionType::CurveOffsets, groom.GetCurveOffsets().data(), offsetBytes);
        // glm::vec3 is three tightly-packed f32 with no padding on every
        // platform this engine targets; the static_assert makes that a build
        // failure rather than a silently wrong stride if it ever changes.
        static_assert(sizeof(glm::vec3) == 3 * sizeof(f32));
        static_assert(sizeof(glm::vec2) == 2 * sizeof(f32));
        AppendSection(payload, OloGroomFormat::SectionType::Points, groom.GetPoints().data(), pointBytes);
        AppendSection(payload, OloGroomFormat::SectionType::Widths, groom.GetPointWidths().data(), widthBytes);
        AppendSection(payload, OloGroomFormat::SectionType::RootUVs, groom.GetRootUVs().data(), rootUVBytes);
        AppendSection(payload, OloGroomFormat::SectionType::CurveGroups, groom.GetCurveGroupIds().data(), groupIdBytes);
        AppendSection(payload, OloGroomFormat::SectionType::CurveFlags, groom.GetCurveFlags().data(), flagBytes);

        // ── Section 7: group names ──
        {
            std::vector<u8> names;
            names.reserve(static_cast<sizet>(groupNameBytes));
            AppendBytes(names, &groupCount, sizeof(groupCount));
            for (const auto& name : groom.GetGroupNames())
            {
                const auto length = static_cast<u32>(name.size());
                AppendBytes(names, &length, sizeof(length));
                AppendBytes(names, name.data(), name.size());
            }
            AppendSection(payload, OloGroomFormat::SectionType::GroupNames, names.data(), names.size());
        }

        // ── Section 8: provenance ──
        {
            const GroomProvenance& provenance = groom.GetProvenance();
            std::vector<u8> bytes;
            bytes.reserve(static_cast<sizet>(provenanceBytes));

            OloGroomFormat::ProvenanceHeader header;
            header.SourceContentHash = provenance.SourceContentHash;
            header.ImporterVersion = provenance.ImporterVersion;
            header.SourcePathLength = static_cast<u32>(provenance.SourcePath.size());
            header.SourceFormatLength = static_cast<u32>(provenance.SourceFormat.size());
            if (header.SourcePathLength > OloGroomFormat::MaxStringLength ||
                header.SourceFormatLength > OloGroomFormat::MaxStringLength)
            {
                outReason = std::format("provenance strings exceed the format cap {}", OloGroomFormat::MaxStringLength);
                return false;
            }
            AppendBytes(bytes, &header, sizeof(header));
            AppendBytes(bytes, provenance.SourcePath.data(), provenance.SourcePath.size());
            AppendBytes(bytes, provenance.SourceFormat.data(), provenance.SourceFormat.size());
            AppendSection(payload, OloGroomFormat::SectionType::Provenance, bytes.data(), bytes.size());
        }

        // ── Compress + header ──
        auto compressed = ZlibSection::Compress(payload.data(), payload.size(), "GroomSerializer");
        if (compressed.empty())
        {
            outReason = "zlib compression of the cooked payload failed";
            return false;
        }

        OloGroomFormat::FileHeader header;
        header.Flags = OloGroomFormat::FlagCompressed;
        header.Checksum = Hash::CRC32(compressed.data(), compressed.size());
        header.UncompressedPayloadSize = payload.size();

        outBytes.clear();
        outBytes.reserve(sizeof(header) + compressed.size());
        AppendBytes(outBytes, &header, sizeof(header));
        AppendBytes(outBytes, compressed.data(), compressed.size());
        return true;
    }

    bool GroomSerializer::DecodeFromBytes(const void* data, sizet size, Ref<GroomAsset>& outGroom, std::string& outReason)
    {
        // All-or-nothing: outGroom is assigned only after every check has
        // passed, so a rejected file leaves the caller's handle untouched
        // rather than half-populated.
        OloGroomFormat::FileHeader header;
        if (!data || size < sizeof(header))
        {
            outReason = std::format("input is {} bytes, too small to hold a .ologroom header", size);
            return false;
        }
        const auto* bytes = static_cast<const u8*>(data);
        std::memcpy(&header, bytes, sizeof(header));

        if (header.Magic != OloGroomFormat::MagicNumber)
        {
            outReason = std::format("invalid magic 0x{:08X} (expected 0x{:08X}) — not a .ologroom",
                                    header.Magic, OloGroomFormat::MagicNumber);
            return false;
        }
        if (header.Version > OloGroomFormat::CurrentVersion)
        {
            outReason = std::format("file is version {} but this build reads at most {} — "
                                    "produced by a newer build; re-import it here",
                                    header.Version, OloGroomFormat::CurrentVersion);
            return false;
        }
        if (header.Version < OloGroomFormat::MinSupportedVersion)
        {
            outReason = std::format("file is version {} below the minimum supported {} — a .ologroom is a "
                                    "derived artifact; re-import instead of migrating",
                                    header.Version, OloGroomFormat::MinSupportedVersion);
            return false;
        }

        const sizet storedPayloadSize = size - sizeof(header);
        const u8* storedPayload = bytes + sizeof(header);
        if (storedPayloadSize == 0 || storedPayloadSize > OloGroomFormat::MaxCompressedPayloadSize)
        {
            outReason = std::format("stored payload size {} is empty or exceeds the cap {}",
                                    storedPayloadSize, OloGroomFormat::MaxCompressedPayloadSize);
            return false;
        }
        if (const auto computed = Hash::CRC32(storedPayload, storedPayloadSize); computed != header.Checksum)
        {
            outReason = std::format("checksum mismatch (stored 0x{:08X}, computed 0x{:08X}) — file is corrupt",
                                    header.Checksum, computed);
            return false;
        }
        if (header.UncompressedPayloadSize == 0 || header.UncompressedPayloadSize > OloGroomFormat::MaxUncompressedPayloadSize)
        {
            outReason = std::format("header claims uncompressed payload size {} (cap {})",
                                    header.UncompressedPayloadSize, OloGroomFormat::MaxUncompressedPayloadSize);
            return false;
        }

        std::vector<u8> decompressed;
        BufferReader reader;
        if ((header.Flags & OloGroomFormat::FlagCompressed) != 0)
        {
            decompressed = ZlibSection::Decompress(storedPayload, storedPayloadSize, header.UncompressedPayloadSize,
                                                   OloGroomFormat::MaxUncompressedPayloadSize, "GroomSerializer");
            if (decompressed.empty())
            {
                outReason = "failed to decompress the payload";
                return false;
            }
            reader = { decompressed.data(), decompressed.size(), 0 };
        }
        else
        {
            if (storedPayloadSize != header.UncompressedPayloadSize)
            {
                outReason = std::format("uncompressed payload size {} does not match the header claim {}",
                                        storedPayloadSize, header.UncompressedPayloadSize);
                return false;
            }
            reader = { storedPayload, storedPayloadSize, 0 };
        }

        // ── Section 0: Info ──
        OloGroomFormat::InfoSection info;
        if (!ExpectSection(reader, OloGroomFormat::SectionType::Info, sizeof(info), outReason) ||
            !reader.Read(&info, sizeof(info)))
        {
            outReason = "Info section is missing or malformed: " + outReason;
            return false;
        }
        if (info.CurveCount == 0 || info.CurveCount > OloGroomFormat::MaxCurveCount)
        {
            outReason = std::format("curve count {} is zero or above the cap {}", info.CurveCount, OloGroomFormat::MaxCurveCount);
            return false;
        }
        if (info.PointCount == 0 || info.PointCount > OloGroomFormat::MaxPointCount)
        {
            outReason = std::format("point count {} is zero or above the cap {}", info.PointCount, OloGroomFormat::MaxPointCount);
            return false;
        }
        if (info.GroupCount == 0 || info.GroupCount > OloGroomFormat::MaxGroupCount)
        {
            outReason = std::format("group count {} is zero or above the cap {}", info.GroupCount, OloGroomFormat::MaxGroupCount);
            return false;
        }
        if (!IsValidGroomCurveBasis(static_cast<i32>(info.Basis)))
        {
            outReason = std::format("curve basis {} is not one this build knows — the file was produced by a "
                                    "build with a wider GroomCurveBasis",
                                    info.Basis);
            return false;
        }

        auto groom = Ref<GroomAsset>::Create();
        groom->m_Basis = static_cast<GroomCurveBasis>(info.Basis);

        // ── Sections 1-6: the flat curve arrays ──
        // glm::vec3/vec2 are tightly packed (static_assert'ed in EncodeToBytes),
        // so sizeof(T) * count is exactly the on-disk byte count for each.
        if (!ReadArraySection(reader, OloGroomFormat::SectionType::CurveOffsets, groom->m_CurveOffsets,
                              static_cast<sizet>(info.CurveCount) + 1u, "CurveOffsets", outReason) ||
            !ReadArraySection(reader, OloGroomFormat::SectionType::Points, groom->m_Points,
                              info.PointCount, "Points", outReason) ||
            !ReadArraySection(reader, OloGroomFormat::SectionType::Widths, groom->m_PointWidths,
                              info.PointCount, "Widths", outReason) ||
            !ReadArraySection(reader, OloGroomFormat::SectionType::RootUVs, groom->m_RootUVs,
                              info.CurveCount, "RootUVs", outReason) ||
            !ReadArraySection(reader, OloGroomFormat::SectionType::CurveGroups, groom->m_CurveGroupIds,
                              info.CurveCount, "CurveGroups", outReason) ||
            !ReadArraySection(reader, OloGroomFormat::SectionType::CurveFlags, groom->m_CurveFlags,
                              info.CurveCount, "CurveFlags", outReason))
        {
            return false;
        }

        // ── Section 7: group names ──
        {
            OloGroomFormat::SectionFrame frame;
            if (!reader.Read(&frame, sizeof(frame)) ||
                frame.SectionId != std::to_underlying(OloGroomFormat::SectionType::GroupNames) ||
                frame.ByteCount > reader.Remaining())
            {
                outReason = "GroupNames section is missing or claims more bytes than remain";
                return false;
            }
            const sizet sectionStart = reader.Pos;
            const sizet sectionEnd = sectionStart + static_cast<sizet>(frame.ByteCount);

            u32 storedGroupCount = 0;
            if (!reader.Read(&storedGroupCount, sizeof(storedGroupCount)))
            {
                outReason = "truncated before the group-name count";
                return false;
            }
            if (storedGroupCount != info.GroupCount)
            {
                outReason = std::format("GroupNames section holds {} names but the Info section declares {} groups",
                                        storedGroupCount, info.GroupCount);
                return false;
            }
            groom->m_GroupNames.resize(storedGroupCount);
            for (u32 g = 0; g < storedGroupCount; ++g)
            {
                if (!ReadString(reader, groom->m_GroupNames[g], outReason, std::format("group name {}", g)))
                {
                    return false;
                }
            }
            if (reader.Pos != sectionEnd)
            {
                outReason = std::format("GroupNames section is {} bytes but its contents consumed {}",
                                        frame.ByteCount, reader.Pos - sectionStart);
                return false;
            }
        }

        // ── Section 8: provenance ──
        {
            OloGroomFormat::SectionFrame frame;
            OloGroomFormat::ProvenanceHeader provenanceHeader;
            if (!reader.Read(&frame, sizeof(frame)) ||
                frame.SectionId != std::to_underlying(OloGroomFormat::SectionType::Provenance) ||
                frame.ByteCount > reader.Remaining() ||
                frame.ByteCount < sizeof(provenanceHeader))
            {
                outReason = "Provenance section is missing or malformed";
                return false;
            }
            const sizet sectionEnd = reader.Pos + static_cast<sizet>(frame.ByteCount);
            if (!reader.Read(&provenanceHeader, sizeof(provenanceHeader)))
            {
                outReason = "truncated inside the provenance header";
                return false;
            }
            if (provenanceHeader.SourcePathLength > OloGroomFormat::MaxStringLength ||
                provenanceHeader.SourceFormatLength > OloGroomFormat::MaxStringLength)
            {
                outReason = std::format("provenance strings exceed the format cap {}", OloGroomFormat::MaxStringLength);
                return false;
            }
            const u64 declared = static_cast<u64>(sizeof(provenanceHeader)) + provenanceHeader.SourcePathLength +
                                 provenanceHeader.SourceFormatLength;
            if (declared != frame.ByteCount)
            {
                outReason = std::format("Provenance section is {} bytes but its string lengths imply {}",
                                        frame.ByteCount, declared);
                return false;
            }

            groom->m_Provenance.SourceContentHash = provenanceHeader.SourceContentHash;
            groom->m_Provenance.ImporterVersion = provenanceHeader.ImporterVersion;
            groom->m_Provenance.SourcePath.resize(provenanceHeader.SourcePathLength);
            groom->m_Provenance.SourceFormat.resize(provenanceHeader.SourceFormatLength);
            if ((provenanceHeader.SourcePathLength != 0 &&
                 !reader.Read(groom->m_Provenance.SourcePath.data(), provenanceHeader.SourcePathLength)) ||
                (provenanceHeader.SourceFormatLength != 0 &&
                 !reader.Read(groom->m_Provenance.SourceFormat.data(), provenanceHeader.SourceFormatLength)))
            {
                outReason = "truncated inside the provenance strings";
                return false;
            }
            if (reader.Pos != sectionEnd)
            {
                outReason = "Provenance section length disagrees with its contents";
                return false;
            }
        }

        if (reader.Remaining() != 0)
        {
            outReason = std::format("{} unexpected trailing payload bytes", reader.Remaining());
            return false;
        }

        // Derived data is recomputed rather than trusted from the file: the
        // bounds and guide count in the Info section are a fast header read for
        // tools, not an authority. Recomputing means a file whose stated bounds
        // disagree with its points cannot hand a consumer a wrong box.
        groom->RecomputeDerivedData();

        // The stated counts are still CHECKED against the recomputation — a
        // disagreement means the file is inconsistent, and saying so is more
        // useful than silently preferring one of the two.
        if (info.GuideCount != groom->GetGuideCount())
        {
            outReason = std::format("Info section declares {} guide curves but the flag array holds {}",
                                    info.GuideCount, groom->GetGuideCount());
            return false;
        }

        if (!groom->Validate(outReason))
        {
            return false;
        }
        outGroom = groom;
        return true;
    }

    // ========================================================================
    // File-level helpers
    // ========================================================================

    namespace
    {
        [[nodiscard]] bool WriteGroomFile(const std::filesystem::path& path, const std::vector<u8>& bytes)
        {
            if (auto const parentDir = path.parent_path(); !parentDir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(parentDir, ec);
                if (ec)
                {
                    OLO_CORE_ERROR("GroomSerializer: failed to create directory '{}': {}", parentDir.string(), ec.message());
                    return false;
                }
            }

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                OLO_CORE_ERROR("GroomSerializer: failed to open '{}' for writing", path.string());
                return false;
            }
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (out.fail())
            {
                OLO_CORE_ERROR("GroomSerializer: failed while writing '{}'", path.string());
                return false;
            }
            return true;
        }

        [[nodiscard]] bool ReadGroomFile(const std::filesystem::path& path, std::vector<u8>& outBytes)
        {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in.is_open())
            {
                OLO_CORE_ERROR("GroomSerializer: failed to open '{}'", path.string());
                return false;
            }
            const auto fileSize = static_cast<u64>(in.tellg());
            if (fileSize < sizeof(OloGroomFormat::FileHeader) ||
                fileSize > sizeof(OloGroomFormat::FileHeader) + OloGroomFormat::MaxCompressedPayloadSize)
            {
                OLO_CORE_ERROR("GroomSerializer: '{}' has implausible size {}", path.string(), fileSize);
                return false;
            }
            in.seekg(0, std::ios::beg);

            outBytes.resize(static_cast<sizet>(fileSize));
            in.read(reinterpret_cast<char*>(outBytes.data()), static_cast<std::streamsize>(outBytes.size()));
            if (in.gcount() != static_cast<std::streamsize>(outBytes.size()))
            {
                OLO_CORE_ERROR("GroomSerializer: short read from '{}'", path.string());
                return false;
            }
            return true;
        }
    } // anonymous namespace

    // ========================================================================
    // AssetSerializer interface
    // ========================================================================

    void GroomSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        auto groom = asset.As<GroomAsset>();
        if (!groom)
        {
            OLO_CORE_ERROR("GroomSerializer::Serialize - asset {} is not a GroomAsset", static_cast<u64>(metadata.Handle));
            return;
        }

        std::vector<u8> bytes;
        std::string reason;
        if (!EncodeToBytes(*groom, bytes, reason))
        {
            OLO_CORE_ERROR("GroomSerializer::Serialize - refusing to write '{}': {}", metadata.FilePath.string(), reason);
            return;
        }

        const auto filepath = Project::GetProjectDirectory() / metadata.FilePath;
        if (WriteGroomFile(filepath, bytes))
        {
            OLO_CORE_TRACE("GroomSerializer: wrote '{}' ({} bytes, {} curves, {} points, {} groups)",
                           filepath.filename().string(), bytes.size(), groom->GetCurveCount(),
                           groom->GetPointCount(), groom->GetGroupCount());
        }
    }

    bool GroomSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // metadata.FilePath is project-root-relative — see the matching
        // comment in TextureSerializer::TryLoadData.
        const auto filepath = Project::GetProjectDirectory() / metadata.FilePath;

        std::vector<u8> bytes;
        if (!ReadGroomFile(filepath, bytes))
        {
            return false;
        }

        Ref<GroomAsset> groom;
        std::string reason;
        if (!DecodeFromBytes(bytes.data(), bytes.size(), groom, reason))
        {
            OLO_CORE_ERROR("GroomSerializer: rejected '{}': {}", filepath.string(), reason);
            return false;
        }

        groom->SetHandle(metadata.Handle);
        if (groom->GetName().empty())
        {
            groom->SetName(filepath.stem().string());
        }
        OLO_CORE_TRACE("GroomSerializer: loaded '{}' ({} curves, {} points, {} groups, {} guides; source '{}' [{}])",
                       filepath.filename().string(), groom->GetCurveCount(), groom->GetPointCount(),
                       groom->GetGroupCount(), groom->GetGuideCount(),
                       groom->GetProvenance().SourcePath, groom->GetProvenance().SourceFormat);
        asset = groom;
        return true;
    }

    bool GroomSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        auto groom = AssetManager::GetAsset<GroomAsset>(handle);
        if (!groom)
        {
            return false;
        }

        // Re-encode from the live asset: a groom keeps all its data resident
        // (no GPU-only residency the way VolumeAsset has), so the pack record
        // is produced from memory rather than by re-reading the source file.
        std::vector<u8> bytes;
        std::string reason;
        if (!EncodeToBytes(*groom, bytes, reason))
        {
            OLO_CORE_ERROR("GroomSerializer::SerializeToAssetPack - refusing to pack asset {}: {}",
                           static_cast<u64>(handle), reason);
            return false;
        }

        outInfo.Offset = stream.GetStreamPosition();
        if (!stream.WriteData(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
        {
            return false;
        }
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> GroomSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        if (assetInfo.PackedSize < sizeof(OloGroomFormat::FileHeader) ||
            assetInfo.PackedSize > sizeof(OloGroomFormat::FileHeader) + OloGroomFormat::MaxCompressedPayloadSize)
        {
            OLO_CORE_ERROR("GroomSerializer::DeserializeFromAssetPack - implausible packed size {} for asset {}",
                           assetInfo.PackedSize, static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        std::vector<u8> bytes(static_cast<sizet>(assetInfo.PackedSize));
        if (!stream.ReadData(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        {
            OLO_CORE_ERROR("GroomSerializer::DeserializeFromAssetPack - failed to read {} bytes for asset {}",
                           assetInfo.PackedSize, static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        Ref<GroomAsset> groom;
        std::string reason;
        if (!DecodeFromBytes(bytes.data(), bytes.size(), groom, reason))
        {
            OLO_CORE_ERROR("GroomSerializer::DeserializeFromAssetPack - rejected asset {}: {}",
                           static_cast<u64>(assetInfo.Handle), reason);
            return nullptr;
        }
        groom->SetHandle(assetInfo.Handle);
        return groom;
    }
} // namespace OloEngine
