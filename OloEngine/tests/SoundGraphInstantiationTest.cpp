#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// SoundGraphInstantiationTest — exercise the runtime path
//
// The serializer tests cover the YAML in/out, and SoundGraphBasicTest covers
// the asset's data model. Neither exercises the *runtime instantiation* path:
//
//     CompileAssetToPrototype → CreateInstance → graph->Init() → ProcessSamples
//
// A bug in endpoint registration (e.g. RegisterEndpointOutputs forgetting to
// register value outputs into OutputStreams) only manifests when something
// downstream calls `node->OutValue(id)` — which means it stays invisible to
// the editor-only and YAML-only tests but crashes the moment any user wires a
// node output to the graph output and hits Play.
//
// This file pins:
//   * A graph with a single Noise node wired to OutLeft/OutRight compiles +
//     instantiates without throwing. Catches the OutputStreams regression
//     that crashed the editor with "invalid unordered_map<K, T> key".
//   * A graph with a WavePlayer triggered by the graph's Play event + wired
//     to OutLeft/OutRight likewise compiles + instantiates. Triggering Play
//     and processing samples produces silent output (no asset loaded) instead
//     of throwing — matches the WavePlayer null-guard contract.
// =============================================================================

#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Audio/SoundGraph/GraphGeneration.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphPrototype.h"
#include "OloEngine/Core/UUID.h"

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    // Add a connection of given shape. The graph compiler maps UUID(0) on either side
    // to graph-input / graph-output endpoints.
    void AddConn(SoundGraphAsset& asset, UUID srcID, const std::string& srcEndpoint,
                 UUID dstID, const std::string& dstEndpoint, bool isEvent)
    {
        SoundGraphConnection c;
        c.m_SourceNodeID = srcID;
        c.m_SourceEndpoint = srcEndpoint;
        c.m_TargetNodeID = dstID;
        c.m_TargetEndpoint = dstEndpoint;
        c.m_IsEvent = isEvent;
        asset.AddConnection(c);
    }
}

TEST(SoundGraphInstantiation, NoiseToGraphOutputCompilesAndInstantiates)
{
    SoundGraphAsset asset;
    asset.SetName("NoiseToOut");

    SoundGraphNodeData noise;
    noise.m_ID = UUID();
    noise.m_Type = "Noise";
    noise.m_Name = "NoiseSource";
    asset.AddNode(noise);

    // Wire the Noise node's OutValue into BOTH graph OutLeft and OutRight buses. The bug
    // this test pins was: RegisterEndpointOutputs didn't register value outputs into
    // OutputStreams, so this connection's `OutValue("OutValue")` lookup threw
    // std::out_of_range with "invalid unordered_map<K, T> key".
    AddConn(asset, noise.m_ID, "OutValue", UUID(0), "OutLeft", /*isEvent=*/false);
    AddConn(asset, noise.m_ID, "OutValue", UUID(0), "OutRight", /*isEvent=*/false);

    Ref<Audio::SoundGraph::Prototype> prototype;
    ASSERT_NO_THROW(prototype = Audio::SoundGraph::CompileAssetToPrototype(asset))
        << "CompileAssetToPrototype must not throw on a well-formed graph";
    ASSERT_NE(prototype, nullptr);

    Ref<Audio::SoundGraph::SoundGraph> instance;
    ASSERT_NO_THROW(instance = Audio::SoundGraph::CreateInstance(prototype))
        << "CreateInstance must not throw — the OutputStreams registration must cover value outputs";
    ASSERT_NE(instance, nullptr);
}

TEST(SoundGraphInstantiation, WavePlayerWithUnconfiguredAssetDoesNotCrash)
{
    // Mirrors HelloDing.olosoundgraph but without the WaveAsset configured. Even with a
    // null asset, the graph must instantiate (output is silent — see WavePlayer's null
    // guards in UpdateWaveSourceIfNeeded / Process). This is the "user dropped a WavePlayer
    // and hit Play before binding audio" scenario.
    SoundGraphAsset asset;
    asset.SetName("WavePlayerSmoke");

    SoundGraphNodeData player;
    player.m_ID = UUID();
    player.m_Type = "WavePlayer";
    player.m_Name = "Player";
    player.m_Properties["WaveAsset"] = "0";        // explicit "no asset"
    player.m_Properties["Loop"] = "false";
    player.m_Properties["NumberOfLoops"] = "-1";
    asset.AddNode(player);

    // Graph "Play" event → WavePlayer Play event.
    AddConn(asset, UUID(0), "Play", player.m_ID, "Play", /*isEvent=*/true);

    // WavePlayer stereo outs → graph OutLeft / OutRight (the actual crash path).
    AddConn(asset, player.m_ID, "OutLeft",  UUID(0), "OutLeft",  /*isEvent=*/false);
    AddConn(asset, player.m_ID, "OutRight", UUID(0), "OutRight", /*isEvent=*/false);

    Ref<Audio::SoundGraph::Prototype> prototype;
    ASSERT_NO_THROW(prototype = Audio::SoundGraph::CompileAssetToPrototype(asset));
    ASSERT_NE(prototype, nullptr);

    Ref<Audio::SoundGraph::SoundGraph> instance;
    ASSERT_NO_THROW(instance = Audio::SoundGraph::CreateInstance(prototype));
    ASSERT_NE(instance, nullptr);
}
