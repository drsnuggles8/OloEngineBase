// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/HAL/PlatformMisc.h"

#ifdef OLO_PLATFORM_WINDOWS

#include "Platform/Windows/WindowsHWrapper.h"

namespace OloEngine
{
    FProcessorGroupDesc FPlatformMisc::QueryProcessorGroupDesc()
    {
        FProcessorGroupDesc Result;

        // Use GetLogicalProcessorInformationEx to query processor groups
        using FnGetLogicalProcessorInformationEx = BOOL(WINAPI*)(
            LOGICAL_PROCESSOR_RELATIONSHIP,
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
            PDWORD);

        static FnGetLogicalProcessorInformationEx GetLogicalProcessorInformationExFn =
            reinterpret_cast<FnGetLogicalProcessorInformationEx>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetLogicalProcessorInformationEx"));

        if (GetLogicalProcessorInformationExFn)
        {
            DWORD BufferSize = 0;
            GetLogicalProcessorInformationExFn(RelationGroup, nullptr, &BufferSize);

            if (BufferSize > 0)
            {
                auto* Buffer = static_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                    HeapAlloc(GetProcessHeap(), 0, BufferSize));

                if (Buffer && GetLogicalProcessorInformationExFn(RelationGroup, Buffer, &BufferSize))
                {
                    Result.NumProcessorGroups = static_cast<u16>(Buffer->Group.ActiveGroupCount);

                    for (u16 GroupIndex = 0;
                         GroupIndex < Result.NumProcessorGroups &&
                         GroupIndex < FProcessorGroupDesc::MaxNumProcessorGroups;
                         ++GroupIndex)
                    {
                        Result.ThreadAffinities[GroupIndex] =
                            Buffer->Group.GroupInfo[GroupIndex].ActiveProcessorMask;
                    }
                }

                if (Buffer)
                {
                    HeapFree(GetProcessHeap(), 0, Buffer);
                }
            }
        }

        // Fallback for single-group systems or if query failed
        if (Result.NumProcessorGroups == 0)
        {
            SYSTEM_INFO SysInfo;
            GetSystemInfo(&SysInfo);
            Result.NumProcessorGroups = 1;
            Result.ThreadAffinities[0] = SysInfo.dwActiveProcessorMask;
        }

        return Result;
    }

} // namespace OloEngine

#endif // OLO_PLATFORM_WINDOWS
