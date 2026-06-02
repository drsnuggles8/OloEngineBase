#include "OloEngine/Networking/MMO/PlayerStatePacket.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Serialization/Archive.h"

#include <cstring>
#include <limits>
#include <optional>

namespace OloEngine
{
    std::vector<u8> PlayerStatePacket::Serialize() const
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;

        u32 clientID = ClientID;
        u64 entityUUID = EntityUUID;
        ZoneID sourceZoneID = SourceZoneID;
        ZoneID targetZoneID = TargetZoneID;
        writer << clientID;
        writer << entityUUID;
        writer << sourceZoneID;
        writer << targetZoneID;

        glm::vec3 pos = Position;
        glm::vec3 rot = Rotation;
        glm::vec3 scl = Scale;
        writer << pos.x << pos.y << pos.z;
        writer << rot.x << rot.y << rot.z;
        writer << scl.x << scl.y << scl.z;

        u32 ownerClientID = OwnerClientID;
        writer << ownerClientID;
        u8 replicated = IsReplicated ? 1 : 0;
        writer << replicated;

        auto blobSize = static_cast<u32>(GameStateBlob.size());
        if (GameStateBlob.size() > static_cast<size_t>(std::numeric_limits<u32>::max()))
        {
            OLO_CORE_ERROR("[PlayerStatePacket] GameStateBlob size {} exceeds u32 max", GameStateBlob.size());
            return {};
        }
        writer << blobSize;
        if (!GameStateBlob.empty())
        {
            writer.Serialize(const_cast<u8*>(GameStateBlob.data()), static_cast<i64>(GameStateBlob.size()));
        }

        return buffer;
    }

    std::optional<PlayerStatePacket> PlayerStatePacket::Deserialize(const u8* data, i64 size)
    {
        OLO_PROFILE_FUNCTION();

        if (!data || size <= 0)
        {
            return std::nullopt;
        }

        FMemoryReader reader(data, size);
        reader.ArIsNetArchive = true;

        PlayerStatePacket packet;

        reader << packet.ClientID;
        reader << packet.EntityUUID;
        reader << packet.SourceZoneID;
        reader << packet.TargetZoneID;

        reader << packet.Position.x << packet.Position.y << packet.Position.z;
        reader << packet.Rotation.x << packet.Rotation.y << packet.Rotation.z;
        reader << packet.Scale.x << packet.Scale.y << packet.Scale.z;

        reader << packet.OwnerClientID;
        u8 replicated = 0;
        reader << replicated;
        packet.IsReplicated = (replicated != 0);

        u32 blobSize = 0;
        reader << blobSize;

        // Guard against excessively large blob sizes (allocation DoS)
        static constexpr u32 kMaxBlobSize = 1024 * 1024; // 1 MB
        if (!reader.IsError() && blobSize > 0 && blobSize <= kMaxBlobSize)
        {
            i64 const remaining = size - reader.Tell();
            if (static_cast<i64>(blobSize) <= remaining)
            {
                packet.GameStateBlob.resize(blobSize);
                std::memcpy(packet.GameStateBlob.data(), data + reader.Tell(), blobSize);
            }
            else
            {
                OLO_CORE_WARN("[PlayerStatePacket] Truncated blob: need {} bytes but only {} remaining", blobSize, remaining);
                reader.SetError();
            }
        }
        else if (blobSize > kMaxBlobSize)
        {
            OLO_CORE_WARN("[PlayerStatePacket] Rejecting oversized blob: {} bytes (max {})", blobSize, kMaxBlobSize);
            reader.SetError();
        }
        else
        {
            // No additional handling required.
        }

        if (reader.IsError())
        {
            return std::nullopt;
        }

        // Reject non-finite transforms. A malicious or buggy client can put
        // NaN/Inf into the replicated Position/Rotation/Scale; applying it would
        // corrupt server-side spatial partitioning, interest management, and the
        // Jolt simulation for every player in the zone. Per cpp-coding-quality §2,
        // floats read from the network must be validated — and for untrusted
        // input the safe response is to drop the packet, not clamp it.
        if (!Math::IsFinite(packet.Position) || !Math::IsFinite(packet.Rotation) || !Math::IsFinite(packet.Scale))
        {
            OLO_CORE_WARN("[PlayerStatePacket] Rejecting packet with non-finite transform (client {})", packet.ClientID);
            return std::nullopt;
        }

        return packet;
    }
} // namespace OloEngine
