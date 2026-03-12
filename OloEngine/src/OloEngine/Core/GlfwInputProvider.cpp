#include "OloEnginePCH.h"
#include "OloEngine/Core/GlfwInputProvider.h"
#include "OloEngine/Core/Input.h"

namespace OloEngine
{
    bool GlfwInputProvider::IsKeyPressed(KeyCode key) const
    {
        return Input::IsKeyPressed(key);
    }

    bool GlfwInputProvider::IsMouseButtonPressed(MouseCode button) const
    {
        return Input::IsMouseButtonPressed(button);
    }

} // namespace OloEngine
