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

        writer << const_cast<u32&>(ClientID);
        writer << const_cast<u64&>(EntityUUID);
        writer << const_cast<ZoneID&>(SourceZoneID);
        writer << const_cast<ZoneID&>(TargetZoneID);

        writer << const_cast<f32&>(Position.x) << const_cast<f32&>(Position.y) << const_cast<f32&>(Position.z);
        writer << const_cast<f32&>(Rotation.x) << const_cast<f32&>(Rotation.y) << const_cast<f32&>(Rotation.z);
        writer << const_cast<f32&>(Scale.x) << const_cast<f32&>(Scale.y) << const_cast<f32&>(Scale.z);

        writer << const_cast<u32&>(OwnerClientID);
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

        if (!reader.IsError() && blobSize > 0)
        {
            i64 const remaining = size - reader.Tell();
            if (static_cast<i64>(blobSize) <= remaining)
            {
                packet.GameStateBlob.resize(blobSize);
                std::memcpy(packet.GameStateBlob.data(), data + reader.Tell(), blobSize);
            }
        }

        return packet;
    }
} // namespace OloEngine
