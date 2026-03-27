#include "OloEnginePCH.h"
#include "OloEngine/ImGui/ImGuiLayer.h"
#include "OloEngine/ImGui/Colors.h"
#include "OloEngine/ImGui/FontAwesome.h"
#include "OloEngine/ImGui/ImGuiFonts.h"
#include "OloEngine/Core/Application.h"

#include <imgui.h>
#include <ImGuizmo.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

namespace OloEngine
{
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
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // Enable Docking
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
        ::ImGui_ImplGlfw_InitForOpenGL(window, true);
        ::ImGui_ImplOpenGL3_Init("#version 430");
    }

    void ImGuiLayer::OnDetach()
    {
        OLO_PROFILE_FUNCTION();

        ::ImGui_ImplOpenGL3_Shutdown();
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

        ::ImGui_ImplOpenGL3_NewFrame();
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
        ::ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            GLFWwindow* const backup_current_context = ::glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            ::glfwMakeContextCurrent(backup_current_context);
        }
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
