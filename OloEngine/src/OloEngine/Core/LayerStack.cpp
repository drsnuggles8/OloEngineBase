#include "OloEnginePCH.h"
#include "OloEngine/Core/LayerStack.h"

namespace OloEngine
{
    LayerStack::~LayerStack()
    {
        for (auto& layer : m_Layers)
        {
            layer->OnDetach();
        }
        // unique_ptrs destroy automatically
    }

    void LayerStack::PushLayer(std::unique_ptr<Layer> layer)
    {
        layer->OnAttach();
        m_Layers.emplace(m_Layers.begin() + m_LayerInsertIndex, std::move(layer));
        ++m_LayerInsertIndex;
        RebuildRawPtrs();
    }

    void LayerStack::PushOverlay(std::unique_ptr<Layer> overlay)
    {
        overlay->OnAttach();
        m_Layers.emplace_back(std::move(overlay));
        RebuildRawPtrs();
    }

    std::unique_ptr<Layer> LayerStack::PopLayer(Layer* const layer)
    {
        const auto it = std::ranges::find_if(m_Layers,
                                             [layer](const std::unique_ptr<Layer>& ptr)
                                             { return ptr.get() == layer; });
        if (it != m_Layers.end())
        {
            (*it)->OnDetach();
            auto owned = std::move(*it);
            m_Layers.erase(it);
            --m_LayerInsertIndex;
            RebuildRawPtrs();
            return owned;
        }
        return nullptr;
    }

    std::unique_ptr<Layer> LayerStack::PopOverlay(Layer* const overlay)
    {
        const auto it = std::ranges::find_if(m_Layers,
                                             [overlay](const std::unique_ptr<Layer>& ptr)
                                             { return ptr.get() == overlay; });
        if (it != m_Layers.end())
        {
            (*it)->OnDetach();
            auto owned = std::move(*it);
            m_Layers.erase(it);
            RebuildRawPtrs();
            return owned;
        }
        return nullptr;
    }

    void LayerStack::RebuildRawPtrs()
    {
        m_RawPtrs.clear();
        m_RawPtrs.reserve(m_Layers.size());
        for (const auto& layer : m_Layers)
        {
            m_RawPtrs.push_back(layer.get());
        }
    }
} // namespace OloEngine
