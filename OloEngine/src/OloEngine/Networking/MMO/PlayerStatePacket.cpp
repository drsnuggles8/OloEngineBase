#include "OloEngine/Networking/MMO/PlayerStatePacket.h"
#include "OloEngine/Serialization/Archive.h"

#include <cstring>

namespace OloEngine
{
    std::vector<u8> PlayerStatePacket::Serialize() const
    {
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
        writer << blobSize;
        if (!GameStateBlob.empty())
        {
            buffer.insert(buffer.end(), GameStateBlob.begin(), GameStateBlob.end());
        }

        return buffer;
    }

    PlayerStatePacket PlayerStatePacket::Deserialize(const u8* data, i64 size)
    {
        PlayerStatePacket packet;

        FMemoryReader reader(data, size);
        reader.ArIsNetArchive = true;

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
        }
        else if (blobSize > kMaxBlobSize)
        {
            OLO_CORE_WARN("[PlayerStatePacket] Rejecting oversized blob: {} bytes (max {})", blobSize, kMaxBlobSize);
            reader.SetError();
        }

        return packet;
    }
} // namespace OloEngine
