#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/TransparentStringHash.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptGraph.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptTypes.h"

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine::VisualScript
{
    class NodeContext;

    //==============================================================================
    /// What the compiler and the VM need to know about a node type beyond its pins.
    enum class NodeFlags : u32
    {
        None = 0,
        /// An entry point. Has no exec INPUT; the VM starts a run at it when the
        /// matching trigger fires.
        Event = 1u << 0,
        /// Has no exec pins at all: a pure data function, pull-evaluated on demand
        /// and memoized for the duration of one exec step.
        Pure = 1u << 1,
        /// May suspend and resume across frames (Delay, WaitForEvent). The VM
        /// keeps a pending record for it on the instance.
        Latent = 1u << 2,
        /// Performs an EnTT structural change. Routed through Scene's deferred
        /// entity-command queue — never applied inline. See
        /// docs/agent-rules/script-structural-command-safe-point.md.
        Structural = 1u << 3,
    };

    [[nodiscard]] constexpr NodeFlags operator|(NodeFlags a, NodeFlags b) noexcept
    {
        return static_cast<NodeFlags>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    [[nodiscard]] constexpr bool HasFlag(NodeFlags value, NodeFlags flag) noexcept
    {
        return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0;
    }

    //==============================================================================
    /// One registered node type. Pins are static by default; the handful of types
    /// whose signature depends on authoring (variable accessors, Sequence, the
    /// function nodes) supply a resolver instead.
    struct NodeTypeDescriptor
    {
        /// The stable, serialized key — "Flow.Branch". Never change one without a
        /// migration: it is what a saved graph stores.
        std::string m_TypeName;
        std::string m_DisplayName;
        std::string m_Category;
        std::string m_Tooltip;

        std::vector<PinDescriptor> m_Pins;
        NodeFlags m_Flags = NodeFlags::None;

        /// Optional. When set, the compiler calls it instead of using m_Pins.
        std::function<std::vector<PinDescriptor>(const VisualScriptNode&, const VisualScriptAsset&)> m_ResolvePins;

        /// Exactly one of these is set. `m_Evaluate` fills the node's output data
        /// pins and must be side-effect free (it can be called any number of
        /// times, in any order). `m_Execute` runs when an exec token arrives.
        std::function<void(NodeContext&)> m_Evaluate;
        std::function<void(NodeContext&)> m_Execute;

        [[nodiscard]] const std::vector<PinDescriptor>& StaticPins() const
        {
            return m_Pins;
        }
    };

    //==============================================================================
    /// Type-name-keyed node factory, mirroring SoundGraphFactory's role but keyed
    /// on a readable string rather than a hashed Identifier — the editor's
    /// context-search menu, the YAML serializer and the compiler all need to move
    /// between the name and the descriptor, and a hash is one-way.
    class NodeRegistry
    {
      public:
        [[nodiscard]] static NodeRegistry& Get();

        /// Registers `descriptor`, replacing any previous entry with the same type
        /// name. Returns false (and logs) when the descriptor is malformed —
        /// missing a name, or supplying neither an Evaluate nor an Execute body.
        bool Register(NodeTypeDescriptor descriptor);

        [[nodiscard]] const NodeTypeDescriptor* Find(std::string_view typeName) const;

        /// Every registered type, sorted by (category, display name) so the menu
        /// and any generated documentation are stable across runs.
        ///
        /// Returns BY VALUE: the descriptors themselves are stable (nothing is
        /// ever erased), but the cache vector is rebuilt on registration, so
        /// handing out a reference would let a caller hold a span that another
        /// thread reallocates underneath it.
        [[nodiscard]] std::vector<const NodeTypeDescriptor*> GetSorted() const;

        [[nodiscard]] sizet GetCount() const
        {
            const std::lock_guard lock(m_Mutex);
            return m_Types.size();
        }

        /// Registers the standard library exactly once. Idempotent and safe to
        /// call from anywhere (test fixtures, the asset serializer, the system) —
        /// there is no static-init order to get wrong because nothing registers at
        /// namespace scope.
        static void EnsureStandardLibrary();

      private:
        /// Registration happens lazily via EnsureStandardLibrary, which the asset
        /// serializer can reach from the asset-system worker thread while the
        /// game thread is compiling a graph. call_once alone only orders the
        /// one-time init — it does not protect Find/GetSorted against a
        /// concurrent Register, nor the mutable sort cache against two readers.
        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, NodeTypeDescriptor, StringHash, StringEqual> m_Types;
        mutable std::vector<const NodeTypeDescriptor*> m_Sorted;
        mutable bool m_SortedDirty = true;
    };

    //-- Standard library registration, one translation unit per category ---------
    void RegisterEventNodes(NodeRegistry& registry);
    void RegisterFlowNodes(NodeRegistry& registry);
    void RegisterMathNodes(NodeRegistry& registry);
    void RegisterVariableNodes(NodeRegistry& registry);
    void RegisterEntityNodes(NodeRegistry& registry);
    void RegisterUtilityNodes(NodeRegistry& registry);
    void RegisterFunctionNodes(NodeRegistry& registry);
    /// Graph -> text-script bridge (Lua + C#). Its own TU because it is the only
    /// part of the node library that pulls in sol2 and Mono.
    void RegisterScriptBridgeNodes(NodeRegistry& registry);

    //-- Well-known type names referenced by the compiler / VM --------------------
    // Constants, not literals at each use site: the compiler special-cases these
    // few types, and a typo would silently produce a graph that never runs.
    namespace NodeTypes
    {
        inline constexpr std::string_view kOnBeginPlay = "Event.OnBeginPlay";
        inline constexpr std::string_view kOnUpdate = "Event.OnUpdate";
        inline constexpr std::string_view kOnEndPlay = "Event.OnEndPlay";
        inline constexpr std::string_view kOnCollisionEnter = "Event.OnCollisionEnter";
        inline constexpr std::string_view kOnTriggerEnter = "Event.OnTriggerEnter";
        inline constexpr std::string_view kCustomEvent = "Event.CustomEvent";
        inline constexpr std::string_view kOnGameplayEvent = "Event.OnGameplayEvent";
        inline constexpr std::string_view kFunctionEntry = "Function.Entry";
        inline constexpr std::string_view kFunctionReturn = "Function.Return";
        inline constexpr std::string_view kFunctionCall = "Function.Call";
        inline constexpr std::string_view kGetVariable = "Variable.Get";
        inline constexpr std::string_view kSetVariable = "Variable.Set";
        inline constexpr std::string_view kSequence = "Flow.Sequence";
    } // namespace NodeTypes

    //-- Property keys used by the resolvers -------------------------------------
    namespace NodeProps
    {
        inline constexpr std::string_view kVariableName = "Variable";
        inline constexpr std::string_view kEventName = "Event";
        inline constexpr std::string_view kFunctionName = "Function";
        inline constexpr std::string_view kOutputCount = "Outputs";
    } // namespace NodeProps

} // namespace OloEngine::VisualScript
