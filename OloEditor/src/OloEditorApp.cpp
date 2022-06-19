// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include <OloEngine.h>
#include <OloEngine/Core/EntryPoint.h>

#include "EditorLayer.h"

namespace OloEngine {

	class OloEngineEditor : public Application
	{
	public:
		OloEngineEditor(ApplicationCommandLineArgs args)
			: Application("OloEngineEditor", args)
		{
			PushLayer(new EditorLayer());
		}

		~OloEngineEditor() = default;
	};

	Application* CreateApplication(ApplicationCommandLineArgs args)
	{
		return new OloEngineEditor(args);
	}

}
