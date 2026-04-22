// Linux implementation of ServerConsolePlatform.
//
// Uses a self-pipe + poll() + non-blocking read() on STDIN_FILENO to assemble
// lines incrementally. Because we never call std::getline (which blocks until
// a newline is actually delivered), Signal() from another thread reliably
// wakes the worker even when stdin is a redirected pipe that has bytes but no
// newline yet, or is idle.

#include "OloEnginePCH.h"
#include "OloEngine/Server/ServerConsolePlatform.h"

#ifdef OLO_PLATFORM_LINUX

#include "OloEngine/Core/Log.h"

#include <cerrno>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace OloEngine::ServerConsolePlatform
{
    struct AbortState
    {
        int WakeupPipe[2] = { -1, -1 };
        std::string PendingBuffer; ///< Bytes read from stdin that don't yet form a complete line.
        bool EndOfStream = false;  ///< Set once stdin reports EOF or a hard error.

        // Original STDIN_FILENO flags captured before Create() flips O_NONBLOCK.
        // Restored in the destructor so the process-wide mode change doesn't leak
        // to code that runs after ServerConsole::Shutdown().
        int OriginalStdinFlags = -1;
        bool DidModifyStdinFlags = false;

        ~AbortState()
        {
            for (auto& fd : WakeupPipe)
            {
                if (fd != -1)
                {
                    ::close(fd);
                    fd = -1;
                }
            }
            // Restore STDIN's original flags if we changed them.
            if (DidModifyStdinFlags && OriginalStdinFlags != -1)
            {
                (void)::fcntl(STDIN_FILENO, F_SETFL, OriginalStdinFlags);
            }
        }
    };

    namespace
    {
        /// Make a file descriptor non-blocking. Returns true on success.
        bool SetNonBlocking(int fd)
        {
            const int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags == -1)
            {
                return false;
            }
            if ((flags & O_NONBLOCK) != 0)
            {
                return true; // already non-blocking
            }
            return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
        }

        /// Scan PendingBuffer for a complete line. If found, extract into outLine
        /// (without the trailing '\n', plus any '\r' before it) and erase from the
        /// buffer. Returns true if a line was produced.
        bool ExtractLine(std::string& buffer, std::string& outLine)
        {
            const auto newlinePos = buffer.find('\n');
            if (newlinePos == std::string::npos)
            {
                return false;
            }
            sizet lineEnd = newlinePos;
            if (lineEnd > 0 && buffer[lineEnd - 1] == '\r')
            {
                --lineEnd;
            }
            outLine.assign(buffer, 0, lineEnd);
            buffer.erase(0, newlinePos + 1);
            return true;
        }
    } // namespace

    void AbortStateDeleter::operator()(AbortState* state) const
    {
        delete state;
    }

    AbortStatePtr Create()
    {
        // std::nothrow so allocation failure surfaces as an empty AbortStatePtr
        // rather than a std::bad_alloc escaping a platform factory.
        AbortState* raw = new (std::nothrow) AbortState();
        if (raw == nullptr)
        {
            OLO_CORE_ERROR("[ServerConsolePlatform] Failed to allocate AbortState");
            return nullptr;
        }
        auto state = std::unique_ptr<AbortState, AbortStateDeleter>(raw);
        if (::pipe(state->WakeupPipe) != 0)
        {
            OLO_CORE_ERROR("[ServerConsolePlatform] Failed to create wakeup pipe");
            return nullptr;
        }
        // Ensure both ends of the wakeup pipe are non-blocking so Signal() never
        // stalls even if the pipe fills up, and the reader can drain without blocking.
        if (!SetNonBlocking(state->WakeupPipe[0]) || !SetNonBlocking(state->WakeupPipe[1]))
        {
            OLO_CORE_ERROR("[ServerConsolePlatform] Failed to set wakeup pipe non-blocking");
            return nullptr;
        }
        // Capture the current STDIN flags so the destructor can restore them, then
        // best-effort put STDIN into non-blocking mode so ReadLine never blocks
        // inside read(). If the fcntl(F_SETFL) fails (e.g. OS refuses to flip a
        // tty), ReadLine still works but may block in read() until data arrives.
        state->OriginalStdinFlags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
        if (state->OriginalStdinFlags != -1 && (state->OriginalStdinFlags & O_NONBLOCK) == 0)
        {
            if (::fcntl(STDIN_FILENO, F_SETFL, state->OriginalStdinFlags | O_NONBLOCK) != -1)
            {
                state->DidModifyStdinFlags = true;
            }
        }
        return state;
    }

    ReadResult ReadLine(AbortState& state, std::string& outLine)
    {
        outLine.clear();

        // Hot path: the buffer already holds a complete line from a previous read.
        if (ExtractLine(state.PendingBuffer, outLine))
        {
            return ReadResult::Line;
        }
        if (state.EndOfStream)
        {
            return ReadResult::EndOfStream;
        }

        constexpr sizet ReadChunkSize = 4096;
        char chunk[ReadChunkSize];

        while (true)
        {
            // Wait for either stdin bytes or a Signal() wakeup.
            struct pollfd fds[2]{};
            fds[0].fd = STDIN_FILENO;
            fds[0].events = POLLIN;
            fds[1].fd = state.WakeupPipe[0];
            fds[1].events = POLLIN;

            int pollRet;
            do
            {
                pollRet = ::poll(fds, 2, -1);
            } while (pollRet == -1 && errno == EINTR);

            if (pollRet == -1)
            {
                // Real poll() error (not EINTR) — treat as end-of-stream so the
                // worker exits rather than spinning.
                state.EndOfStream = true;
                return ReadResult::EndOfStream;
            }

            if ((fds[1].revents & POLLIN) != 0)
            {
                // Drain the wakeup pipe so a future Signal() can re-trigger poll.
                char drain[16];
                while (::read(state.WakeupPipe[0], drain, sizeof(drain)) > 0)
                {
                    // keep draining until empty
                }
                return ReadResult::Aborted;
            }

            // Check stdin error conditions BEFORE POLLIN/POLLHUP so a broken or
            // invalid descriptor breaks out of the loop instead of spinning.
            if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0)
            {
                state.EndOfStream = true;
                return ReadResult::EndOfStream;
            }

            if ((fds[0].revents & (POLLIN | POLLHUP)) == 0)
            {
                continue;
            }

            // Read what's available. With O_NONBLOCK set we'll get EAGAIN when the
            // kernel buffer is drained; without it, read() returns as soon as the
            // kernel has any bytes (stdin is line-buffered by the tty driver in
            // canonical mode, and pipes deliver whatever was written).
            const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
            if (n > 0)
            {
                state.PendingBuffer.append(chunk, static_cast<sizet>(n));
                if (ExtractLine(state.PendingBuffer, outLine))
                {
                    return ReadResult::Line;
                }
                // No newline yet — loop and poll for more bytes.
                continue;
            }
            if (n == 0)
            {
                // EOF. Flush any trailing bytes as a final line if non-empty.
                state.EndOfStream = true;
                if (!state.PendingBuffer.empty())
                {
                    outLine = std::move(state.PendingBuffer);
                    state.PendingBuffer.clear();
                    return ReadResult::Line;
                }
                return ReadResult::EndOfStream;
            }
            // n == -1
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // No data yet — go back to poll().
                continue;
            }
            // Hard error.
            state.EndOfStream = true;
            return ReadResult::EndOfStream;
        }
    }

    void Signal(AbortState& state)
    {
        if (state.WakeupPipe[1] != -1)
        {
            char dummy = 0;
            for (;;)
            {
                const ssize_t w = ::write(state.WakeupPipe[1], &dummy, 1);
                if (w == 1)
                {
                    break;
                }
                if (w == -1 && errno == EINTR)
                {
                    continue;
                }
                // EAGAIN: pipe already has pending wakeup bytes, so the reader will
                // wake up anyway. Any other error is unrecoverable from here.
                break;
            }
        }
    }

} // namespace OloEngine::ServerConsolePlatform

#endif // OLO_PLATFORM_LINUX
