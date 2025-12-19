// Ported from Unreal Engine's Misc/Exec.h

#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class FOutputDevice;

    #ifndef OLO_ALLOW_EXEC_COMMANDS
        #if defined(OLO_DIST)
            #define OLO_ALLOW_EXEC_COMMANDS 0
        #else
            #define OLO_ALLOW_EXEC_COMMANDS 1
        #endif
    #endif

    #ifndef OLO_ALLOW_EXEC_DEV
        #define OLO_ALLOW_EXEC_DEV !defined(OLO_DIST) && OLO_ALLOW_EXEC_COMMANDS
    #endif

    #ifndef OLO_ALLOW_EXEC_EDITOR
        #define OLO_ALLOW_EXEC_EDITOR OLO_ALLOW_EXEC_COMMANDS
    #endif

    // Any object that is capable of taking commands.
    class FExec
    {
    public:
        virtual ~FExec();

    #if OLO_ALLOW_EXEC_COMMANDS

        // Exec handler
        //
        // @param Cmd Command to parse
        // @param Ar Output device to log to
        //
        // @return true if command was handled, false otherwise
        virtual bool Exec(const char* Cmd, FOutputDevice& Ar);

    #else // OLO_ALLOW_EXEC_COMMANDS

        // final override of Exec that asserts if called
        virtual bool Exec(const char* Cmd, FOutputDevice& Ar) final;

        // final override of Exec to replace overrides where a default value for Ar is provided
        virtual bool Exec(const char* Cmd) final;

    #endif // !OLO_ALLOW_EXEC_COMMANDS

    protected:
        // Implementation of Exec that is called on all targets where OLO_ALLOW_EXEC_COMMANDS is true
        virtual bool Exec_Runtime(const char* Cmd, FOutputDevice& Ar) { (void)Cmd; (void)Ar; return false; }

        // Implementation of Exec that is only called in non-shipping targets
        virtual bool Exec_Dev(const char* Cmd, FOutputDevice& Ar) { (void)Cmd; (void)Ar; return false; }

        // Implementation of Exec that is only called in editor
        virtual bool Exec_Editor(const char* Cmd, FOutputDevice& Ar) { (void)Cmd; (void)Ar; return false; }
    };

} // namespace OloEngine
