#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Layer.h"

#include <memory>
#include <vector>

namespace OloEngine
{
    class LayerStack
    {
      public:
        LayerStack() = default;
        ~LayerStack();

        LayerStack(LayerStack&& other) noexcept;
        LayerStack& operator=(LayerStack&& other) noexcept;
        LayerStack(const LayerStack&) = delete;
        LayerStack& operator=(const LayerStack&) = delete;

        // Calls OnDetach() on every layer and empties the stack.
        void Clear();

        // Takes ownership. Calls OnAttach() on the layer.
        void PushLayer(std::unique_ptr<Layer> layer);
        // Takes ownership. Calls OnAttach() on the overlay.
        void PushOverlay(std::unique_ptr<Layer> overlay);
        // Calls OnDetach(), removes from stack, returns ownership to caller.
        std::unique_ptr<Layer> PopLayer(Layer* layer);
        // Calls OnDetach(), removes from stack, returns ownership to caller.
        std::unique_ptr<Layer> PopOverlay(Layer* overlay);

        // Forward iteration yielding Layer* (for OnUpdate, OnImGuiRender)
        [[nodiscard]] auto begin()
        {
            return m_RawPtrs.begin();
        }
        [[nodiscard]] auto end()
        {
            return m_RawPtrs.end();
        }
        [[nodiscard]] auto begin() const
        {
            return m_RawPtrs.cbegin();
        }
        [[nodiscard]] auto end() const
        {
            return m_RawPtrs.cend();
        }

        // Reverse iteration (for event dispatch — overlays first)
        [[nodiscard]] auto rbegin()
        {
            return m_RawPtrs.rbegin();
        }
        [[nodiscard]] auto rend()
        {
            return m_RawPtrs.rend();
        }
        [[nodiscard]] auto rbegin() const
        {
            return m_RawPtrs.crbegin();
        }
        [[nodiscard]] auto rend() const
        {
            return m_RawPtrs.crend();
        }

      private:
        void RebuildRawPtrs();

        std::vector<std::unique_ptr<Layer>> m_Layers;
        // Cached raw-pointer mirror for fast iteration and ranges compatibility.
        // Rebuilt on push/pop (rare) so per-frame iteration is zero-overhead.
        std::vector<Layer*> m_RawPtrs;
        u32 m_LayerInsertIndex = 0;
    };
} // namespace OloEngine
