#include "OloEnginePCH.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Core/Application.h"

#include <commdlg.h>
#include <shellapi.h>
#include <filesystem>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

namespace OloEngine
{
    f32 Time::GetTime()
    {
        if (HasMockTime())
            return GetMockTime();
        return static_cast<f32>(GLFWAPI::glfwGetTime());
    }

    std::string FileDialogs::OpenFile(const char* const filter, const char* const initialDir)
    {
        OPENFILENAMEA ofn;
        CHAR szFile[260] = { 0 };
        CHAR currentDir[256] = { 0 };
        ZeroMemory(&ofn, sizeof(OPENFILENAME));
        ofn.lStructSize = sizeof(OPENFILENAME);
        ofn.hwndOwner = GLFWAPI::glfwGetWin32Window(static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow()));
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        if (initialDir)
        {
            ofn.lpstrInitialDir = initialDir;
        }
        else if (::GetCurrentDirectoryA(256, currentDir))
        {
            ofn.lpstrInitialDir = currentDir;
        }
        else
        {
            // No additional handling required.
        }
        ofn.lpstrFilter = filter;
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (FALSE != ::GetOpenFileNameA(&ofn))
        {
            return ofn.lpstrFile;
        }
        return {};
    }

    std::string FileDialogs::SaveFile(const char* const filter, const char* const initialDir)
    {
        OPENFILENAMEA ofn;
        CHAR szFile[260] = { 0 };
        CHAR currentDir[256] = { 0 };
        ZeroMemory(&ofn, sizeof(OPENFILENAME));
        ofn.lStructSize = sizeof(OPENFILENAME);
        ofn.hwndOwner = GLFWAPI::glfwGetWin32Window(static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow()));
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        if (initialDir)
        {
            ofn.lpstrInitialDir = initialDir;
        }
        else if (::GetCurrentDirectoryA(256, currentDir))
        {
            ofn.lpstrInitialDir = currentDir;
        }
        else
        {
            // No additional handling required.
        }
        ofn.lpstrFilter = filter;
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

        // Sets the default extension by extracting it from the filter
        ofn.lpstrDefExt = std::strchr(filter, '\0') + 1;

        if (FALSE != ::GetSaveFileNameA(&ofn))
        {
            return ofn.lpstrFile;
        }
        return {};
    }

    void FileDialogs::ShowInFileManager(const std::filesystem::path& path)
    {
        // canonical() resolves the path relative to the process cwd and fails (sets
        // ec) when it does not exist — so it doubles as the existence check.
        std::error_code ec;
        const std::filesystem::path canonical = std::filesystem::canonical(path, ec);
        if (ec)
        {
            OLO_CORE_WARN("ShowInFileManager: cannot resolve path '{}': {}", path.string(), ec.message());
            return;
        }

        // ShellExecuteW returns an HINSTANCE whose value <= 32 indicates failure;
        // surface it as a warning so a failed launch isn't silent (matching the
        // header contract and the Linux backend's logging).
        if (std::filesystem::is_directory(canonical, ec) && !ec)
        {
            // Open the directory itself in a new Explorer window.
            const HINSTANCE rc = ::ShellExecuteW(nullptr, L"explore", canonical.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(rc) <= 32)
                OLO_CORE_WARN("ShowInFileManager: failed to open '{}' (ShellExecute code {})", canonical.string(), reinterpret_cast<INT_PTR>(rc));
        }
        else
        {
            // Select the file inside its parent folder. The path is quoted because
            // /select treats everything after the comma as the (possibly spaced) path.
            const std::wstring args = L"/select,\"" + canonical.wstring() + L"\"";
            const HINSTANCE rc = ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(rc) <= 32)
                OLO_CORE_WARN("ShowInFileManager: failed to reveal '{}' (ShellExecute code {})", canonical.string(), reinterpret_cast<INT_PTR>(rc));
        }
    }

    MessagePromptResult MessagePrompt::YesNoCancel(const char* const title, const char* const message)
    {
        OLO_PROFILE_FUNCTION();

        HWND hwnd = GLFWAPI::glfwGetWin32Window(static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow()));
        int const result = ::MessageBoxA(hwnd, message, title, MB_YESNOCANCEL | MB_ICONWARNING);
        switch (result)
        {
            case IDYES:
                return MessagePromptResult::Yes;
            case IDNO:
                return MessagePromptResult::No;
            default:
                return MessagePromptResult::Cancel;
        }
    }
} // namespace OloEngine
