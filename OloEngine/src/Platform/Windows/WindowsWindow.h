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

		[[nodiscard]] unsigned int GetWidth() const noexcept override { return m_Data.Width; }
		[[nodiscard]] unsigned int GetHeight() const noexcept override { return m_Data.Height; }

		// Window attributes
		void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
		void SetVSync(bool enabled) override;
		[[nodiscard]] bool IsVSync() const override;

		[[nodiscard]] void* GetNativeWindow() const noexcept override { return m_Window; }
	private:
		virtual void Init(const WindowProps& props);
		virtual void Shutdown();
	private:
		GLFWwindow* m_Window{};
		Scope<GraphicsContext> m_Context;

		struct WindowData
		{
			std::string Title;
			unsigned int Width{}, Height{};
			bool VSync{};

			EventCallbackFn EventCallback;
		};

		WindowData m_Data;
	};

}
