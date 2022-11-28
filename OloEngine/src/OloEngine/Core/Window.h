#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Events/Event.h"

#include <sstream>
#include <utility>

namespace OloEngine
{
	struct WindowProps
	{
		std::string Title;
		uint32_t Width;
		uint32_t Height;

		explicit WindowProps(std::string  title = "OloEngine",
			const uint32_t width = 1600,
			const uint32_t height = 900)
			: Title(std::move(title)), Width(width), Height(height)
		{
		}
	};

	// Interface representing a desktop system based Window
	class Window
	{
	public:
		using EventCallbackFn = std::function<void(Event&)>;

		virtual ~Window() = default;

		virtual void OnUpdate() = 0;

		[[nodiscard("Store this!")]] virtual uint32_t GetWidth() const = 0;
		[[nodiscard("Store this!")]] virtual uint32_t GetHeight() const = 0;

		// Window attributes
		virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
		virtual void SetVSync(bool enabled) = 0;
		[[nodiscard("Store this!")]] virtual bool IsVSync() const = 0;

		[[nodiscard("Store this!")]] virtual void* GetNativeWindow() const = 0;

		virtual void SetTitle(const std::string& title) = 0;

		static Scope<Window> Create(const WindowProps& props = WindowProps());
	public:
		static float s_HighDPIScaleFactor;
	};
}
