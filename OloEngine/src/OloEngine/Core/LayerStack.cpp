#include "OloEnginePCH.h"
#include "OloEngine/Core/LayerStack.h"

namespace OloEngine
{
	void LayerStack::PushLayer(Layer* layer)
	{
		m_Layers.emplace(m_Layers.begin() + m_LayerInsertIndex, layer);
		++m_LayerInsertIndex;
	}

	void LayerStack::PushOverlay(Layer* overlay)
	{
		m_Layers.emplace_back(overlay);
	}

	void LayerStack::PopLayer(Layer* const layer)
	{
		const std::input_iterator auto it = std::ranges::find(m_Layers.begin(), m_Layers.end(), layer);
		if (it != m_Layers.end())
		{
			m_Layers.erase(it);
			--m_LayerInsertIndex;
		}
	}

	void LayerStack::PopOverlay(Layer* const overlay)
	{
		const std::input_iterator auto it = std::ranges::find(m_Layers.begin(), m_Layers.end(), overlay);
		if (it != m_Layers.end())
		{
			m_Layers.erase(it);
		}
	}
}
