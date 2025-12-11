// OloEngine
// Ported from Unreal Engine's Misc/Exec.cpp

#include "OloEngine/Misc/Exec.h"

namespace OloEngine
{

FExec::~FExec()
{
}

#if OLO_ALLOW_EXEC_COMMANDS

bool FExec::Exec(const char* Cmd, FOutputDevice& Ar)
{
    bool bExecSuccess = false;

#if OLO_ALLOW_EXEC_EDITOR
    bExecSuccess = bExecSuccess || Exec_Editor(Cmd, Ar);
#endif

#if OLO_ALLOW_EXEC_DEV
    bExecSuccess = bExecSuccess || Exec_Dev(Cmd, Ar);
#endif

    bExecSuccess = bExecSuccess || Exec_Runtime(Cmd, Ar);

    return bExecSuccess;
}

#else // OLO_ALLOW_EXEC_COMMANDS

bool FExec::Exec(const char* Cmd, FOutputDevice& Ar)
{
    (void)Ar;
    return Exec(Cmd);
}

bool FExec::Exec(const char* Cmd)
{
    (void)Cmd;
    OLO_CORE_ASSERT(false, "Exec commands are disabled in this build");
    // Note: UE5 also calls FCoreDelegates::OnDisallowedExecCommandCalled.Broadcast(Cmd) here
    return false;
}

#endif // !OLO_ALLOW_EXEC_COMMANDS

} // namespace OloEngine
