// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include <OloEngine.h>
#include <OloEngine/Core/EntryPoint.h>

#include "EditorLayer.h"

namespace OloEngine {

	class OloEngineEditor : public Application
	{
	public:
		OloEngineEditor()
			: Application("OloEngine Editor")
		{
			PushLayer(new EditorLayer());
		}

		~OloEngineEditor()
		{
		}
	};

	Application* CreateApplication()
	{
		return new OloEngineEditor();
	}

}