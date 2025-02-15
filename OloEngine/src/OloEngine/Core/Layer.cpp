#include <utility>

#include "OloEnginePCH.h"
#include "OloEngine/Core/Layer.h"

namespace OloEngine
{
	Layer::Layer(std::string debugName)
		: m_DebugName(std::move(debugName))
	{
	}
}
