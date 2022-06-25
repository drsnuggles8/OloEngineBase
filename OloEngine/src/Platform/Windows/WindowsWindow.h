#pragma once

#include "OloEngine/Core/Window.h"
#include "OloEngine/Renderer/GraphicsContext.h"

#include <GLFW/glfw3.h>

namespace OloEngine {
	class WindowsWindow : public Window
	{
	public:
		explicit WindowsWindow(const WindowProps& props);
		~WindowsWindow() override;

		void OnUpdate() override;

		[[nodiscard("This returns m_Data.Width, you probably wanted another function!")]] unsigned int GetWidth() const noexcept override { return m_Data.Width; }
		[[nodiscard("This returns m_Data.Height, you probably wanted another function!")]] unsigned int GetHeight() const noexcept override { return m_Data.Height; }

		// Window attributes
		void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
		void SetVSync(bool enabled) override;
		[[nodiscard("This returns m_Data.VSync, you probably wanted another function!")]] bool IsVSync() const override;

		[[nodiscard("This returns m_Window, you probably wanted another function!")]] void* GetNativeWindow() const noexcept override { return m_Window; }

		void SetTitle(const std::string& title) override;
	private:
		virtual void Init(const WindowProps& props);
		virtual void Shutdown();
	private:
		GLFWwindow* m_Window{};
		Scope<GraphicsContext> m_Context;

		struct WindowData
		{
			std::string Title;
			unsigned int Width{};
			unsigned int Height{};
			bool VSync{};

			EventCallbackFn EventCallback;
		};

		WindowData m_Data;
	};

}
