#include "OloEnginePCH.h"
#include "CapturedFrameData.h"

namespace OloEngine
{
    const char* CapturedCommandData::GetCommandTypeString() const
    {
        return CommandTypeToString(m_CommandType);
    }
} // namespace OloEngine
