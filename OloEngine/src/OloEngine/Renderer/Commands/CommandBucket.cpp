#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include <algorithm>

namespace OloEngine
{
    CommandBucket::~CommandBucket()
    {
        Clear();
    }

    void CommandBucket::AddPacket(const CommandKey& key, CommandPacket* packet)
    {
        if (!packet)
        {
            OLO_CORE_WARN("CommandBucket::AddPacket: Attempted to add null packet!");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Commands.push_back({ key, packet });
            m_IsSorted = false;
        }

        m_CommandCount++;
    }

    void CommandBucket::Sort()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (m_IsSorted)
            return;

        // Sort commands by key
        std::sort(m_Commands.begin(), m_Commands.end());
        
        // Update the ordered commands vector
        m_OrderedCommands.clear();
        m_OrderedCommands.reserve(m_Commands.size());
        
        for (const auto& keyedPacket : m_Commands)
        {
            m_OrderedCommands.push_back(keyedPacket.Packet);
        }
        
        m_IsSorted = true;
    }

    void CommandBucket::Execute()
    {
        OLO_PROFILE_FUNCTION();
        
        // Make sure commands are sorted before execution
        if (!m_IsSorted)
            Sort();
            
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        for (CommandPacket* packet : m_OrderedCommands)
        {
            packet->Execute();
        }
    }
    
    void CommandBucket::MergeCommands()
    {
        OLO_PROFILE_FUNCTION();
        
        // Make sure commands are sorted before merging
        if (!m_IsSorted)
            Sort();
            
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        if (m_OrderedCommands.empty())
            return;
            
        // TODO: Implement command merging logic
        // This would involve analyzing consecutive commands with the same key
        // and potentially combining their data
    }

    void CommandBucket::Clear()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        m_Commands.clear();
        m_OrderedCommands.clear();
        m_CommandCount = 0;
        m_IsSorted = false;
    }
}