// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/HAL/PlatformProcess.h"

#ifdef OLO_PLATFORM_WINDOWS

#include "Platform/Windows/WindowsHWrapper.h"
#include <malloc.h> // For _alloca
#include <vector>

namespace OloEngine
{
    int FPlatformProcess::TranslateThreadPriority(EThreadPriority Priority)
    {
        switch (Priority)
        {
            case EThreadPriority::TPri_AboveNormal:
                return THREAD_PRIORITY_ABOVE_NORMAL;
            case EThreadPriority::TPri_Normal:
                return THREAD_PRIORITY_NORMAL;
            case EThreadPriority::TPri_BelowNormal:
                return THREAD_PRIORITY_BELOW_NORMAL;
            case EThreadPriority::TPri_Highest:
                return THREAD_PRIORITY_HIGHEST;
            case EThreadPriority::TPri_TimeCritical:
                return THREAD_PRIORITY_HIGHEST;
            case EThreadPriority::TPri_Lowest:
                return THREAD_PRIORITY_LOWEST;
            // There is no such thing as slightly below normal on Windows.
            // This can't be below normal since we don't want latency sensitive tasks
            // to go to efficient cores on Alder Lake (hybrid architecture).
            case EThreadPriority::TPri_SlightlyBelowNormal:
                return THREAD_PRIORITY_NORMAL;
            default:
                return THREAD_PRIORITY_NORMAL;
        }
    }

    void* FPlatformProcess::GetCurrentThreadHandle()
    {
        return ::GetCurrentThread();
    }

    void FPlatformProcess::SetThreadAffinityMask(u64 AffinityMask)
    {
        if (AffinityMask != GetNoAffinityMask())
        {
            ::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(AffinityMask));
        }
    }

    void FPlatformProcess::SetThreadPriority(EThreadPriority Priority)
    {
        ::SetThreadPriority(::GetCurrentThread(), TranslateThreadPriority(Priority));
    }

    void FPlatformProcess::SetThreadPriority(std::thread& Thread, EThreadPriority Priority)
    {
        if (Thread.joinable())
        {
            HANDLE Handle = static_cast<HANDLE>(Thread.native_handle());
            ::SetThreadPriority(Handle, TranslateThreadPriority(Priority));
        }
    }

    void FPlatformProcess::SetThreadName(const char* Name)
    {
        // Windows 10 1607+ supports SetThreadDescription for thread naming
        // This shows up in debuggers and profilers
        if (Name)
        {
            // Convert UTF-8 to wide string
            int WideLen = ::MultiByteToWideChar(CP_UTF8, 0, Name, -1, nullptr, 0);
            if (WideLen <= 0)
            {
                return;
            }

            // Use stack allocation for reasonably-sized names; fall back to heap
            // for anything that could risk a stack overflow via _alloca.
            constexpr int MaxStackWideLen = 4096;
            wchar_t* WideName = nullptr;
            std::vector<wchar_t> HeapBuffer;
            if (WideLen <= MaxStackWideLen)
            {
                WideName = static_cast<wchar_t*>(_alloca(static_cast<sizet>(WideLen) * sizeof(wchar_t)));
            }
            else
            {
                HeapBuffer.resize(static_cast<sizet>(WideLen));
                WideName = HeapBuffer.data();
            }

            if (::MultiByteToWideChar(CP_UTF8, 0, Name, -1, WideName, WideLen) == 0)
            {
                return;
            }

            // SetThreadDescription is available on Windows 10 1607+
            // We use GetProcAddress to avoid requiring the newer SDK
            typedef HRESULT(WINAPI * SetThreadDescriptionFn)(HANDLE, PCWSTR);
            static SetThreadDescriptionFn SetThreadDescriptionPtr = nullptr;
            if (static bool bChecked = false; !bChecked)
            {
                if (HMODULE hKernel32 = ::GetModuleHandleW(L"kernel32.dll"); hKernel32)
                {
                    SetThreadDescriptionPtr = reinterpret_cast<SetThreadDescriptionFn>(
                        ::GetProcAddress(hKernel32, "SetThreadDescription"));
                }
                bChecked = true;
            }

            if (SetThreadDescriptionPtr)
            {
                SetThreadDescriptionPtr(::GetCurrentThread(), WideName);
            }
        }
    }

    void FPlatformProcess::YieldThread()
    {
        ::SwitchToThread();
    }

    void FPlatformProcess::SetThreadGroupAffinity(u64 AffinityMask, u16 ProcessorGroup)
    {
        if (AffinityMask == GetNoAffinityMask())
        {
            return;
        }

        // Use SetThreadGroupAffinity for multi-group systems (>64 cores)
        // This API is available on Windows 7+ and is required for systems with >64 logical processors
        GROUP_AFFINITY GroupAffinity = {};
        GroupAffinity.Mask = static_cast<KAFFINITY>(AffinityMask);
        GroupAffinity.Group = ProcessorGroup;

        ::SetThreadGroupAffinity(::GetCurrentThread(), &GroupAffinity, nullptr);
    }

} // namespace OloEngine

#endif // OLO_PLATFORM_WINDOWS
