#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/vec4.hpp>

#include <span>
#include <string>
#include <string_view>

namespace OloEngine
{
    class VertexArray;

    // Minimal graph-visible command context used to scope pass execution.
    // This is intentionally backend-agnostic and does not expose GL/VK/DX
    // types. Future phases can extend it with encoder operations.
    class RGCommandContext
    {
      public:
        void BeginPass(std::string_view passName)
        {
            m_ActivePassName = passName;
            m_IsPassActive = true;
        }

        void EndPass()
        {
            m_ActivePassName.clear();
            m_IsPassActive = false;
        }

        [[nodiscard]] bool IsPassActive() const
        {
            return m_IsPassActive;
        }

        [[nodiscard]] std::string_view GetActivePassName() const
        {
            return m_ActivePassName;
        }

        void SetViewport(u32 x, u32 y, u32 width, u32 height);
        void SetClearColor(const glm::vec4& color);
        void Clear();
        void ResetGraphicsStateToDefault();
        void BindDefaultFramebuffer();
        void SetDepthTest(bool enabled);
        void SetDepthMask(bool enabled);
        void SetBlendState(bool enabled);
        void SetAlphaBlendStandard();
        void SetOpaqueReplaceBlend();
        void SetCulling(bool enabled);
        void SetDrawBuffers(std::span<const u32> attachments);
        void BindTexture(u32 slot, u32 textureID);
        void DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount = 0);

      private:
        std::string m_ActivePassName;
        bool m_IsPassActive = false;
    };
} // namespace OloEngine
