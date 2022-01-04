#include "OloEnginePCH.h"
#include "RenderCommand.h"

#include "Platform/OpenGL/OpenGLRendererAPI.h"

namespace OloEngine {

	RendererAPI* RenderCommand::s_RendererAPI = new OpenGLRendererAPI;

}