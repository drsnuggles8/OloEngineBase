#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine {

	Scope<RendererAPI> RenderCommand::s_RendererAPI = RendererAPI::Create();

}