#pragma once

namespace OloEngine
{
    class NetworkDebugPanel
    {
      public:
        NetworkDebugPanel() = default;

        void OnImGuiRender(bool* p_open = nullptr);
    };
} // namespace OloEngine
