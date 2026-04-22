// Linux implementation of ServerConsolePlatform.
// Uses a self-pipe + poll() to abort a blocking stdin read from another thread.

#include "OloEnginePCH.h"
#include "OloEngine/Server/ServerConsolePlatform.h"

#ifdef OLO_PLATFORM_LINUX

#include "OloEngine/Core/Log.h"

#include <cerrno>
#include <poll.h>
#include <unistd.h>

namespace OloEngine::ServerConsolePlatform
{
    struct AbortState
    {
        int WakeupPipe[2] = { -1, -1 };

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
        }
    };

    void AbortStateDeleter::operator()(AbortState* state) const
    {
        delete state;
    }

    AbortStatePtr Create()
    {
        auto state = std::unique_ptr<AbortState, AbortStateDeleter>(new AbortState());
        if (::pipe(state->WakeupPipe) != 0)
        {
            OLO_CORE_ERROR("[ServerConsolePlatform] Failed to create wakeup pipe");
            return nullptr;
        }
        return state;
    }

    bool WaitForStdin(AbortState& state)
    {
        struct pollfd fds[2]{};
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[1].fd = state.WakeupPipe[0];
        fds[1].events = POLLIN;

        int ret;
        do
        {
            ret = ::poll(fds, 2, -1);
        } while (ret == -1 && errno == EINTR);

        if (ret <= 0)
        {
            // Real error (not an interrupted syscall) — treat as abort.
            return false;
        }
        if (fds[1].revents & POLLIN)
        {
            // Wakeup pipe signalled — abort.
            return false;
        }
        return (fds[0].revents & POLLIN) != 0;
    }

    void Signal(AbortState& state)
    {
        if (state.WakeupPipe[1] != -1)
        {
            char dummy = 0;
            (void)::write(state.WakeupPipe[1], &dummy, 1);
        }
    }

} // namespace OloEngine::ServerConsolePlatform

#endif // OLO_PLATFORM_LINUX
