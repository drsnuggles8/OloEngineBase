#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Window.h"
#include "OloEngine/Core/LayerStack.h"
#include "OloEngine/Events/Event.h"
#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/ImGui/ImGuiLayer.h"

int main(int argc, char** argv);

namespace OloEngine {

	struct ApplicationCommandLineArgs
	{
		int Count = 0;
		char** Args = nullptr;

		const char* operator[](const int index) const
		{
			OLO_CORE_ASSERT(index < Count)
			return Args[index];
		}
	};

	struct ApplicationSpecification
	{
		std::string Name = "OloEngine Application";
		std::string WorkingDirectory;
		ApplicationCommandLineArgs CommandLineArgs;
	};

	class Application
	{
	public:
		explicit Application(const ApplicationSpecification& specification);
		virtual ~Application();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* layer);
		void PopLayer(Layer* layer);
		void PopOverlay(Layer* layer);

		[[nodiscard("Returns *m_Window, you probably wanted some other function!")]] Window& GetWindow() { return *m_Window; }

		[[nodiscard("Returns *s_Instance, you probably wanted some other function!")]] static Application& Get() { return *s_Instance; }

		void Close();

		[[nodiscard("Returns m_ImGuiLayer, you probably wanted some other function!")]] ImGuiLayer* GetImGuiLayer() { return m_ImGuiLayer; }

		[[nodiscard("Returns m_Specification, you probably wanted some other function!")]] const ApplicationSpecification& GetSpecification() const { return m_Specification; }
	private:
		void Run();
		bool OnWindowClose(WindowCloseEvent const& e);
		bool OnWindowResize(WindowResizeEvent const& e);
	private:
		ApplicationSpecification m_Specification;
		Scope<Window> m_Window;
		ImGuiLayer* m_ImGuiLayer;
		bool m_Running = true;
		bool m_Minimized = false;
		LayerStack m_LayerStack;
		float m_LastFrameTime = 0.0f;
	private:
		static Application* s_Instance;
		friend int ::main(int argc, char** argv);
	};

	// To be defined in CLIENT
	Application* CreateApplication(ApplicationCommandLineArgs args);

}
