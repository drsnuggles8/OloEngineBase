// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <ranges>
#include <utility>

namespace OloEngine {

	Application* Application::s_Instance = nullptr;

	Application::Application(ApplicationSpecification specification)
		: m_Specification(std::move(specification))
	{
		OLO_PROFILE_FUNCTION();

		OLO_CORE_ASSERT(!s_Instance, "Application already exists!");
		s_Instance = this;
		// Set working directory here
		if (!m_Specification.WorkingDirectory.empty())
		{
			std::filesystem::current_path(m_Specification.WorkingDirectory);
		}

		m_Window = Window::Create(WindowProps(m_Specification.Name));
		m_Window->SetEventCallback(OLO_BIND_EVENT_FN(Application::OnEvent));

		Renderer::Init();
		ScriptEngine::Init();
		LuaScriptEngine::Init();

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

		LuaScriptEngine::Shutdown();
		ScriptEngine::Shutdown();
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

	void Application::SubmitToMainThread(const std::function<void()>& function)
	{
		std::scoped_lock<std::mutex> lock(m_MainThreadQueueMutex);

		m_MainThreadQueue.emplace_back(function);
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

			const auto timeNow = Time::GetTime();
			const Timestep timestep = timeNow - m_LastFrameTime;
			m_LastFrameTime = timeNow;

			ExecuteMainThreadQueue();

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

	void Application::ExecuteMainThreadQueue()
	{
		std::scoped_lock<std::mutex> lock(m_MainThreadQueueMutex);

		for (auto const& func : m_MainThreadQueue)
		{
			func();
		}
		m_MainThreadQueue.clear();
	}

}
