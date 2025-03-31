#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Commands/CommandTypes.h"

namespace OloEngine
{
    // Command packet wrapping a command with chain linkage
    struct CommandPacket 
    {
        CommandPacket* Next = nullptr;
        CommandPacket* Prev = nullptr;
        
        void* AuxiliaryMemory = nullptr;
        DispatchFn Dispatch = nullptr;
        
        CommandHeader Header;

        // Helper methods
        template<typename T>
        T* GetCommand() { return reinterpret_cast<T*>(&Header); }

        template<typename T>
        const T* GetCommand() const { return reinterpret_cast<const T*>(&Header); }
        
        void Execute() const;

        // Chain management
        void LinkAfter(CommandPacket* packet);
        void LinkBefore(CommandPacket* packet);
        void Unlink();
    };
}