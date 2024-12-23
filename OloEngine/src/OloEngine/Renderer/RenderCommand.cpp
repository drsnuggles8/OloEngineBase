// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
	Scope<RendererAPI> RenderCommand::s_RendererAPI = RendererAPI::Create();
}
