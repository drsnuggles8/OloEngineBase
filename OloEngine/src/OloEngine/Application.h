#pragma once

#include "Core.h"

namespace OloEngine {

	class OLO_API Application
	{
	public:
		Application();
		virtual ~Application();

		void Run();
	};

	// To be defined in CLIENT
	Application* CreateApplication();
}
