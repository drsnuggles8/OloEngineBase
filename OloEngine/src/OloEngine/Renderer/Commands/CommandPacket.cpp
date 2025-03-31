#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"

namespace OloEngine
{
    void CommandPacket::Execute() const
    {
        if (Dispatch)
        {
            Dispatch(&Header);
        }
        else
        {
            OLO_CORE_WARN("CommandPacket::Execute: No dispatch function registered for command type {}", static_cast<int>(Header.Type));
        }
    }

    void CommandPacket::LinkAfter(CommandPacket* packet)
    {
        if (!packet)
            return;
        
        // Connect this packet after the provided packet
        Next = packet->Next;
        Prev = packet;
        
        // Update neighboring links
        packet->Next = this;
        if (Next)
            Next->Prev = this;
    }

    void CommandPacket::LinkBefore(CommandPacket* packet)
    {
        if (!packet)
            return;
        
        // Connect this packet before the provided packet
        Next = packet;
        Prev = packet->Prev;
        
        // Update neighboring links
        packet->Prev = this;
        if (Prev)
            Prev->Next = this;
    }

    void CommandPacket::Unlink()
    {
        // Connect neighbors to each other, bypassing this packet
        if (Prev)
            Prev->Next = Next;
            
        if (Next)
            Next->Prev = Prev;
            
        // Clear our links
        Next = nullptr;
        Prev = nullptr;
    }
}