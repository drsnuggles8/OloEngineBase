// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "OloEngine/Core/Layer.h"

namespace OloEngine {
	Layer::Layer(const std::string& debugName)
		: m_DebugName(debugName)
	{
	}
}
