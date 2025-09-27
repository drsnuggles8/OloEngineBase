#pragma once

#include <string>
#include <filesystem>

// Forward declarations
namespace OloEngine
{
    class SoundGraphAsset;
}

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
        static std::string SerializeToString(const SoundGraphAsset& asset);

        /**
         * @brief Deserialize a SoundGraphAsset from YAML string
         * @param asset Output SoundGraphAsset to populate
         * @param yamlString Input YAML string
         * @return true on success, false on failure
         */
        static bool DeserializeFromString(SoundGraphAsset& asset, const std::string& yamlString);

        /**
         * @brief Serialize a SoundGraphAsset to file
         * @param asset The SoundGraphAsset to serialize
         * @param filePath Path to output file
         * @return true on success, false on failure
         */
        static bool Serialize(const SoundGraphAsset& asset, const std::filesystem::path& filePath);

        /**
         * @brief Deserialize a SoundGraphAsset from file
         * @param asset Output SoundGraphAsset to populate
         * @param filePath Path to input file
         * @return true on success, false on failure
         */
        static bool Deserialize(SoundGraphAsset& asset, const std::filesystem::path& filePath);

    private:
        SoundGraphSerializer() = delete; // Static class
    };

} // namespace OloEngine::Audio::SoundGraph