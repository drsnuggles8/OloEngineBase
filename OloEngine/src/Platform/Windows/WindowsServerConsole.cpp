// Windows implementation of ServerConsolePlatform.
//
// Unblocking a thread parked in std::getline(std::cin, ...) is the whole job
// here, and on Windows it takes two different mechanisms depending on what
// stdin actually IS:
//
//   * a redirected pipe or file -> the read sits in ReadFile, and CancelIoEx
//     cancels it;
//   * a console -> the read sits in ReadConsole, which is NOT ordinary file
//     I/O. CancelIoEx routinely fails on a console handle with
//     ERROR_NOT_FOUND, so the reliable wake is to post a synthetic Return key
//     into the input buffer with WriteConsoleInput and let the pending read
//     complete normally.
//
// A server that cannot do this does not merely lose its console: ServerConsole
// ::Shutdown() JOINS the input thread, so a read that never unblocks is a
// process that never exits. That is a hang with no output and no error — see
// the header comment on HasUsableStdin() for the case that produced one.

#include "OloEnginePCH.h"
#include "OloEngine/Server/ServerConsolePlatform.h"

#ifdef OLO_PLATFORM_WINDOWS

#include <atomic>
#include <iostream>
#include <new>
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
    namespace
    {
        // Is there a standard input this process can actually read?
        //
        // GetStdHandle returns **NULL** — not INVALID_HANDLE_VALUE — when the
        // process simply has no such handle: a service, a detached launch, or a
        // child created with bInheritHandles=FALSE and no STARTF_USESTDHANDLES.
        // INVALID_HANDLE_VALUE means the call itself failed. Testing only the
        // latter (which this file used to do) lets a NULL through to
        // CancelIoEx(NULL, ...), which fails with ERROR_INVALID_HANDLE, cancels
        // nothing, and leaves Shutdown()'s join waiting forever.
        [[nodiscard]] bool HasUsableStdin(HANDLE& outHandle)
        {
            outHandle = ::GetStdHandle(STD_INPUT_HANDLE);
            return outHandle != nullptr && outHandle != INVALID_HANDLE_VALUE;
        }
    } // namespace

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
        // Use std::nothrow so an allocation failure surfaces as an empty
        // AbortStatePtr (per the header's documented contract) rather than
        // propagating std::bad_alloc out of a platform factory.
        return AbortStatePtr(new (std::nothrow) AbortState());
    }

    ReadResult ReadLine(AbortState& state, std::string& outLine)
    {
        outLine.clear();

        if (state.Aborted.load(std::memory_order_acquire))
        {
            return ReadResult::Aborted;
        }

        // No stdin at all: report end-of-stream rather than blocking on a read
        // that can never complete and can never be cancelled. The caller then
        // stops the input thread immediately, which is what lets a headless
        // server started without standard handles shut down at all.
        if (HANDLE stdinHandle = nullptr; !HasUsableStdin(stdinHandle))
        {
            return ReadResult::EndOfStream;
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

        HANDLE stdinHandle = nullptr;
        if (!HasUsableStdin(stdinHandle))
        {
            // Nothing to cancel — and nothing is blocked, because ReadLine()
            // above refuses to read in this state.
            return;
        }

        // A console read is not cancellable through CancelIoEx; wake it by
        // giving it the newline it is waiting for. GetConsoleMode succeeding is
        // the test for "this really is a console handle" — on a pipe or a file
        // it fails and we fall through to the cancel path.
        if (DWORD consoleMode = 0; ::GetConsoleMode(stdinHandle, &consoleMode) != 0)
        {
            INPUT_RECORD newline{};
            newline.EventType = KEY_EVENT;
            newline.Event.KeyEvent.bKeyDown = TRUE;
            newline.Event.KeyEvent.wRepeatCount = 1;
            newline.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
            newline.Event.KeyEvent.uChar.AsciiChar = '\r';

            DWORD written = 0;
            if (::WriteConsoleInputA(stdinHandle, &newline, 1, &written) != 0 && written == 1)
            {
                return;
            }
            // Fall through: if the synthetic key could not be posted, a cancel
            // is still better than leaving the reader parked.
        }

        ::CancelIoEx(stdinHandle, nullptr);
    }

} // namespace OloEngine::ServerConsolePlatform

#endif // OLO_PLATFORM_WINDOWS
