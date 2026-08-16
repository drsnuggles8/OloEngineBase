#pragma once

#include "OloEngine/Core/UUID.h"

#include <string>

namespace OloEngine
{
    /// The one payload type visual scripting adds to `GameplayEventBus`.
    ///
    /// A graph cannot name an arbitrary C++ type, and the bus is keyed by
    /// `std::type_index` — so a graph-authored event travels as a NAME plus a
    /// stringified payload inside this single type. Non-graph subscribers (UI,
    /// audio, analytics) can therefore listen for graph events with one
    /// `Subscribe<VisualScriptCustomEvent>`, and a text script can trigger graph
    /// flow by publishing one.
    ///
    /// Declared in OloEngine (not OloEngine::VisualScript) so subscribing does not
    /// drag the whole VM header into a UI panel.
    struct VisualScriptCustomEvent
    {
        std::string m_Name;
        std::string m_Payload;
        /// 0 broadcasts to every graph in the scene.
        UUID m_Target{ 0 };
        UUID m_Sender{ 0 };
    };

} // namespace OloEngine
