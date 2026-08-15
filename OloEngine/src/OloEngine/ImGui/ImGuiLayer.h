#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Layer.h"

#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Events/KeyEvent.h"
#include "OloEngine/Events/MouseEvent.h"

namespace OloEngine
{
    class Framebuffer;
    class Texture2D;

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

        // The ImTextureID for an ImGui::Image of an engine render target /
        // texture, backend-neutral (#691 Phase 8). On GL this is the raw GL
        // texture name (what call sites always passed); on Vulkan it is an
        // imgui_impl_vulkan descriptor set minted and cached by
        // VulkanImGuiBackend. Returns 0 when no binding can be produced —
        // callers MUST skip the ImGui::Image draw on 0 (the Vulkan backend
        // crashes binding a null descriptor set; GL merely sampled black).
        [[nodiscard]] static u64 GetFramebufferTextureID(const Framebuffer& framebuffer, u32 attachmentIndex);
        [[nodiscard]] static u64 GetTextureID(const Texture2D& texture);

        void BlockEvents(bool const block)
        {
            m_BlockEvents = block;
        }

        static void SetDarkThemeColors();

      private:
        bool m_BlockEvents = true;
    };
} // namespace OloEngine
