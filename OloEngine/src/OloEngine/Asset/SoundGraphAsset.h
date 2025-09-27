#pragma once

#include "Asset.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"

#include <vector>
#include <string>
#include <unordered_map>

// Forward declarations
namespace OloEngine::Audio::SoundGraph
{
    class SoundGraphPrototype;
    struct SoundGraph;
}

namespace OloEngine
{
    /// Connection between two nodes in the sound graph
    struct SoundGraphConnection
    {
        UUID SourceNodeID;
        std::string SourceEndpoint;
        UUID TargetNodeID; 
        std::string TargetEndpoint;
        bool IsEvent = false;
    };

    /// Node data in the sound graph asset
    struct SoundGraphNodeData
    {
        UUID ID;
        std::string Name;
        std::string Type;  // Node type identifier
        std::unordered_map<std::string, std::string> Properties;
        
        // Editor-specific data (position, etc.)
        float PosX = 0.0f;
        float PosY = 0.0f;
    };

    /// SoundGraph Asset - serializable representation of a sound graph
    class SoundGraphAsset : public Asset
    {
    public:
        std::string Name;
        std::string Description;
        
        // Graph structure
        std::vector<SoundGraphNodeData> Nodes;
        std::vector<SoundGraphConnection> Connections;
        
        // Graph inputs/outputs configuration
        std::unordered_map<std::string, std::string> GraphInputs;
        std::unordered_map<std::string, std::string> GraphOutputs;
        std::unordered_map<std::string, std::string> LocalVariables;
        
        // Runtime prototype (compiled graph)
        // TODO: Implement SoundGraphPrototype when runtime audio graph system is ready
        // Ref<Audio::SoundGraph::SoundGraphPrototype> CompiledPrototype;
        
        // Referenced wave sources
        std::vector<AssetHandle> WaveSources;
        
        // Serialization version for compatibility
        u32 Version = 1;

        SoundGraphAsset() = default;
        virtual ~SoundGraphAsset() = default;

        static AssetType GetStaticType() { return AssetType::SoundGraph; }
        virtual AssetType GetAssetType() const override { return GetStaticType(); }

        // Utility methods
        bool HasNode(const UUID& nodeId) const;
        SoundGraphNodeData* GetNode(const UUID& nodeId);
        const SoundGraphNodeData* GetNode(const UUID& nodeId) const;
        
        void AddNode(const SoundGraphNodeData& node);
        bool RemoveNode(const UUID& nodeId);
        
        void AddConnection(const SoundGraphConnection& connection);
        bool RemoveConnection(const UUID& sourceNodeId, const std::string& sourceEndpoint,
                            const UUID& targetNodeId, const std::string& targetEndpoint);
        
        // Clear all data
        void Clear();
        
        // Validation
        bool IsValid() const;
        std::vector<std::string> GetValidationErrors() const;
    };

    /// SoundGraphSound Asset - specific instance configuration of a sound graph  
    class SoundGraphSoundAsset : public Asset
    {
    public:
        std::string Name;
        std::string Description;
        
        // Reference to the base sound graph
        AssetHandle SoundGraphHandle;
        
        // Parameter overrides for this specific sound instance
        std::unordered_map<std::string, std::string> ParameterOverrides;
        
        // Audio properties specific to this sound
        float Volume = 1.0f;
        float Pitch = 1.0f;
        bool Loop = false;
        
        SoundGraphSoundAsset() = default;
        virtual ~SoundGraphSoundAsset() = default;

        static AssetType GetStaticType() { return AssetType::SoundGraphSound; }
        virtual AssetType GetAssetType() const override { return GetStaticType(); }
    };

} // namespace OloEngine