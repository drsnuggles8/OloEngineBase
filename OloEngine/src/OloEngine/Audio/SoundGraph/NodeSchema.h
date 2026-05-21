#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
    // Minimal, hand-maintained schema describing the editable input parameters of each
    // SoundGraph node type. Used by:
    //   * SoundGraphEditorPanel — render typed widgets in the property sidebar.
    //   * CompileAssetToPrototype — convert SoundGraphNodeData::m_Properties (string map)
    //     into typed Prototype::Node::m_DefaultValuePlugs entries.
    //
    // This duplicates information that NodeProcessor subclasses encode in their
    // NodeDescription specializations, but extracting it from those templates is
    // non-trivial and out of scope for the first editor pass. New node types added to
    // SoundGraphFactory should pick up matching entries here so they're authorable
    // in-editor; if they aren't listed, the editor falls back to the raw m_Properties
    // string editor and the compiler skips populating defaults (nodes use their
    // hardcoded constructor defaults).
    enum class NodeParamKind
    {
        Float,
        Int,
        Bool,
        AudioAsset, // AssetHandle stored as u64
    };

    struct NodeParamSchema
    {
        std::string Name;        // Endpoint name (matches the node's m_In<Name> pointer).
        NodeParamKind Kind;
        f32 DefaultFloat = 0.0f; // Used when Kind == Float
        i32 DefaultInt = 0;      // Used when Kind == Int
        bool DefaultBool = false; // Used when Kind == Bool
        f32 MinFloat = 0.0f;
        f32 MaxFloat = 1.0f;
        f32 Step = 0.01f;
        const char* Tooltip = nullptr;
    };

    using NodeSchema = std::vector<NodeParamSchema>;

    /** Look up the schema for a node type. Returns nullptr if the type isn't in the
        schema map (caller falls back to a generic string-property editor). */
    const NodeSchema* GetNodeSchema(const std::string& nodeType);

    // --- Pin schema -----------------------------------------------------------------
    //
    // Parallel to NodeSchema (which only describes *editable* properties), NodePinSchema
    // lists *every* pin a node exposes — both inputs and outputs, including event pins
    // that don't appear in the property panel. The editor needs this so a freshly-placed
    // node shows all its wireable pins immediately, instead of starting blank and only
    // sprouting pins as the user makes connections to them.
    //
    // Endpoint names mirror the runtime registration in NodeDescriptions.cpp + each node's
    // ctor (event registrations via AddInEvent). The naming convention is *whatever the
    // reflection / DECLARE_ID expansion produces* after `RemovePrefixAndSuffix` strips `m_`:
    // a member named `m_WaveAsset` registers as "WaveAsset", `m_OutLeft` as "OutLeft",
    // `m_OnPlay` as "OnPlay", and `DECLARE_ID(s_Trigger)` as "s_Trigger". The editor
    // doesn't prettify these because the graph compiler matches by exact identifier —
    // diverging from the runtime name would silently break wiring.
    struct NodePinDescriptor
    {
        std::string Name;
        bool IsEvent = false;
    };

    struct NodePinSchema
    {
        std::vector<NodePinDescriptor> Inputs;
        std::vector<NodePinDescriptor> Outputs;
    };

    /** Look up the pin schema for a node type. Returns nullptr if the type isn't in the
        pin map (caller falls back to connection-derived pin discovery). */
    const NodePinSchema* GetNodePinSchema(const std::string& nodeType);

    // String-form conventions used in SoundGraphNodeData::m_Properties so the editor
    // and compiler agree on how to round-trip values through the YAML asset:
    //   * Float — std::format("{}", value) — e.g. "440" / "0.5"
    //   * Int   — std::format("{}", value) — e.g. "-1"
    //   * Bool  — "true" / "false"
    //   * AudioAsset — std::format("{}", static_cast<u64>(handle)) — e.g. "12345678901234567890"

    // Parse a property string into a typed value matching the schema's Kind. Returns the
    // schema's default if parsing fails. These helpers exist to keep the editor and
    // compiler parsing identical.
    f32 ParsePropertyFloat(const NodeParamSchema& schema, const std::string& valueStr);
    i32 ParsePropertyInt(const NodeParamSchema& schema, const std::string& valueStr);
    bool ParsePropertyBool(const NodeParamSchema& schema, const std::string& valueStr);
    u64 ParsePropertyAssetHandle(const std::string& valueStr);

} // namespace OloEngine::Audio::SoundGraph
