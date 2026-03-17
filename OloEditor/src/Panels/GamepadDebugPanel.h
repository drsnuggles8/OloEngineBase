#pragma once

namespace OloEngine
{
    class GamepadDebugPanel
    {
    public:
        GamepadDebugPanel() = default;
        ~GamepadDebugPanel() = default;

        void OnImGuiRender(bool* p_open = nullptr);
    };

} // namespace OloEngine
