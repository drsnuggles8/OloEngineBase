// Platform-specific helper for aborting a blocking `std::getline(std::cin, ...)`
// call from another thread. Used by ServerConsole.
//
// The engine needs to wake a thread that is blocked inside `std::getline` on
// stdin when the server shuts down. Windows exposes `CancelIoEx(stdin)` for
// this; POSIX needs a self-pipe + `poll()` dance. This interface hides both.
//
// Typical usage:
//
//   auto abort = ServerConsolePlatform::Create();  // AbortStatePtr (unique_ptr)
//   // ... worker thread ...
//   while (running) {
//       if (!ServerConsolePlatform::WaitForStdin(*abort)) break;  // aborted/EOF
//       if (!std::getline(std::cin, line)) break;
//       // ... consume line ...
//   }
//   // ... shutdown thread ...
//   ServerConsolePlatform::Signal(*abort);
//   worker.join();
//   // `abort` goes out of scope here; AbortStateDeleter runs automatically.

#pragma once

#include "OloEngine/Core/Base.h"

#include <memory>

namespace OloEngine::ServerConsolePlatform
{
    struct AbortState; // opaque — definition lives in platform .cpp

    struct AbortStateDeleter
    {
        void operator()(AbortState* state) const;
    };
    using AbortStatePtr = std::unique_ptr<AbortState, AbortStateDeleter>;

    /// Create a new abort state. Returns an empty pointer on failure.
    AbortStatePtr Create();

    /// Block until stdin has input available OR Signal() has been called.
    /// Returns true if stdin is ready (caller should read), false if aborted / EOF.
    bool WaitForStdin(AbortState& state);

    /// Called from another thread to unblock a pending WaitForStdin / std::getline.
    void Signal(AbortState& state);

} // namespace OloEngine::ServerConsolePlatform
