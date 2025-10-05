#include <OloEngine.h>
#include <OloEngine/Core/EntryPoint.h>

#include "EditorLayer.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace OloEngine
{
	class OloEngineEditor : public Application
	{
	public:
		explicit OloEngineEditor(const ApplicationSpecification& spec)
			: Application(spec)
		{
			PushLayer(new EditorLayer());
		}

		~OloEngineEditor() final = default;
	};

	Application* CreateApplication(ApplicationCommandLineArgs const args)
	{
		ApplicationSpecification spec;
		spec.Name = "OloEditor";
		spec.CommandLineArgs = args;

		return new OloEngineEditor(spec);
	}
}
