#pragma once

#include "SoundGraphPrototype.h"
#include "SoundGraph.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"

#include <string>
#include <vector>

// Forward decl — full include lives in the .cpp where the helper is defined.
namespace OloEngine
{
    class SoundGraphAsset;
}

namespace OloEngine::Audio::SoundGraph
{
    struct GraphGeneratorOptions
    {
        std::string m_Name;
        u32 m_NumInChannels;
        u32 m_NumOutChannels;
        Ref<Prototype> m_GraphPrototype;
        // Note: Editor model and cache dependencies removed for now
    };

    Ref<Prototype> ConstructPrototype(const GraphGeneratorOptions& options, std::vector<UUID>& waveAssetsToLoad);

    /** Create instance of SoundGraph from Prototype for playback */
    Ref<SoundGraph> CreateInstance(const Ref<Prototype>& prototype);

    // Sentinel node ID used in SoundGraphConnection to represent the graph itself
    // (rather than a real node). Connections with m_SourceNodeID == kGraphPseudoNodeID
    // are routed from a graph input endpoint; m_TargetNodeID == kGraphPseudoNodeID are
    // routed into a graph output endpoint. The editor draws this as a "Graph Output"
    // pseudo-node; the compiler turns these into the appropriate typed Prototype
    // connection (NodeValue_GraphValue etc.).
    inline constexpr u64 kGraphPseudoNodeIDValue = 0ULL;
    inline const UUID kGraphPseudoNodeID{ kGraphPseudoNodeIDValue };

    /** Compile a SoundGraphAsset's node + connection data into an executable Prototype.
        This is what the editor calls on Save and what Scene::InitAudioRuntime calls
        lazily on first use if no prototype has been compiled yet. Returns nullptr on
        failure (no nodes, invalid types, etc.). Stereo I/O by default. */
    Ref<Prototype> CompileAssetToPrototype(const SoundGraphAsset& asset,
                                           u32 numInChannels = 2,
                                           u32 numOutChannels = 2);

} // namespace OloEngine::Audio::SoundGraph
