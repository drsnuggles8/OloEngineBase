#pragma once

#include "OloEngine/Core/Layer.h"

#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Events/KeyEvent.h"
#include "OloEngine/Events/MouseEvent.h"

namespace OloEngine
{
    class ImGuiLayer : public Layer
    {
      public:
        ImGuiLayer();
        ~ImGuiLayer() override = default;

        void OnAttach() override;
        void OnDetach() override;
        void OnEvent(Event& e) override;

        static void Begin();
        static void End();

        void BlockEvents(bool const block)
        {
            m_BlockEvents = block;
        }

        static void SetDarkThemeColors();

      private:
        bool m_BlockEvents = true;
    };
} // namespace OloEngine
