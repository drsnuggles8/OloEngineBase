#pragma once

#include <filesystem>
#include <string>

// Forward-declared so yaml-cpp stays out of every TU that only wants to save or
// load a graph — the SoundGraphSerializer shape, not the ShaderGraph one (which
// is a shim into AssetSerializer and needs a live AssetManager, making it
// untestable in isolation).
namespace YAML
{
    class Emitter;
    class Node;
} // namespace YAML

namespace OloEngine::VisualScript
{
    class VisualScriptAsset;
    struct VisualScriptGraph;
    struct VisualScriptNode;
    struct VisualScriptLink;
    struct VisualScriptVariable;

    /// YAML persistence for `.olovs` assets.
    ///
    /// The round-trip contract (AC#1) is byte-identity: save → load → save must
    /// produce the same text. Two things make that hold and must not be relaxed —
    /// every map the graph owns is ordered (`std::map`, not `unordered_map`), and
    /// floats go through PinValue's shortest-round-trip storage form rather than
    /// a fixed precision. `VisualScriptSerializerTest` pins both.
    class VisualScriptSerializer
    {
      public:
        [[nodiscard]] static std::string SerializeToString(const VisualScriptAsset& asset);
        [[nodiscard]] static bool DeserializeFromString(VisualScriptAsset& asset, const std::string& yamlText);

        [[nodiscard]] static bool Serialize(const VisualScriptAsset& asset, const std::filesystem::path& path);
        [[nodiscard]] static bool Deserialize(VisualScriptAsset& asset, const std::filesystem::path& path);

        VisualScriptSerializer() = delete;

      private:
        static void SerializeGraph(YAML::Emitter& out, const VisualScriptGraph& graph);
        static void SerializeVariable(YAML::Emitter& out, const VisualScriptVariable& variable);
        [[nodiscard]] static bool DeserializeGraph(const YAML::Node& node, VisualScriptGraph& outGraph);
        [[nodiscard]] static bool DeserializeVariable(const YAML::Node& node, VisualScriptVariable& outVariable);
    };

} // namespace OloEngine::VisualScript
