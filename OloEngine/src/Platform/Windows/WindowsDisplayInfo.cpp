#include "OloEnginePCH.h"
#include "OloEngine/Renderer/DisplayOutput.h"

#include <dxgi1_6.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <string>

namespace OloEngine
{
    namespace
    {
        // Releases a COM pointer at scope exit; DXGI is the only COM this file
        // touches and a full ComPtr dependency is not worth one function.
        template<typename T>
        struct ComRelease
        {
            T* Ptr = nullptr;
            ComRelease() = default;
            ComRelease(const ComRelease&) = delete;
            ComRelease& operator=(const ComRelease&) = delete;
            ~ComRelease()
            {
                if (Ptr != nullptr)
                    Ptr->Release();
            }
        };

        [[nodiscard]] std::string Narrow(const WCHAR* wide)
        {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (bytes <= 1)
                return {};
            std::string out(static_cast<sizet>(bytes - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
            return out;
        }
    } // namespace

    // DXGI is loaded at run time rather than linked: it is a Windows system DLL
    // on every supported version, but a missing export degrades to "unknown"
    // instead of a process that cannot start.
    void QueryPlatformDisplayInfo(void* glfwWindow, DisplayCapabilities& caps)
    {
        caps.OsHdr = DisplayHdrState::Unknown;

        HMONITOR monitor = nullptr;
        if (glfwWindow != nullptr)
        {
            if (HWND hwnd = GLFWAPI::glfwGetWin32Window(static_cast<GLFWwindow*>(glfwWindow)); hwnd != nullptr)
                monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        }
        if (monitor == nullptr)
            monitor = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        if (dxgi == nullptr)
            return;
        using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
        const auto createFactory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        if (createFactory == nullptr)
        {
            FreeLibrary(dxgi);
            return;
        }

        {
            ComRelease<IDXGIFactory1> factory;
            if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory.Ptr))))
            {
                FreeLibrary(dxgi);
                return;
            }
            bool found = false;
            for (UINT adapterIndex = 0; !found; ++adapterIndex)
            {
                ComRelease<IDXGIAdapter1> adapter;
                if (factory.Ptr->EnumAdapters1(adapterIndex, &adapter.Ptr) == DXGI_ERROR_NOT_FOUND)
                    break;
                for (UINT outputIndex = 0; !found; ++outputIndex)
                {
                    ComRelease<IDXGIOutput> output;
                    if (adapter.Ptr->EnumOutputs(outputIndex, &output.Ptr) == DXGI_ERROR_NOT_FOUND)
                        break;
                    ComRelease<IDXGIOutput6> output6;
                    if (FAILED(output.Ptr->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6.Ptr))))
                        continue;
                    DXGI_OUTPUT_DESC1 desc{};
                    if (FAILED(output6.Ptr->GetDesc1(&desc)) || desc.Monitor != monitor)
                        continue;

                    found = true;
                    // G2084 (PQ) over Rec.2020 is what DXGI reports for a display
                    // the OS composes in HDR; anything else is SDR composition.
                    caps.OsHdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? DisplayHdrState::On
                                                                                              : DisplayHdrState::Off;
                    caps.PeakNits = desc.MaxLuminance;
                    caps.MaxFullFrameNits = desc.MaxFullFrameLuminance;
                    caps.MinNits = desc.MinLuminance;
                    caps.RedPrimary = { desc.RedPrimary[0], desc.RedPrimary[1] };
                    caps.GreenPrimary = { desc.GreenPrimary[0], desc.GreenPrimary[1] };
                    caps.BluePrimary = { desc.BluePrimary[0], desc.BluePrimary[1] };
                    caps.WhitePoint = { desc.WhitePoint[0], desc.WhitePoint[1] };

                    std::string name = Narrow(desc.DeviceName);
                    // The monitor's own name, when the device reports one.
                    DISPLAY_DEVICEW device{};
                    device.cb = sizeof(device);
                    if (EnumDisplayDevicesW(desc.DeviceName, 0, &device, 0) != FALSE)
                        name += " (" + Narrow(device.DeviceString) + ")";
                    name += ", " + std::to_string(desc.BitsPerColor) + " bpc";
                    caps.DisplayName = FString(name);
                }
            }
        }
        FreeLibrary(dxgi);
    }

    bool PollPlatformDisplayChange(void* glfwWindow)
    {
        // A factory goes stale (IsCurrent() == FALSE) when the OS display
        // configuration changes, an HDR toggle included: the documented way to
        // notice one (Microsoft's D3D12HDR sample). The factory and the DLL
        // are held for the life of the process; both are cheap, and making a
        // factory per frame is not.
        struct Cache
        {
            HMODULE Dll = nullptr;
            IDXGIFactory1* Factory = nullptr;
            HMONITOR Monitor = nullptr;
            bool Initialised = false;
        };
        static Cache s_Cache;

        HMONITOR monitor = nullptr;
        if (glfwWindow != nullptr)
        {
            if (HWND hwnd = GLFWAPI::glfwGetWin32Window(static_cast<GLFWwindow*>(glfwWindow)); hwnd != nullptr)
                monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        }

        const auto makeFactory = []() -> IDXGIFactory1*
        {
            if (s_Cache.Dll == nullptr)
                s_Cache.Dll = LoadLibraryW(L"dxgi.dll");
            if (s_Cache.Dll == nullptr)
                return nullptr;
            using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
            const auto createFactory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(s_Cache.Dll, "CreateDXGIFactory1"));
            IDXGIFactory1* factory = nullptr;
            if (createFactory == nullptr || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
                return nullptr;
            return factory;
        };

        if (!s_Cache.Initialised)
        {
            s_Cache.Initialised = true;
            s_Cache.Factory = makeFactory();
            s_Cache.Monitor = monitor;
            return false; // the first look is the baseline the swapchain was made against
        }

        bool changed = false;
        if (monitor != s_Cache.Monitor)
        {
            s_Cache.Monitor = monitor;
            changed = true;
        }
        if (s_Cache.Factory != nullptr && s_Cache.Factory->IsCurrent() == FALSE)
        {
            s_Cache.Factory->Release();
            s_Cache.Factory = makeFactory();
            changed = true;
        }
        return changed;
    }
} // namespace OloEngine
