#include "OloEnginePCH.h"
#include "OloEngine/Core/LayerStack.h"

#include <ranges>

namespace OloEngine
{
    LayerStack::~LayerStack()
    {
        Clear();
    }

    void LayerStack::Clear()
    {
        for (auto it = m_Layers.rbegin(); it != m_Layers.rend(); ++it)
        {
            (*it)->OnDetach();
        }
        m_Layers.clear();
        m_RawPtrs.clear();
        m_LayerInsertIndex = 0;
    }

    LayerStack::LayerStack(LayerStack&& other) noexcept
        : m_Layers(std::move(other.m_Layers)), m_RawPtrs(std::move(other.m_RawPtrs)), m_LayerInsertIndex(other.m_LayerInsertIndex)
    {
        other.m_LayerInsertIndex = 0;
    }

    LayerStack& LayerStack::operator=(LayerStack&& other) noexcept
    {
        if (this != &other)
        {
            Clear();
            m_Layers = std::move(other.m_Layers);
            m_RawPtrs = std::move(other.m_RawPtrs);
            m_LayerInsertIndex = other.m_LayerInsertIndex;
            other.m_LayerInsertIndex = 0;
        }
        return *this;
    }

    void LayerStack::PushLayer(std::unique_ptr<Layer> layer)
    {
        auto* const raw = layer.get();
        m_Layers.emplace(m_Layers.begin() + m_LayerInsertIndex, std::move(layer));
        ++m_LayerInsertIndex;
        RebuildRawPtrs();
        raw->OnAttach();
    }

    void LayerStack::PushOverlay(std::unique_ptr<Layer> overlay)
    {
        auto* const raw = overlay.get();
        m_Layers.emplace_back(std::move(overlay));
        RebuildRawPtrs();
        raw->OnAttach();
    }

    std::unique_ptr<Layer> LayerStack::PopLayer(Layer* const layer)
    {
        auto const layerEnd = m_Layers.begin() + m_LayerInsertIndex;
        const auto it = std::ranges::find_if(m_Layers.begin(), layerEnd,
                                             [layer](const std::unique_ptr<Layer>& ptr)
                                             { return ptr.get() == layer; });
        if (it != layerEnd)
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
        auto const overlayBegin = m_Layers.begin() + m_LayerInsertIndex;
        const auto it = std::ranges::find_if(overlayBegin, m_Layers.end(),
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
