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
    struct Prototype;
    class SoundGraph;
    class SoundGraphSerializer;
    class SoundGraphCache;
    class SoundGraphFactory;  // Defined in SoundGraphSerializer.cpp
}

namespace OloEngine
{
    /// Connection between two nodes in the sound graph
    struct SoundGraphConnection
    {
        UUID m_SourceNodeID;
        std::string m_SourceEndpoint;
        UUID m_TargetNodeID; 
        std::string m_TargetEndpoint;
        bool m_IsEvent = false;
    };

    /// Node data in the sound graph asset
    struct SoundGraphNodeData
    {
        UUID m_ID;
        std::string m_Name;
        std::string m_Type;  // Node type identifier
        std::unordered_map<std::string, std::string> m_Properties;
        
        // Editor-specific data (position, etc.)
        f32 m_PosX = 0.0f;
        f32 m_PosY = 0.0f;
    };

    /// SoundGraph Asset - serializable representation of a sound graph
    class SoundGraphAsset : public Asset
    {
        // Friend declarations for serialization access
        friend class OloEngine::Audio::SoundGraph::SoundGraphSerializer;
        friend class OloEngine::Audio::SoundGraph::SoundGraphCache;
        friend class OloEngine::Audio::SoundGraph::SoundGraphFactory;
        
    public:
        SoundGraphAsset() = default;
        virtual ~SoundGraphAsset() = default;

        static AssetType GetStaticType() { return AssetType::SoundGraph; }
        virtual AssetType GetAssetType() const override { return GetStaticType(); }

        // Accessors - Name and Description
        const std::string& GetName() const { return m_Name; }
        void SetName(const std::string& name) { m_Name = name; }
        
        const std::string& GetDescription() const { return m_Description; }
        void SetDescription(const std::string& description) { m_Description = description; }
        
        // Node accessors (controlled access to maintain m_NodeIdMap consistency)
        const std::vector<SoundGraphNodeData>& GetNodes() const { return m_Nodes; }
        sizet GetNodeCount() const { return m_Nodes.size(); }
        
        // Connection accessors
        const std::vector<SoundGraphConnection>& GetConnections() const { return m_Connections; }
        sizet GetConnectionCount() const { return m_Connections.size(); }
        
        // Graph configuration accessors
        const std::unordered_map<std::string, std::string>& GetGraphInputs() const { return m_GraphInputs; }
        const std::unordered_map<std::string, std::string>& GetGraphOutputs() const { return m_GraphOutputs; }
        const std::unordered_map<std::string, std::string>& GetLocalVariables() const { return m_LocalVariables; }
        
        // Runtime prototype accessors (non-inline due to incomplete type)
        const Ref<Audio::SoundGraph::Prototype>& GetCompiledPrototype() const;
        void SetCompiledPrototype(const Ref<Audio::SoundGraph::Prototype>& prototype);
        
        // Wave sources accessors
        const std::vector<AssetHandle>& GetWaveSources() const { return m_WaveSources; }
        
        // Version accessor
        u32 GetVersion() const { return m_Version; }

        // Node manipulation methods
        bool HasNode(const UUID& nodeId) const;
        SoundGraphNodeData* GetNode(const UUID& nodeId);
        const SoundGraphNodeData* GetNode(const UUID& nodeId) const;
        
        bool AddNode(const SoundGraphNodeData& node);
        bool RemoveNode(const UUID& nodeId);
        
        // Connection manipulation methods
        bool AddConnection(const SoundGraphConnection& connection);
        bool RemoveConnection(const UUID& sourceNodeId, const std::string& sourceEndpoint,
                            const UUID& targetNodeId, const std::string& targetEndpoint, 
                            bool isEvent);
        
        // Clear all data
        void Clear();
        
        // Rebuild the node ID map from m_Nodes (call after deserialization or batch modifications)
        void RebuildNodeIdMap();
        
        // Validation
        bool IsValid() const;
        std::vector<std::string> GetValidationErrors() const;

    private:
        // Core data
        std::string m_Name;
        std::string m_Description;
        
        // Graph structure
        std::vector<SoundGraphNodeData> m_Nodes;
        std::vector<SoundGraphConnection> m_Connections;
        
        // Graph inputs/outputs configuration
        std::unordered_map<std::string, std::string> m_GraphInputs;
        std::unordered_map<std::string, std::string> m_GraphOutputs;
        std::unordered_map<std::string, std::string> m_LocalVariables;
        
        // Runtime prototype (compiled graph)
        Ref<Audio::SoundGraph::Prototype> m_CompiledPrototype;
        
        // Referenced wave sources
        std::vector<AssetHandle> m_WaveSources;
        
        // Serialization version for compatibility
        u32 m_Version = 1;
        
        // Fast node ID lookup: maps UUID to index in m_Nodes
        std::unordered_map<UUID, sizet> m_NodeIdMap;
    };

    /// SoundGraphSound Asset - specific instance configuration of a sound graph  
    class SoundGraphSoundAsset : public Asset
    {
        // Friend declarations for serialization access
        friend class OloEngine::Audio::SoundGraph::SoundGraphSerializer;
        
    public:
        SoundGraphSoundAsset() = default;
        virtual ~SoundGraphSoundAsset() = default;

        static AssetType GetStaticType() { return AssetType::SoundGraphSound; }
        virtual AssetType GetAssetType() const override { return GetStaticType(); }
        
        // Accessors
        const std::string& GetName() const { return m_Name; }
        void SetName(const std::string& name) { m_Name = name; }
        
        const std::string& GetDescription() const { return m_Description; }
        void SetDescription(const std::string& description) { m_Description = description; }
        
        AssetHandle GetSoundGraphHandle() const { return m_SoundGraphHandle; }
        void SetSoundGraphHandle(AssetHandle handle) { m_SoundGraphHandle = handle; }
        
        const std::unordered_map<std::string, std::string>& GetParameterOverrides() const { return m_ParameterOverrides; }
        void SetParameterOverride(const std::string& param, const std::string& value) { m_ParameterOverrides[param] = value; }
        void ClearParameterOverrides() { m_ParameterOverrides.clear(); }
        
        f32 GetVolume() const { return m_Volume; }
        void SetVolume(f32 volume) { m_Volume = volume; }
        
        f32 GetPitch() const { return m_Pitch; }
        void SetPitch(f32 pitch) { m_Pitch = pitch; }
        
        bool GetLoop() const { return m_Loop; }
        void SetLoop(bool loop) { m_Loop = loop; }
        
    private:
        std::string m_Name;
        std::string m_Description;
        
        // Reference to the base sound graph
        AssetHandle m_SoundGraphHandle = 0;
        
        // Parameter overrides for this specific sound instance
        std::unordered_map<std::string, std::string> m_ParameterOverrides;
        
        // Audio properties specific to this sound
        f32 m_Volume = 1.0f;
        f32 m_Pitch = 1.0f;
        bool m_Loop = false;
    };

} // namespace OloEngine
