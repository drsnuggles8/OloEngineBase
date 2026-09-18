#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Serialization/GroomBindingBinaryFormat.h"
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
    static_assert(OloGroomBindingFormat::MaxRootCount == GroomBindingLimits::MaxRootCount);
    static_assert(OloGroomBindingFormat::MaxTargetTriangleCount == GroomBindingLimits::MaxTargetTriangleCount);

    namespace
    {
        void AppendBytes(std::vector<u8>& out, const void* data, sizet size)
        {
            const auto* bytes = static_cast<const u8*>(data);
            out.insert(out.end(), bytes, bytes + size);
        }

        void AppendSection(std::vector<u8>& out, OloGroomBindingFormat::SectionType section, const void* data,
                           u64 byteCount)
        {
            OloGroomBindingFormat::SectionFrame frame;
            frame.SectionId = std::to_underlying(section);
            frame.ByteCount = byteCount;
            AppendBytes(out, &frame, sizeof(frame));
            if (byteCount != 0)
            {
                AppendBytes(out, data, static_cast<sizet>(byteCount));
            }
        }

        // Bounds-checked sequential reader over the decompressed payload — the
        // same shape GroomSerializer's has, deliberately: two readers of two
        // sibling formats that differ in their bounds checking is how one of
        // them ends up without any.
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
        [[nodiscard]] bool ExpectSection(BufferReader& reader, OloGroomBindingFormat::SectionType section,
                                         u64 expectedBytes, std::string& outReason)
        {
            OloGroomBindingFormat::SectionFrame frame;
            if (!reader.Read(&frame, sizeof(frame)))
            {
                outReason = std::format("truncated before section {}", std::to_underlying(section));
                return false;
            }
            if (frame.SectionId != std::to_underlying(section))
            {
                outReason = std::format("expected section {} but found section {}", std::to_underlying(section),
                                        frame.SectionId);
                return false;
            }
            if (frame.ByteCount != expectedBytes)
            {
                outReason = std::format("section {} claims {} bytes but the Info section implies {}",
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
    } // anonymous namespace

    // ========================================================================
    // Byte-stream encode / decode — the single source of the layout.
    // The standalone file and the asset-pack record carry identical bytes.
    // ========================================================================

    bool GroomBindingSerializer::EncodeToBytes(const GroomBindingAsset& binding, std::vector<u8>& outBytes,
                                               std::string& outReason)
    {
        // The writer refuses to produce a file the reader would reject, so a
        // corrupt binding can only ever come from outside this process.
        if (!binding.Validate(outReason))
        {
            return false;
        }

        const auto& groomPath = binding.GetGroomSourcePath();
        const auto& targetPath = binding.GetTargetSourcePath();
        if (groomPath.size() > OloGroomBindingFormat::MaxStringLength ||
            targetPath.size() > OloGroomBindingFormat::MaxStringLength)
        {
            outReason = std::format("source paths exceed the format cap {}", OloGroomBindingFormat::MaxStringLength);
            return false;
        }

        const u32 rootCount = binding.GetRootCount();
        const u64 rootBytes = static_cast<u64>(rootCount) * sizeof(GroomRootBinding);
        const u64 pathBytes = sizeof(OloGroomBindingFormat::PathHeader) + groomPath.size() + targetPath.size();
        const u64 payloadSize =
            (static_cast<u64>(OloGroomBindingFormat::kSectionCount) * sizeof(OloGroomBindingFormat::SectionFrame)) +
            sizeof(OloGroomBindingFormat::InfoSection) + rootBytes + pathBytes;
        if (payloadSize > OloGroomBindingFormat::MaxUncompressedPayloadSize)
        {
            outReason = std::format("cooked payload size {} exceeds the format cap {}", payloadSize,
                                    OloGroomBindingFormat::MaxUncompressedPayloadSize);
            return false;
        }

        std::vector<u8> payload;
        payload.reserve(static_cast<sizet>(payloadSize));

        // ── Section 0: Info ──
        {
            const auto& source = binding.GetSourceSignature();
            const auto& target = binding.GetTargetSignature();

            OloGroomBindingFormat::InfoSection info;
            info.SourceRootHash = source.RootHash;
            info.TargetIndexHash = target.IndexHash;
            info.TargetRestPositionHash = target.RestPositionHash;
            info.TargetSkeletonNameHash = target.SkeletonNameHash;
            info.RootCount = rootCount;
            info.BinderVersion = binding.GetBinderVersion();
            info.SourceCurveCount = source.CurveCount;
            info.SourceGuideCount = source.GuideCount;
            info.TargetVertexCount = target.VertexCount;
            info.TargetIndexCount = target.IndexCount;
            info.TargetBoneCount = target.BoneCount;
            AppendSection(payload, OloGroomBindingFormat::SectionType::Info, &info, sizeof(info));
        }

        // ── Section 1: the flat root array ──
        //
        // Written as a block, which is only legal because GroomRootBinding is a
        // 56-byte hole-free trivially-copyable struct — the static_assert on its
        // size in GroomBinding.h is what makes that a build failure rather than
        // a silently wrong stride if a field is ever added.
        static_assert(sizeof(GroomRootBinding) == 56);
        AppendSection(payload, OloGroomBindingFormat::SectionType::Roots, binding.GetRoots().data(), rootBytes);

        // ── Section 2: the source paths ──
        {
            std::vector<u8> bytes;
            bytes.reserve(static_cast<sizet>(pathBytes));

            OloGroomBindingFormat::PathHeader header;
            header.GroomPathLength = static_cast<u32>(groomPath.size());
            header.TargetPathLength = static_cast<u32>(targetPath.size());
            AppendBytes(bytes, &header, sizeof(header));
            AppendBytes(bytes, groomPath.data(), groomPath.size());
            AppendBytes(bytes, targetPath.data(), targetPath.size());
            AppendSection(payload, OloGroomBindingFormat::SectionType::Paths, bytes.data(), bytes.size());
        }

        // ── Compress + header ──
        auto compressed = ZlibSection::Compress(payload.data(), payload.size(), "GroomBindingSerializer");
        if (compressed.empty())
        {
            outReason = "zlib compression of the cooked payload failed";
            return false;
        }

        OloGroomBindingFormat::FileHeader header;
        header.Flags = OloGroomBindingFormat::FlagCompressed;
        header.Checksum = Hash::CRC32(compressed.data(), compressed.size());
        header.UncompressedPayloadSize = payload.size();

        outBytes.clear();
        outBytes.reserve(sizeof(header) + compressed.size());
        AppendBytes(outBytes, &header, sizeof(header));
        AppendBytes(outBytes, compressed.data(), compressed.size());
        return true;
    }

    bool GroomBindingSerializer::DecodeFromBytes(const void* data, sizet size, Ref<GroomBindingAsset>& outBinding,
                                                 std::string& outReason)
    {
        // All-or-nothing: outBinding is assigned only after every check has
        // passed, so a rejected file leaves the caller's handle untouched rather
        // than half-populated.
        OloGroomBindingFormat::FileHeader header;
        if (!data || size < sizeof(header))
        {
            outReason = std::format("input is {} bytes, too small to hold a .ologroombinding header", size);
            return false;
        }
        const auto* bytes = static_cast<const u8*>(data);
        std::memcpy(&header, bytes, sizeof(header));

        if (header.Magic != OloGroomBindingFormat::MagicNumber)
        {
            outReason = std::format("invalid magic 0x{:08X} (expected 0x{:08X}) — not a .ologroombinding",
                                    header.Magic, OloGroomBindingFormat::MagicNumber);
            return false;
        }
        if (header.Version > OloGroomBindingFormat::CurrentVersion)
        {
            outReason = std::format("file is version {} but this build reads at most {} — "
                                    "produced by a newer build; rebuild the binding here",
                                    header.Version, OloGroomBindingFormat::CurrentVersion);
            return false;
        }
        if (header.Version < OloGroomBindingFormat::MinSupportedVersion)
        {
            outReason = std::format("file is version {} below the minimum supported {} — a binding is a "
                                    "derived artifact; rebuild it instead of migrating",
                                    header.Version, OloGroomBindingFormat::MinSupportedVersion);
            return false;
        }

        const sizet storedPayloadSize = size - sizeof(header);
        const u8* storedPayload = bytes + sizeof(header);
        if (storedPayloadSize == 0 || storedPayloadSize > OloGroomBindingFormat::MaxCompressedPayloadSize)
        {
            outReason = std::format("stored payload size {} is empty or exceeds the cap {}", storedPayloadSize,
                                    OloGroomBindingFormat::MaxCompressedPayloadSize);
            return false;
        }
        if (const auto computed = Hash::CRC32(storedPayload, storedPayloadSize); computed != header.Checksum)
        {
            outReason = std::format("checksum mismatch (stored 0x{:08X}, computed 0x{:08X}) — file is corrupt",
                                    header.Checksum, computed);
            return false;
        }
        if (header.UncompressedPayloadSize == 0 ||
            header.UncompressedPayloadSize > OloGroomBindingFormat::MaxUncompressedPayloadSize)
        {
            outReason = std::format("header claims uncompressed payload size {} (cap {})",
                                    header.UncompressedPayloadSize,
                                    OloGroomBindingFormat::MaxUncompressedPayloadSize);
            return false;
        }

        std::vector<u8> decompressed;
        BufferReader reader;
        if ((header.Flags & OloGroomBindingFormat::FlagCompressed) != 0)
        {
            decompressed = ZlibSection::Decompress(storedPayload, storedPayloadSize, header.UncompressedPayloadSize,
                                                   OloGroomBindingFormat::MaxUncompressedPayloadSize,
                                                   "GroomBindingSerializer");
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
                outReason = std::format("uncompressed file holds {} payload bytes but its header declares {}",
                                        storedPayloadSize, header.UncompressedPayloadSize);
                return false;
            }
            reader = { storedPayload, storedPayloadSize, 0 };
        }

        auto binding = Ref<GroomBindingAsset>::Create();

        // ── Section 0: Info ──
        OloGroomBindingFormat::InfoSection info;
        if (!ExpectSection(reader, OloGroomBindingFormat::SectionType::Info, sizeof(info), outReason))
        {
            outReason = "Info section is missing or malformed: " + outReason;
            return false;
        }
        if (!reader.Read(&info, sizeof(info)))
        {
            outReason = "truncated inside the Info section";
            return false;
        }
        if (info.RootCount > OloGroomBindingFormat::MaxRootCount)
        {
            outReason = std::format("file declares {} roots, above the format cap {}", info.RootCount,
                                    OloGroomBindingFormat::MaxRootCount);
            return false;
        }
        if (info.SourceCurveCount != info.RootCount)
        {
            outReason = std::format("file declares {} roots but {} source curves — one record per curve is the "
                                    "whole contract",
                                    info.RootCount, info.SourceCurveCount);
            return false;
        }
        if ((info.TargetIndexCount % 3u) != 0u ||
            (info.TargetIndexCount / 3u) > OloGroomBindingFormat::MaxTargetTriangleCount)
        {
            outReason = std::format("file declares {} target indices, which is not a plausible triangle count",
                                    info.TargetIndexCount);
            return false;
        }

        binding->m_BinderVersion = info.BinderVersion;
        binding->m_Source.RootHash = info.SourceRootHash;
        binding->m_Source.CurveCount = info.SourceCurveCount;
        binding->m_Source.GuideCount = info.SourceGuideCount;
        binding->m_Target.IndexHash = info.TargetIndexHash;
        binding->m_Target.RestPositionHash = info.TargetRestPositionHash;
        binding->m_Target.SkeletonNameHash = info.TargetSkeletonNameHash;
        binding->m_Target.VertexCount = info.TargetVertexCount;
        binding->m_Target.IndexCount = info.TargetIndexCount;
        binding->m_Target.BoneCount = info.TargetBoneCount;

        // ── Section 1: roots ──
        {
            const u64 rootBytes = static_cast<u64>(info.RootCount) * sizeof(GroomRootBinding);
            // The frame is validated BEFORE the destination is sized — the order
            // is the point, and it is the order GroomSerializer's
            // ReadArraySection enforces for the same reason: a CRC-valid file
            // declaring 8M roots would otherwise allocate 448 MB before the
            // truncation was ever detected.
            if (!ExpectSection(reader, OloGroomBindingFormat::SectionType::Roots, rootBytes, outReason))
            {
                outReason = "Roots section is missing or malformed: " + outReason;
                return false;
            }
            binding->m_Roots.resize(info.RootCount);
            if (info.RootCount != 0 && !reader.Read(binding->m_Roots.data(), static_cast<sizet>(rootBytes)))
            {
                outReason = "Roots section is truncated";
                return false;
            }
        }

        // ── Section 2: paths ──
        {
            OloGroomBindingFormat::SectionFrame frame;
            OloGroomBindingFormat::PathHeader pathHeader;
            if (!reader.Read(&frame, sizeof(frame)) ||
                frame.SectionId != std::to_underlying(OloGroomBindingFormat::SectionType::Paths) ||
                frame.ByteCount > reader.Remaining() || frame.ByteCount < sizeof(pathHeader))
            {
                outReason = "Paths section is missing or malformed";
                return false;
            }
            const sizet sectionEnd = reader.Pos + static_cast<sizet>(frame.ByteCount);
            if (!reader.Read(&pathHeader, sizeof(pathHeader)))
            {
                outReason = "truncated inside the path header";
                return false;
            }
            if (pathHeader.GroomPathLength > OloGroomBindingFormat::MaxStringLength ||
                pathHeader.TargetPathLength > OloGroomBindingFormat::MaxStringLength)
            {
                outReason = std::format("source paths exceed the format cap {}",
                                        OloGroomBindingFormat::MaxStringLength);
                return false;
            }
            const u64 declared =
                static_cast<u64>(sizeof(pathHeader)) + pathHeader.GroomPathLength + pathHeader.TargetPathLength;
            if (declared != frame.ByteCount)
            {
                outReason = std::format("Paths section is {} bytes but its string lengths imply {}",
                                        frame.ByteCount, declared);
                return false;
            }

            binding->m_GroomSourcePath.resize(pathHeader.GroomPathLength);
            binding->m_TargetSourcePath.resize(pathHeader.TargetPathLength);
            if ((pathHeader.GroomPathLength != 0 &&
                 !reader.Read(binding->m_GroomSourcePath.data(), pathHeader.GroomPathLength)) ||
                (pathHeader.TargetPathLength != 0 &&
                 !reader.Read(binding->m_TargetSourcePath.data(), pathHeader.TargetPathLength)))
            {
                outReason = "truncated inside the source paths";
                return false;
            }
            if (reader.Pos != sectionEnd)
            {
                outReason = "Paths section length disagrees with its contents";
                return false;
            }
        }

        if (reader.Remaining() != 0)
        {
            outReason = std::format("{} unexpected trailing payload bytes", reader.Remaining());
            return false;
        }

        // Derived data is recomputed rather than trusted from the file, for the
        // reason GroomSerializer gives: a stated count that disagrees with the
        // records must not be able to hand a consumer a wrong number.
        binding->RecomputeDerivedData();

        if (!binding->Validate(outReason))
        {
            return false;
        }
        outBinding = binding;
        return true;
    }

    // ========================================================================
    // File-level helpers
    // ========================================================================

    namespace
    {
        [[nodiscard]] bool WriteBindingFile(const std::filesystem::path& path, const std::vector<u8>& bytes)
        {
            if (auto const parentDir = path.parent_path(); !parentDir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(parentDir, ec);
                if (ec)
                {
                    OLO_CORE_ERROR("GroomBindingSerializer: failed to create directory '{}': {}",
                                   parentDir.string(), ec.message());
                    return false;
                }
            }

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                OLO_CORE_ERROR("GroomBindingSerializer: failed to open '{}' for writing", path.string());
                return false;
            }
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (out.fail())
            {
                OLO_CORE_ERROR("GroomBindingSerializer: failed while writing '{}'", path.string());
                return false;
            }
            return true;
        }

        [[nodiscard]] bool ReadBindingFile(const std::filesystem::path& path, std::vector<u8>& outBytes)
        {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in.is_open())
            {
                OLO_CORE_ERROR("GroomBindingSerializer: failed to open '{}'", path.string());
                return false;
            }
            const auto fileSize = static_cast<u64>(in.tellg());
            if (fileSize < sizeof(OloGroomBindingFormat::FileHeader) ||
                fileSize > sizeof(OloGroomBindingFormat::FileHeader) + OloGroomBindingFormat::MaxCompressedPayloadSize)
            {
                OLO_CORE_ERROR("GroomBindingSerializer: '{}' has implausible size {}", path.string(), fileSize);
                return false;
            }
            in.seekg(0, std::ios::beg);

            outBytes.resize(static_cast<sizet>(fileSize));
            in.read(reinterpret_cast<char*>(outBytes.data()), static_cast<std::streamsize>(outBytes.size()));
            if (in.gcount() != static_cast<std::streamsize>(outBytes.size()))
            {
                OLO_CORE_ERROR("GroomBindingSerializer: short read from '{}'", path.string());
                return false;
            }
            return true;
        }
    } // anonymous namespace

    // ========================================================================
    // AssetSerializer interface
    // ========================================================================

    void GroomBindingSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        auto binding = asset.As<GroomBindingAsset>();
        if (!binding)
        {
            OLO_CORE_ERROR("GroomBindingSerializer::Serialize - asset {} is not a GroomBindingAsset",
                           static_cast<u64>(metadata.Handle));
            return;
        }

        std::vector<u8> bytes;
        std::string reason;
        if (!EncodeToBytes(*binding, bytes, reason))
        {
            OLO_CORE_ERROR("GroomBindingSerializer::Serialize - refusing to write '{}': {}",
                           metadata.FilePath.string(), reason);
            return;
        }

        const auto filepath = Project::GetProjectDirectory() / metadata.FilePath;
        if (WriteBindingFile(filepath, bytes))
        {
            OLO_CORE_TRACE("GroomBindingSerializer: wrote '{}' ({} bytes, {} roots)", filepath.filename().string(),
                           bytes.size(), binding->GetRootCount());
        }
    }

    bool GroomBindingSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // metadata.FilePath is project-root-relative — see the matching comment
        // in TextureSerializer::TryLoadData.
        const auto filepath = Project::GetProjectDirectory() / metadata.FilePath;

        std::vector<u8> bytes;
        if (!ReadBindingFile(filepath, bytes))
        {
            return false;
        }

        Ref<GroomBindingAsset> binding;
        std::string reason;
        if (!DecodeFromBytes(bytes.data(), bytes.size(), binding, reason))
        {
            OLO_CORE_ERROR("GroomBindingSerializer: rejected '{}': {}", filepath.string(), reason);
            return false;
        }

        binding->SetHandle(metadata.Handle);
        if (binding->GetName().empty())
        {
            binding->SetName(filepath.stem().string());
        }
        OLO_CORE_TRACE("GroomBindingSerializer: loaded '{}' ({} roots, binder v{}, {} exact / {} clamped / {} "
                       "distant, max rest distance {})",
                       filepath.filename().string(), binding->GetRootCount(), binding->GetBinderVersion(),
                       binding->GetQualityCount(GroomRootBindQuality::Exact),
                       binding->GetQualityCount(GroomRootBindQuality::Clamped),
                       binding->GetQualityCount(GroomRootBindQuality::Distant), binding->GetMaxRestDistance());
        asset = binding;
        return true;
    }

    bool GroomBindingSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream,
                                                      AssetSerializationInfo& outInfo) const
    {
        auto binding = AssetManager::GetAsset<GroomBindingAsset>(handle);
        if (!binding)
        {
            return false;
        }

        std::vector<u8> bytes;
        std::string reason;
        if (!EncodeToBytes(*binding, bytes, reason))
        {
            OLO_CORE_ERROR("GroomBindingSerializer::SerializeToAssetPack - refusing to pack asset {}: {}",
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

    Ref<Asset> GroomBindingSerializer::DeserializeFromAssetPack(FileStreamReader& stream,
                                                                const AssetPackFile::AssetInfo& assetInfo) const
    {
        if (assetInfo.PackedSize < sizeof(OloGroomBindingFormat::FileHeader) ||
            assetInfo.PackedSize >
                sizeof(OloGroomBindingFormat::FileHeader) + OloGroomBindingFormat::MaxCompressedPayloadSize)
        {
            OLO_CORE_ERROR("GroomBindingSerializer::DeserializeFromAssetPack - implausible packed size {} for asset {}",
                           assetInfo.PackedSize, static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        std::vector<u8> bytes(static_cast<sizet>(assetInfo.PackedSize));
        if (!stream.ReadData(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        {
            OLO_CORE_ERROR("GroomBindingSerializer::DeserializeFromAssetPack - failed to read {} bytes for asset {}",
                           assetInfo.PackedSize, static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        Ref<GroomBindingAsset> binding;
        std::string reason;
        if (!DecodeFromBytes(bytes.data(), bytes.size(), binding, reason))
        {
            OLO_CORE_ERROR("GroomBindingSerializer::DeserializeFromAssetPack - rejected asset {}: {}",
                           static_cast<u64>(assetInfo.Handle), reason);
            return nullptr;
        }
        binding->SetHandle(assetInfo.Handle);
        return binding;
    }
} // namespace OloEngine
