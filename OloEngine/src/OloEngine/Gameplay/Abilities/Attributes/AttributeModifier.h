#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

namespace OloEngine
{

    struct AttributeModifier
    {
        enum class Operation : u8
        {
            Add,
            Multiply,
            Override
        };

        Operation Op = Operation::Add;
        f32 Magnitude = 0.0f;
        GameplayTag Source; // What applied this modifier (for removal)

        auto operator==(const AttributeModifier&) const -> bool = default;
    };

} // namespace OloEngine
