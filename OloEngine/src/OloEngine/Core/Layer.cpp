// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include <utility>

#include "OloEnginePCH.h"
#include "OloEngine/Core/Layer.h"

namespace OloEngine {
	Layer::Layer(std::string  debugName)
		: m_DebugName(std::move(debugName))
	{
	}
}
