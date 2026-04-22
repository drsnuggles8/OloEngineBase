// Windows implementation of ServerConsolePlatform.
// Uses CancelIoEx to unblock a stdin read from another thread.

#include "OloEnginePCH.h"
#include "OloEngine/Server/ServerConsolePlatform.h"

#ifdef OLO_PLATFORM_WINDOWS

#include <atomic>
#include <iostream>
#include <string>

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
        // `Aborted` is set by Signal() before CancelIoEx so that if ReadLine races
        // Signal and completes before the cancel, we still classify the subsequent
        // iteration as Aborted rather than EndOfStream.
        std::atomic<bool> Aborted{ false };
    };

    void AbortStateDeleter::operator()(AbortState* state) const
    {
        delete state;
    }

    AbortStatePtr Create()
    {
        return AbortStatePtr(new AbortState());
    }

    ReadResult ReadLine(AbortState& state, std::string& outLine)
    {
        outLine.clear();

        if (state.Aborted.load(std::memory_order_acquire))
        {
            return ReadResult::Aborted;
        }

        // std::getline blocks inside ReadFile; Signal() calls CancelIoEx to wake it.
        // On cancel, getline typically reports failure — we differentiate Aborted
        // from real EOF by checking the Aborted flag after the call.
        if (std::getline(std::cin, outLine))
        {
            return ReadResult::Line;
        }

        if (state.Aborted.load(std::memory_order_acquire))
        {
            return ReadResult::Aborted;
        }
        return ReadResult::EndOfStream;
    }

    void Signal(AbortState& state)
    {
        state.Aborted.store(true, std::memory_order_release);
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        if (hStdin != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(hStdin, nullptr);
        }
    }

} // namespace OloEngine::ServerConsolePlatform

#endif // OLO_PLATFORM_WINDOWS
