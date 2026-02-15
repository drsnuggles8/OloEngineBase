#include "OloEnginePCH.h"
#include "CapturedFrameData.h"

namespace OloEngine
{
    const char* CapturedCommandData::GetCommandTypeString() const
    {
        OLO_PROFILE_FUNCTION();
        return CommandTypeToString(m_CommandType);
    }
} // namespace OloEngine
