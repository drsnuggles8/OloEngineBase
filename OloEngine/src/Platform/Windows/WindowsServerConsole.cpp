// Windows implementation of ServerConsolePlatform.
// Uses CancelIoEx to unblock a stdin read from another thread.

#include "OloEnginePCH.h"
#include "OloEngine/Server/ServerConsolePlatform.h"

#ifdef OLO_PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace OloEngine::ServerConsolePlatform
{
    struct AbortState
    {
        // Nothing needed — CancelIoEx operates directly on stdin.
    };

    void AbortStateDeleter::operator()(AbortState* state) const
    {
        delete state;
    }

    AbortStatePtr Create()
    {
        return AbortStatePtr(new AbortState());
    }

    bool WaitForStdin(AbortState& /*state*/)
    {
        // On Windows we don't need to poll ahead of std::getline — a pending
        // CancelIoEx will cause ReadFile inside getline to return with EOF,
        // which terminates the loop cleanly.
        return true;
    }

    void Signal(AbortState& /*state*/)
    {
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        if (hStdin != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(hStdin, nullptr);
        }
    }

} // namespace OloEngine::ServerConsolePlatform

#endif // OLO_PLATFORM_WINDOWS
