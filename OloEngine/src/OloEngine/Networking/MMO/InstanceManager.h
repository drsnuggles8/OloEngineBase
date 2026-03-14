#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/MMO/ZoneDefinition.h"
#include "OloEngine/Networking/MMO/ZoneServer.h"

#include <chrono>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    using InstanceID = u32;

    enum class EInstanceType : u8
    {
        Group = 0, // Small group content (5-man dungeons)
        Raid,      // Large group content (10-40 players)
        Scenario,  // Scripted PvE/PvP content
        OpenWorld  // Layered overworld zone
    };

    struct InstanceInfo
    {
        InstanceID ID = 0;
        ZoneID TemplateZoneID = 0;
        EInstanceType Type = EInstanceType::Group;
        u32 MaxPlayers = 5;
        std::chrono::steady_clock::time_point LastPlayerTime;
    };

    // Creates/destroys isolated copies of zones for dungeons, raids, and scenarios.
    class InstanceManager
    {
      public:
        InstanceManager() = default;

        // Create a new instance based on a zone template.
        InstanceID CreateInstance(const ZoneDefinition& templateZone, EInstanceType type, u32 maxPlayers);

        // Destroy an instance and kick all players.
        void DestroyInstance(InstanceID instanceID);

        // Get the zone server for an instance.
        [[nodiscard]] ZoneServer* GetInstance(InstanceID instanceID);

        // Add a player to an instance.
        bool AddPlayerToInstance(InstanceID instanceID, u32 clientID);

        // Remove a player from an instance.
        void RemovePlayerFromInstance(InstanceID instanceID, u32 clientID);

        // Check if instance exists.
        [[nodiscard]] bool HasInstance(InstanceID instanceID) const;

        // Get instance info.
        [[nodiscard]] const InstanceInfo* GetInstanceInfo(InstanceID instanceID) const;

        // Get number of active instances.
        [[nodiscard]] u32 GetInstanceCount() const;

        // Tick all instances. Auto-destroys empty Group/Raid/Scenario instances
        // after a grace period.
        void TickAll(f32 dt, f32 gracePeriodSeconds = 60.0f);

      private:
        std::unordered_map<InstanceID, ZoneServer> m_Instances;
        std::unordered_map<InstanceID, InstanceInfo> m_InstanceInfos;
        InstanceID m_NextInstanceID = 1;
    };
} // namespace OloEngine
