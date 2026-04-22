// Platform-specific helper for reading lines from stdin in a way that can be
// aborted from another thread. Used by ServerConsole.
//
// The engine needs to wake a worker that is blocked reading stdin when the
// server shuts down. Windows uses `CancelIoEx(stdin)` inside std::getline;
// POSIX uses a self-pipe + poll() + non-blocking read to assemble lines while
// remaining interruptible. This interface hides both.
//
// Typical usage:
//
//   auto abort = ServerConsolePlatform::Create();  // AbortStatePtr (unique_ptr)
//   if (!abort) {
//       // Create() failed (e.g. pipe/handle exhaustion) — skip stdin handling
//       // or bail out of initialization. *abort must not be dereferenced.
//       return;
//   }
//   // ... worker thread ...
//   std::string line;
//   while (running) {
//       auto r = ServerConsolePlatform::ReadLine(*abort, line);
//       if (r != ServerConsolePlatform::ReadResult::Line) break;  // EOF or aborted
//       // ... consume line ...
//   }
//   // ... shutdown thread ...
//   ServerConsolePlatform::Signal(*abort);
//   worker.join();
//   // `abort` goes out of scope here; AbortStateDeleter runs automatically.

#pragma once

#include "OloEngine/Core/Base.h"

#include <memory>
#include <string>

namespace OloEngine::ServerConsolePlatform
{
    struct AbortState; // opaque — definition lives in platform .cpp

    struct AbortStateDeleter
    {
        void operator()(AbortState* state) const;
    };
    using AbortStatePtr = std::unique_ptr<AbortState, AbortStateDeleter>;

    enum class ReadResult
    {
        Line,        ///< A full line was read into outLine (trailing newline stripped).
        EndOfStream, ///< stdin hit EOF / hard error. Worker should exit.
        Aborted,     ///< Signal() was called from another thread. Worker should exit.
    };

    /// Create a new abort state. Returns an empty pointer on failure.
    AbortStatePtr Create();

    /// Block until a full line is available on stdin, then return it in outLine.
    /// Returns Aborted if Signal() is called concurrently, or EndOfStream on EOF /
    /// unrecoverable stream error. `outLine` is always cleared on entry and only
    /// populated for ReadResult::Line.
    ReadResult ReadLine(AbortState& state, std::string& outLine);

    /// Called from another thread to unblock a pending ReadLine call.
    void Signal(AbortState& state);

} // namespace OloEngine::ServerConsolePlatform
