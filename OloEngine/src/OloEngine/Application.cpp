#include "OloEnginePCH.h"
#include "Application.h"

#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Log.h"

namespace OloEngine {
	Application::Application()
	{
	}

	Application::~Application()
	{
	}

	void Application::Run()
	{
		WindowResizeEvent e(1280, 720);
		{
			OLO_TRACE(e);
		}
		if (e.IsInCategory(EventCategoryApplication))
		{
			OLO_TRACE(e);
		}
		if (e.IsInCategory(EventCategoryInput))
		{
			OLO_TRACE(e);
		}

		while (true);
	}
}