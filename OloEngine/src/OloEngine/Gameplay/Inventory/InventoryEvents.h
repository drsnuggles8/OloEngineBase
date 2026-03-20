#pragma once

#include "OloEngine/Core/UUID.h"

#include <string>

namespace OloEngine
{
    // Simple event data structs for inventory operations.
    // These are POD-style notification payloads, not Event-derived classes.

    struct ItemAddedEvent
    {
        UUID EntityID;
        UUID ItemInstanceID;
        std::string ItemDefinitionID;
        i32 SlotIndex = -1;
    };

    struct ItemRemovedEvent
    {
        UUID EntityID;
        UUID ItemInstanceID;
        std::string ItemDefinitionID;
        i32 SlotIndex = -1;
    };

    struct ItemEquippedEvent
    {
        UUID EntityID;
        UUID ItemInstanceID;
        std::string SlotName;
    };

    struct ItemUnequippedEvent
    {
        UUID EntityID;
        UUID ItemInstanceID;
        std::string SlotName;
    };

} // namespace OloEngine
