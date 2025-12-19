#pragma once

#include <string>
#include <filesystem>
#include "../../Asset/SoundGraphAsset.h"

// Forward declare YAML types to avoid including yaml-cpp in header
namespace YAML
{
    class Emitter;
    class Node;
} // namespace YAML

namespace OloEngine::Audio::SoundGraph
{
    /**
     * @brief Serializer for SoundGraph assets to/from YAML format
     *
     * Handles serialization and deserialization of SoundGraphAsset objects
     * to/from YAML files for editor and runtime use.
     */
    class SoundGraphSerializer
    {
      public:
        /**
         * @brief Serialize a SoundGraphAsset to YAML string
         * @param asset The SoundGraphAsset to serialize
         * @return YAML string representation, or empty string on failure
         */
        static std::string SerializeToString(const OloEngine::SoundGraphAsset& asset);

        /**
         * @brief Deserialize a SoundGraphAsset from YAML string
         * @param asset Output SoundGraphAsset to populate
         * @param yamlString Input YAML string
         * @return true on success, false on failure
         */
        static bool DeserializeFromString(OloEngine::SoundGraphAsset& asset, const std::string& yamlString);

        /**
         * @brief Serialize a SoundGraphAsset to file
         * @param asset The SoundGraphAsset to serialize
         * @param filePath Path to output file
         * @return true on success, false on failure
         */
        static bool Serialize(const OloEngine::SoundGraphAsset& asset, const std::filesystem::path& filePath);

        /**
         * @brief Deserialize a SoundGraphAsset from file
         * @param asset Output SoundGraphAsset to populate
         * @param filePath Path to input file
         * @return true on success, false on failure
         */
        static bool Deserialize(OloEngine::SoundGraphAsset& asset, const std::filesystem::path& filePath);

      private:
        // Helper serialization methods
        static YAML::Emitter& SerializeNodeData(YAML::Emitter& out, const OloEngine::SoundGraphNodeData& nodeData);
        static YAML::Emitter& SerializeConnection(YAML::Emitter& out, const OloEngine::SoundGraphConnection& connection);

        static bool DeserializeNodeData(const YAML::Node& node, OloEngine::SoundGraphNodeData& outNodeData);
        static bool DeserializeConnection(const YAML::Node& node, OloEngine::SoundGraphConnection& outConnection);

        SoundGraphSerializer() = delete; // Static class
    };

} // namespace OloEngine::Audio::SoundGraph
