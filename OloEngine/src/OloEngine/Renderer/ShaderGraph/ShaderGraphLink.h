#pragma once

#include "OloEngine/Core/UUID.h"

namespace OloEngine
{
    /// A directed edge from an output pin to an input pin in a shader graph
    struct ShaderGraphLink
    {
        UUID ID;
        UUID OutputPinID; // Source (output pin on the upstream node)
        UUID InputPinID;  // Destination (input pin on the downstream node)

        ShaderGraphLink() = default;
        ShaderGraphLink(UUID id, UUID outputPinID, UUID inputPinID)
            : ID(id), OutputPinID(outputPinID), InputPinID(inputPinID)
        {
        }

        bool operator==(const ShaderGraphLink& other) const
        {
            return ID == other.ID;
        }
    };

} // namespace OloEngine
