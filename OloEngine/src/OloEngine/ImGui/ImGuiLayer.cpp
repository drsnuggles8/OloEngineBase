#include "OloEnginePCH.h"
#include "OloEngine/ImGui/ImGuiLayer.h"
#include "OloEngine/ImGui/Colors.h"
#include "OloEngine/ImGui/FontAwesome.h"
#include "OloEngine/ImGui/ImGuiFonts.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/Texture.h"
// OLO_WITH_VULKAN-guarded factory-TU convention (see Renderer/Framebuffer.cpp):
// the header self-guards, and the Vulkan renderer backend lives in
// Platform/Vulkan because it needs the device/swapchain/facade.
#include "Platform/Vulkan/VulkanImGuiBackend.h"

#include <imgui.h>
#include <ImGuizmo.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] bool ImGuiOpenGLBackendActive()
        {
            return RendererAPI::GetAPI() == RendererAPI::API::OpenGL;
        }

        // #691 Phase 8: the Vulkan renderer backend (imgui_impl_vulkan via
        // VulkanImGuiBackend) is live once Init succeeded. When it did NOT
        // (no swapchain at attach — e.g. minimised at startup — or a non-GL,
        // non-Vulkan future backend), the layer falls back to Phase 7's
        // PLATFORM-ONLY mode: the context, the GLFW input backend and every
        // panel's ImGui code work exactly as on GL, and only the draw-data
        // submission is absent. That is what lets the editor layer — and its
        // ImGui-touching update-path code (DrainMcpInputQueue's io.AddKeyEvent,
        // the viewport's ImGui::GetMousePos) — run at all on this backend, so
        // the render graph has a scene to draw. A context-less "just skip
        // ImGui" would crash in those calls instead (GImGui is asserted).
        [[nodiscard]] bool ImGuiVulkanBackendActive()
        {
#if OLO_WITH_VULKAN
            return RendererAPI::GetAPI() == RendererAPI::API::Vulkan && VulkanImGuiBackend::IsInitialized();
#else
            return false;
#endif
        }
    } // namespace

    ImGuiLayer::ImGuiLayer()
        : Layer("ImGuiLayer")
    {
    }

    void ImGuiLayer::OnAttach()
    {
        OLO_PROFILE_FUNCTION();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
        // io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Enable Docking
        // Multi-viewport stays GL-only for now: the Phase 8 Vulkan renderer
        // backend draws the MAIN window's UI, but its secondary-viewport path
        // (imgui_impl_vulkan-owned swapchains + imgui_impl_glfw's
        // Platform_CreateVkSurface, which our GLFW_INCLUDE_NONE build compiles
        // out — GLFW_HAS_VULKAN is 0 in ImGuiBuild.cpp) is untested here.
        // Follow-up work; an undocked panel would otherwise be an empty
        // OS window.
        if (ImGuiOpenGLBackendActive())
            io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable Multi-Viewport / Platform Windows
        // io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoTaskBarIcons;
        // io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoMerge;

        const f32 dpiScale = Window::s_HighDPIScaleFactor;

        // Configure Fonts
        {
            UI::FontConfiguration robotoBold;
            robotoBold.FontName = "Bold";
            robotoBold.FilePath = "assets/fonts/roboto/Roboto-Bold.ttf";
            robotoBold.Size = dpiScale * 18.0f;
            UI::Fonts::Add(robotoBold);

            UI::FontConfiguration robotoLarge;
            robotoLarge.FontName = "Large";
            robotoLarge.FilePath = "assets/fonts/roboto/Roboto-Regular.ttf";
            robotoLarge.Size = dpiScale * 24.0f;
            UI::Fonts::Add(robotoLarge);

            UI::FontConfiguration robotoDefault;
            robotoDefault.FontName = "Default";
            robotoDefault.FilePath = "assets/fonts/roboto/Roboto-SemiMedium.ttf";
            robotoDefault.Size = dpiScale * 15.0f;
            UI::Fonts::Add(robotoDefault, true);

            static const ImWchar s_FontAwesomeRanges[] = { OLO_ICON_MIN, OLO_ICON_MAX, 0 };
            UI::FontConfiguration fontAwesome;
            fontAwesome.FontName = "FontAwesome";
            fontAwesome.FilePath = "assets/fonts/fontawesome/fontawesome-webfont.ttf";
            fontAwesome.Size = dpiScale * 16.0f;
            fontAwesome.GlyphRanges = s_FontAwesomeRanges;
            fontAwesome.MergeWithLast = true;
            UI::Fonts::Add(fontAwesome);

            UI::FontConfiguration robotoMedium;
            robotoMedium.FontName = "Medium";
            robotoMedium.FilePath = "assets/fonts/roboto/Roboto-SemiMedium.ttf";
            robotoMedium.Size = dpiScale * 18.0f;
            UI::Fonts::Add(robotoMedium);

            UI::FontConfiguration robotoSmall;
            robotoSmall.FontName = "Small";
            robotoSmall.FilePath = "assets/fonts/roboto/Roboto-SemiMedium.ttf";
            robotoSmall.Size = dpiScale * 12.0f;
            UI::Fonts::Add(robotoSmall);

            UI::FontConfiguration robotoExtraSmall;
            robotoExtraSmall.FontName = "ExtraSmall";
            robotoExtraSmall.FilePath = "assets/fonts/roboto/Roboto-SemiMedium.ttf";
            robotoExtraSmall.Size = dpiScale * 10.0f;
            UI::Fonts::Add(robotoExtraSmall);

            UI::FontConfiguration robotoBoldTitle;
            robotoBoldTitle.FontName = "BoldTitle";
            robotoBoldTitle.FilePath = "assets/fonts/roboto/Roboto-Bold.ttf";
            robotoBoldTitle.Size = dpiScale * 16.0f;
            UI::Fonts::Add(robotoBoldTitle);
        }

        // Setup Dear ImGui style
        ImGui::StyleColorsDark();
        // ImGui::StyleColorsClassic();

        // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(dpiScale);
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            style.WindowRounding = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }

        SetDarkThemeColors();

        Application& app = Application::Get();
        auto* const window = static_cast<GLFWwindow*>(app.GetWindow().GetNativeWindow());

        // Setup Platform/Renderer bindings
        if (ImGuiOpenGLBackendActive())
        {
            ::ImGui_ImplGlfw_InitForOpenGL(window, true);
            ::ImGui_ImplOpenGL3_Init("#version 430");
        }
        else
        {
#if OLO_WITH_VULKAN
            if (RendererAPI::GetAPI() == RendererAPI::API::Vulkan)
            {
                // #691 Phase 8: the real Vulkan renderer backend. InitForVulkan
                // installs the same input callbacks as InitForOpenGL (the
                // ClientApi tag only matters to multi-viewport, which stays
                // off here); VulkanImGuiBackend wires imgui_impl_vulkan to the
                // live device/swapchain, and VulkanContext::SwapBuffers
                // records the draw data into the frame command buffer.
                ::ImGui_ImplGlfw_InitForVulkan(window, true);
                if (VulkanImGuiBackend::Init())
                {
                    return;
                }
                // Init can only fail without a swapchain (minimised at
                // startup) — degrade to Phase 7 platform-only below rather
                // than crash panel logic.
                OLO_CORE_WARN("[ImGui] Vulkan renderer backend unavailable — falling back to platform-only mode");
            }
            else
#endif
            {
                // InitForOther installs the same input callbacks but leaves
                // Platform_RenderWindow/SwapBuffers unset, so no path reaches a
                // GL call.
                ::ImGui_ImplGlfw_InitForOther(window, true);
            }
            // ImGui 1.92's texture protocol: a renderer backend either sets
            // this flag and services io.Textures, or it must build the legacy
            // font atlas itself. With NEITHER, ImGui::Render() asserts
            // ("font atlas is not built") on the first frame — which is
            // exactly what a platform-only layer would hit. Claiming the flag
            // is honest here: there IS no renderer to feed, the draw data is
            // discarded, and no texture request ever needs servicing.
            ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            OLO_CORE_INFO("[ImGui] Platform-only mode: no renderer backend for this RHI — "
                          "panel logic runs, nothing is drawn");
        }
    }

    void ImGuiLayer::OnDetach()
    {
        OLO_PROFILE_FUNCTION();

        if (ImGuiOpenGLBackendActive())
            ::ImGui_ImplOpenGL3_Shutdown();
#if OLO_WITH_VULKAN
        // Before ImGui_ImplGlfw_Shutdown and — crucially — while the layer
        // stack is being cleared, which happens BEFORE the Window (and with
        // it the VulkanContext/device) is destroyed: the backend must free
        // its pipelines/buffers/descriptors against a live device.
        VulkanImGuiBackend::Shutdown();
#endif
        ::ImGui_ImplGlfw_Shutdown();
        UI::Fonts::ClearFonts();
        ImGui::DestroyContext();
    }

    void ImGuiLayer::OnEvent(Event& e)
    {
        if (m_BlockEvents)
        {
            const ImGuiIO& io = ImGui::GetIO();
            e.Handled |= e.IsInCategory(EventCategory::Mouse) & io.WantCaptureMouse;
            e.Handled |= e.IsInCategory(EventCategory::Keyboard) & io.WantCaptureKeyboard;
        }
    }

    void ImGuiLayer::Begin()
    {
        OLO_PROFILE_FUNCTION();

        if (ImGuiOpenGLBackendActive())
            ::ImGui_ImplOpenGL3_NewFrame();
#if OLO_WITH_VULKAN
        VulkanImGuiBackend::NewFrame(); // no-op unless the Vulkan backend initialised
#endif
        ::ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
    }

    void ImGuiLayer::End()
    {
        OLO_PROFILE_FUNCTION();

        ImGuiIO& io = ImGui::GetIO();

        Window const& window = Application::Get().GetWindow();
        io.DisplaySize = ImVec2(static_cast<f32>(window.GetWidth()), static_cast<f32>(window.GetHeight()));

        // Rendering
        ImGui::Render();
        if (ImGuiOpenGLBackendActive())
            ::ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#if OLO_WITH_VULKAN
        // On Vulkan the draw data is NOT recorded here: End() runs inside the
        // frame callback (Application::RenderFrameLayers, driven from
        // VulkanContext::SwapBuffers), and the UI must land in the frame's
        // command buffer AFTER the 3D content but BEFORE the present
        // transition. Marking the data fresh hands it to
        // VulkanImGuiBackend::RecordOverlay, which SwapBuffers invokes at
        // exactly that point.
        if (ImGuiVulkanBackendActive())
            VulkanImGuiBackend::NotifyDrawDataReady();
#endif

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            GLFWwindow* const backup_current_context = ::glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            ::glfwMakeContextCurrent(backup_current_context);
        }
    }

    u64 ImGuiLayer::GetFramebufferTextureID(const Framebuffer& framebuffer, const u32 attachmentIndex)
    {
#if OLO_WITH_VULKAN
        if (RendererAPI::GetAPI() == RendererAPI::API::Vulkan)
            return VulkanImGuiBackend::GetTextureID(framebuffer.GetColorAttachmentHandle(attachmentIndex));
#endif
        return framebuffer.GetColorAttachmentRendererID(attachmentIndex);
    }

    u64 ImGuiLayer::GetTextureID(const Texture2D& texture)
    {
#if OLO_WITH_VULKAN
        if (RendererAPI::GetAPI() == RendererAPI::API::Vulkan)
            return VulkanImGuiBackend::GetTextureID(texture.GetRHIHandle());
#endif
        return texture.GetRendererID();
    }

    bool ImGuiLayer::RenderTargetRowsAreBottomUp()
    {
        // The uv-facing spelling of RHI::RenderTargetRowsAreBottomUp — one
        // owner for the row-order convention (ADR 0011 amendment (85)), not a
        // second copy of the test.
        return RHI::RenderTargetRowsAreBottomUp();
    }

    void ImGuiLayer::SetDarkThemeColors()
    {
        auto& style = ImGui::GetStyle();
        auto& colors = style.Colors;

        //========================================================
        /// Colors

        // Headers
        colors[ImGuiCol_Header] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::groupHeader);
        colors[ImGuiCol_HeaderHovered] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::groupHeader);
        colors[ImGuiCol_HeaderActive] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::groupHeader);

        // Buttons
        colors[ImGuiCol_Button] = ImColor(56, 56, 56, 200);
        colors[ImGuiCol_ButtonHovered] = ImColor(70, 70, 70, 255);
        colors[ImGuiCol_ButtonActive] = ImColor(56, 56, 56, 150);

        // Frame BG
        colors[ImGuiCol_FrameBg] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::propertyField);
        colors[ImGuiCol_FrameBgHovered] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::propertyField);
        colors[ImGuiCol_FrameBgActive] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::propertyField);

        // Tabs
        colors[ImGuiCol_Tab] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::titlebar);
        colors[ImGuiCol_TabHovered] = ImColor(255, 225, 135, 30);
        colors[ImGuiCol_TabActive] = ImColor(255, 225, 135, 60);
        colors[ImGuiCol_TabUnfocused] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::titlebar);
        colors[ImGuiCol_TabUnfocusedActive] = colors[ImGuiCol_TabHovered];

        // Title
        colors[ImGuiCol_TitleBg] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::titlebar);
        colors[ImGuiCol_TitleBgActive] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::titlebar);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };

        // Resize Grip
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.91f, 0.91f, 0.91f, 0.25f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.81f, 0.81f, 0.81f, 0.67f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.46f, 0.46f, 0.46f, 0.95f);

        // Scrollbar
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.0f);

        // Check Mark
        colors[ImGuiCol_CheckMark] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::text);

        // Slider
        colors[ImGuiCol_SliderGrab] = ImVec4(0.51f, 0.51f, 0.51f, 0.7f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.66f, 0.66f, 0.66f, 1.0f);

        // Text
        colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::text);

        // Separator
        colors[ImGuiCol_Separator] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::backgroundDark);
        colors[ImGuiCol_SeparatorActive] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::highlight);
        colors[ImGuiCol_SeparatorHovered] = ImColor(39, 185, 242, 150);

        // Window Background
        colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::titlebar);
        colors[ImGuiCol_ChildBg] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::background);
        colors[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::backgroundPopup);
        colors[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::backgroundDark);

        // Tables
        colors[ImGuiCol_TableHeaderBg] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::groupHeader);
        colors[ImGuiCol_TableBorderLight] = ImGui::ColorConvertU32ToFloat4(Colors::Theme::backgroundDark);

        // Menubar
        colors[ImGuiCol_MenuBarBg] = ImVec4{ 0.0f, 0.0f, 0.0f, 0.0f };

        //========================================================
        /// Style
        style.FrameRounding = 2.5f;
        style.FrameBorderSize = 1.0f;
        style.IndentSpacing = 11.0f;
    }
} // namespace OloEngine
