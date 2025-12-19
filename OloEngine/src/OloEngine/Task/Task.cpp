// Task.cpp - High-level task API implementation
// Ported from UE5.7 Tasks/Task.cpp

#include "OloEngine/Task/Task.h"

namespace OloEngine::Tasks
{
    // ============================================================================
    // FTaskPriorityCVar Implementation
    // ============================================================================

    FTaskPriorityCVar::FTaskPriorityCVar(
        const char* Name,
        const char* Help,
        ETaskPriority DefaultPriority,
        EExtendedTaskPriority DefaultExtendedPriority)
        : m_Priority(DefaultPriority), m_ExtendedPriority(DefaultExtendedPriority), m_Name(Name), m_Help(Help)
    {
        // Note: In UE5.7, this creates an FAutoConsoleVariableRef that allows
        // runtime configuration via console commands. OloEngine doesn't have
        // a console variable system yet, so we just store the defaults.
        //
        // When a console system is implemented, this should:
        // 1. Register a console variable with the given name
        // 2. Parse strings like "Normal None" or "High GameThreadNormalPri"
        // 3. Update m_Priority and m_ExtendedPriority when the cvar changes
        //
        // Example UE5.7 format: "[TaskPriority] [ExtendedTaskPriority]"
        // where TaskPriority is: High, Normal, BackgroundHigh, BackgroundNormal, BackgroundLow
        // and ExtendedTaskPriority is: None, Inline, TaskEvent, GameThreadNormalPri, etc.
    }

} // namespace OloEngine::Tasks
