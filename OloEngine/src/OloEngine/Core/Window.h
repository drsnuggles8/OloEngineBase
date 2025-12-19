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
        u32 Width;
        u32 Height;

        explicit WindowProps(std::string title = "OloEngine", const u32 width = 1280, const u32 height = 720)
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

        [[nodiscard("Store this!")]] virtual u32 GetWidth() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetHeight() const = 0;

        // Add new methods for framebuffer size
        virtual u32 GetFramebufferWidth() const = 0;
        virtual u32 GetFramebufferHeight() const = 0;

        // Window attributes
        virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
        virtual void SetVSync(bool enabled) = 0;
        [[nodiscard("Store this!")]] virtual bool IsVSync() const = 0;

        [[nodiscard("Store this!")]] virtual void* GetNativeWindow() const = 0;

        virtual void SetTitle(const std::string& title) = 0;

        static Scope<Window> Create(const WindowProps& props = WindowProps());

      public:
        static f32 s_HighDPIScaleFactor;
    };
} // namespace OloEngine
