#pragma once

#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

namespace OloEngine
{
    class IInputProvider
    {
      public:
        virtual ~IInputProvider() = default;

        [[nodiscard]] virtual bool IsKeyPressed(KeyCode key) const = 0;
        [[nodiscard]] virtual bool IsMouseButtonPressed(MouseCode button) const = 0;
    };

} // namespace OloEngine
