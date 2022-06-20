// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include <OloEngine.h>
#include <OloEngine/Core/EntryPoint.h>

#include "EditorLayer.h"

namespace OloEngine {

	class OloEngineEditor : public Application
	{
	public:
		explicit OloEngineEditor(const ApplicationCommandLineArgs args)
			: Application("OloEngineEditor", args)
		{
			PushLayer(new EditorLayer());
		}

		~OloEngineEditor() final = default;
	};

	Application* CreateApplication(ApplicationCommandLineArgs const args)
	{
		return new OloEngineEditor(args);
	}

}
