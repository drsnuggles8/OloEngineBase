// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include <ranges>
#include "OloEnginePCH.h"
#include "OloEngine/Core/Application.h"

#include "OloEngine/Core/Log.h"

#include "OloEngine/Renderer/Renderer.h"

#include "OloEngine/Core/Input.h"

#include <GLFW/glfw3.h>

namespace OloEngine {

	Application* Application::s_Instance = nullptr;

	Application::Application(const std::string& name, ApplicationCommandLineArgs const args)
		: m_CommandLineArgs(args)
	{
		OLO_PROFILE_FUNCTION();

		OLO_CORE_ASSERT(!s_Instance, "Application already exists!")
		s_Instance = this;
		m_Window = Window::Create(WindowProps(name));
		m_Window->SetEventCallback(OLO_BIND_EVENT_FN(Application::OnEvent));

		Renderer::Init();

		m_ImGuiLayer = new ImGuiLayer();
		PushOverlay(m_ImGuiLayer);
	}

	Application::~Application()
	{
		OLO_PROFILE_FUNCTION();

		for (Layer* const layer : m_LayerStack)
		{
			layer->OnDetach();
			delete layer;
		}

		Renderer::Shutdown();
	}

	void Application::PushLayer(Layer* const layer)
	{
		OLO_PROFILE_FUNCTION();

		m_LayerStack.PushLayer(layer);
		layer->OnAttach();
	}

	void Application::PushOverlay(Layer* const layer)
	{
		OLO_PROFILE_FUNCTION();

		m_LayerStack.PushOverlay(layer);
		layer->OnAttach();
	}

	void Application::PopLayer(Layer* const layer)
	{
		m_LayerStack.PopLayer(layer);
		layer->OnDetach();
	}

	void Application::PopOverlay(Layer* const layer)
	{
		m_LayerStack.PopOverlay(layer);
		layer->OnDetach();
	}


	void Application::Close()
	{
		m_Running = false;
	}

	void Application::OnEvent(Event& e)
	{
		OLO_PROFILE_FUNCTION();

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowCloseEvent>(OLO_BIND_EVENT_FN(Application::OnWindowClose));
		dispatcher.Dispatch<WindowResizeEvent>(OLO_BIND_EVENT_FN(Application::OnWindowResize));

		for (auto& it : std::ranges::reverse_view(m_LayerStack))
		{
			if (e.Handled)
			{
				break;
			}
			it->OnEvent(e);
		}
	}

	void Application::Run()
	{
		OLO_PROFILE_FUNCTION();

		while (m_Running)
		{
			OLO_PROFILE_SCOPE("RunLoop");

			const auto timeNow = static_cast<float>(::glfwGetTime());
			const Timestep timestep = timeNow - m_LastFrameTime;
			m_LastFrameTime = timeNow;

			if (!m_Minimized)
			{
				{
					OLO_PROFILE_SCOPE("LayerStack OnUpdate");
					for (Layer* const layer : m_LayerStack)
					{
						layer->OnUpdate(timestep);
					}
				}

				OloEngine::ImGuiLayer::Begin();
				{
					OLO_PROFILE_SCOPE("LayerStack OnImGuiRender");

					for (Layer* const layer : m_LayerStack)
					{
						layer->OnImGuiRender();
					}
				}
				OloEngine::ImGuiLayer::End();
			}

			m_Window->OnUpdate();
		}
	}

	bool Application::OnWindowClose([[maybe_unused]] WindowCloseEvent const& e)
	{
		m_Running = false;
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent const& e)
	{
		OLO_PROFILE_FUNCTION();

		if ((0 == e.GetWidth()) || (0 == e.GetHeight()))
		{
			m_Minimized = true;
			return false;
		}

		m_Minimized = false;
		Renderer::OnWindowResize(e.GetWidth(), e.GetHeight());

		return false;
	}

}
