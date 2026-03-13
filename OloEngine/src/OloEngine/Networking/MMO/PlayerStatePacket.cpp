#include "OloEngine/Networking/MMO/PlayerStatePacket.h"

#include <cstring>

namespace OloEngine
{
    std::vector<u8> PlayerStatePacket::Serialize() const
    {
        std::vector<u8> buffer;

        auto writeU32 = [&](u32 val)
        {
            const auto* bytes = reinterpret_cast<const u8*>(&val);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(u32));
        };
        auto writeU64 = [&](u64 val)
        {
            const auto* bytes = reinterpret_cast<const u8*>(&val);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(u64));
        };
        auto writeF32 = [&](f32 val)
        {
            const auto* bytes = reinterpret_cast<const u8*>(&val);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(f32));
        };
        auto writeBool = [&](bool val)
        {
            buffer.push_back(val ? 1 : 0);
        };

        writeU32(ClientID);
        writeU64(EntityUUID);
        writeU32(SourceZoneID);
        writeU32(TargetZoneID);

        writeF32(Position.x);
        writeF32(Position.y);
        writeF32(Position.z);
        writeF32(Rotation.x);
        writeF32(Rotation.y);
        writeF32(Rotation.z);
        writeF32(Scale.x);
        writeF32(Scale.y);
        writeF32(Scale.z);

        writeU32(OwnerClientID);
        writeBool(IsReplicated);

        auto blobSize = static_cast<u32>(GameStateBlob.size());
        writeU32(blobSize);
        buffer.insert(buffer.end(), GameStateBlob.begin(), GameStateBlob.end());

        return buffer;
    }

    PlayerStatePacket PlayerStatePacket::Deserialize(const u8* data, [[maybe_unused]] i64 size)
    {
        PlayerStatePacket packet;
        i64 offset = 0;

        auto readU32 = [&]() -> u32
        {
            u32 val = 0;
            std::memcpy(&val, data + offset, sizeof(u32));
            offset += sizeof(u32);
            return val;
        };
        auto readU64 = [&]() -> u64
        {
            u64 val = 0;
            std::memcpy(&val, data + offset, sizeof(u64));
            offset += sizeof(u64);
            return val;
        };
        auto readF32 = [&]() -> f32
        {
            f32 val = 0.0f;
            std::memcpy(&val, data + offset, sizeof(f32));
            offset += sizeof(f32);
            return val;
        };
        auto readBool = [&]() -> bool
        {
            bool val = data[offset] != 0;
            offset += 1;
            return val;
        };

        packet.ClientID = readU32();
        packet.EntityUUID = readU64();
        packet.SourceZoneID = readU32();
        packet.TargetZoneID = readU32();

        packet.Position.x = readF32();
        packet.Position.y = readF32();
        packet.Position.z = readF32();
        packet.Rotation.x = readF32();
        packet.Rotation.y = readF32();
        packet.Rotation.z = readF32();
        packet.Scale.x = readF32();
        packet.Scale.y = readF32();
        packet.Scale.z = readF32();

        packet.OwnerClientID = readU32();
        packet.IsReplicated = readBool();

        u32 blobSize = readU32();
        if (blobSize > 0)
        {
            packet.GameStateBlob.assign(data + offset, data + offset + blobSize);
        }

        return packet;
    }
} // namespace OloEngine
