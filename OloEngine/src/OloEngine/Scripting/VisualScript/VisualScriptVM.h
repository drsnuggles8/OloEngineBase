#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptGraph.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptNodeRegistry.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptTypes.h"

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    class Scene;
} // namespace OloEngine

namespace OloEngine::VisualScript
{
    //==============================================================================
    // The runtime half of issue #634. The execution model — exec-token push with
    // synchronous branch descent, lazy data pull with per-exec-step memoization,
    // and latent suspend/resume — is written up in
    // docs/adr/0014-visual-script-execution-model.md. Read that before changing
    // how Trigger / Suspend / the memo stamp interact.
    //==============================================================================

    /// A resolved exec-wire endpoint: which node to run, and which of its exec
    /// input pins the token arrived on (Gate and DoOnce behave differently
    /// depending on the entry, so the pin is part of the target, not just the
    /// node).
    struct ExecTarget
    {
        i32 m_Node = -1;
        i32 m_Pin = -1;
    };

    /// Per-pin compilation result. Which fields are meaningful depends on the
    /// pin's direction and kind — an input data pin uses the literal/source
    /// triple, an output data pin uses the value slot, an output exec pin uses
    /// the target list.
    struct CompiledPin
    {
        PinValue m_Literal{};
        i32 m_SourceNode = -1;
        i32 m_SourcePin = -1;
        i32 m_ValueSlot = -1;
        std::vector<ExecTarget> m_ExecTargets;
    };

    struct CompiledNode
    {
        NodeId m_SourceId = kInvalidNodeId;
        const NodeTypeDescriptor* m_Type = nullptr;
        std::vector<PinDescriptor> m_Pins;
        std::vector<CompiledPin> m_PinInfo;
        /// Copied, not pointed at: the plan must stay valid across an asset
        /// hot-reload that frees the authored graph out from under it.
        std::map<std::string, std::string> m_Properties;
        /// Function.Call only — index into VisualScriptPlan::m_Functions.
        i32 m_FunctionIndex = -1;
    };

    /// One compiled graph: the asset's event graph (index 0) or one function.
    struct CompiledGraph
    {
        std::string m_Name;
        std::vector<CompiledNode> m_Nodes;
        u32 m_ValueSlotCount = 0;

        /// Entry points, keyed by trigger. See VisualScriptPlan::MakeEventKey.
        std::unordered_map<std::string, std::vector<i32>> m_EventEntries;

        /// Function graphs only.
        i32 m_EntryNode = -1;
        i32 m_ReturnNode = -1;
        std::vector<VisualScriptVariable> m_Inputs;
        std::vector<VisualScriptVariable> m_Outputs;
    };

    struct CompileDiagnostic
    {
        std::string m_Graph;
        NodeId m_Node = kInvalidNodeId;
        std::string m_Message;
    };

    //==============================================================================
    /// An asset compiled into a form the VM can run. Shared by every instance of
    /// that asset (one per entity), which is what makes per-entity instancing
    /// cheap — the same Asset → Prototype → instance layering SoundGraph uses.
    class VisualScriptPlan : public RefCounted
    {
      public:
        [[nodiscard]] static Ref<VisualScriptPlan> Compile(const VisualScriptAsset& asset, std::vector<CompileDiagnostic>& outErrors);

        /// The one place the event-entry key format is defined. `scope` is the
        /// node type name for engine events, "Custom" for CustomEvent, "Bus" for
        /// a GameplayEventBus bridge node.
        [[nodiscard]] static std::string MakeEventKey(std::string_view scope, std::string_view name);

        [[nodiscard]] const CompiledGraph& GetEventGraph() const
        {
            return m_EventGraph;
        }
        [[nodiscard]] const std::vector<CompiledGraph>& GetFunctions() const
        {
            return m_Functions;
        }
        [[nodiscard]] const std::vector<VisualScriptVariable>& GetVariables() const
        {
            return m_Variables;
        }
        [[nodiscard]] i32 FindVariableIndex(std::string_view name) const;
        [[nodiscard]] u32 GetNodeBudgetPerTick() const
        {
            return m_NodeBudgetPerTick;
        }
        /// Every event key with at least one entry node — lets the system skip
        /// work for graphs that do not care about a given trigger.
        [[nodiscard]] bool HasEventEntry(std::string_view key) const;

      private:
        CompiledGraph m_EventGraph;
        std::vector<CompiledGraph> m_Functions;
        std::vector<VisualScriptVariable> m_Variables;
        std::unordered_map<std::string, i32> m_VariableIndex;
        u32 m_NodeBudgetPerTick = 10000;
    };

    //==============================================================================
    /// Mutable scratch a node body owns across ticks. One struct for every node
    /// type rather than a per-type allocation: the fields are small, the set is
    /// closed (counter / timer / flag / one value), and it keeps instance
    /// construction a single vector resize.
    struct NodeState
    {
        i64 m_Counter = 0;
        f32 m_Timer = 0.0f;
        bool m_Flag = false;
        PinValue m_Scratch{};
    };

    /// An event a node asked to publish. Queued rather than dispatched inline —
    /// GameplayEventBus::Publish is synchronous and must not run inside the
    /// system's entity iteration.
    struct EmittedEvent
    {
        std::string m_Name;
        PinValue m_Payload{};
        /// 0 = broadcast to every graph in the scene.
        UUID m_Target{ 0 };
        /// True when the author asked for it to reach the GameplayEventBus too.
        bool m_PublishToBus = false;
    };

    /// A trigger delivered into a graph instance.
    struct IncomingEvent
    {
        std::string m_Key;
        PinValue m_Payload{};
        UUID m_OtherEntity{ 0 };
    };

    /// Everything a node body may reach outside its own graph. Passed per tick
    /// rather than stored on the instance so a headless test can drive a graph
    /// with no Scene at all (m_Scene == nullptr; the ECS nodes then no-op and
    /// report an error rather than dereferencing).
    struct RuntimeContext
    {
        Scene* m_Scene = nullptr;
        f32 m_DeltaTime = 0.0f;
        std::vector<EmittedEvent>* m_EventOutbox = nullptr;
        /// Print/Log sink. The system owns a bounded ring; tests read it to
        /// assert what a graph actually did.
        std::vector<std::string>* m_LogSink = nullptr;
        /// Deterministic per-scene RNG stream for the Random nodes. Nodes must
        /// never call rand() — issue #452 seeds the game-thread stream so a
        /// replay reproduces.
        u64* m_RandomState = nullptr;
    };

    class VisualScriptInstance;

    //==============================================================================
    /// The view of the world a node body gets. Deliberately narrow: a node can
    /// read its inputs, write its outputs, trigger exec pins, suspend, touch its
    /// own scratch state, read variables, and report an error. Anything wider
    /// (arbitrary registry access, publishing inline) is a hazard the VM exists
    /// to prevent.
    class NodeContext
    {
      public:
        NodeContext(VisualScriptInstance& instance, i32 graphIndex, i32 nodeIndex, i32 entryPin, RuntimeContext& runtime)
            : m_Instance(instance), m_Runtime(runtime), m_GraphIndex(graphIndex), m_NodeIndex(nodeIndex), m_EntryPin(entryPin)
        {
        }

        //-- Inputs (pull-evaluated; safe to call repeatedly) ----------------------
        [[nodiscard]] PinValue GetInput(sizet pin) const;
        [[nodiscard]] bool GetInputBool(sizet pin) const;
        [[nodiscard]] i64 GetInputInt(sizet pin) const;
        [[nodiscard]] f32 GetInputFloat(sizet pin) const;
        [[nodiscard]] glm::vec3 GetInputVec3(sizet pin) const;
        [[nodiscard]] std::string GetInputString(sizet pin) const;
        [[nodiscard]] UUID GetInputEntity(sizet pin) const;
        [[nodiscard]] bool IsInputConnected(sizet pin) const;

        //-- Outputs ---------------------------------------------------------------
        void SetOutput(sizet pin, PinValue value) const;

        //-- Control flow ----------------------------------------------------------
        /// Run the branch wired to exec output `pin` to completion, right now.
        /// Loop nodes call this once per iteration.
        void Trigger(sizet pin) const;
        /// Stop here; resume by triggering `resumePin` after `seconds` of scaled
        /// game time. Only legal from a node flagged Latent.
        void SuspendForSeconds(sizet resumePin, f32 seconds) const;
        /// Stop here; resume by triggering `resumePin` when `eventName` arrives.
        void SuspendForEvent(sizet resumePin, std::string eventName) const;
        /// True once the per-tick node budget is spent — loop bodies must check it
        /// so a runaway `While` stops instead of spinning inside one node.
        [[nodiscard]] bool IsBudgetExhausted() const;

        /// Every loop node MUST call this once per iteration and stop when it
        /// returns false. It charges the iteration against the node budget and
        /// invalidates the pure-value memo, which is what makes a `While` with an
        /// EMPTY body terminate: without it nothing inside the loop consumes
        /// budget or refreshes the condition, and the graph hangs the frame.
        [[nodiscard]] bool BeginIteration() const;

        //-- Identity and scratch --------------------------------------------------
        [[nodiscard]] NodeState& State() const;
        [[nodiscard]] std::string Property(std::string_view key, std::string_view fallback = {}) const;
        [[nodiscard]] sizet GetEntryPin() const
        {
            return static_cast<sizet>(m_EntryPin < 0 ? 0 : m_EntryPin);
        }
        [[nodiscard]] i32 GetNodeIndex() const
        {
            return m_NodeIndex;
        }
        [[nodiscard]] i32 GetGraphIndex() const
        {
            return m_GraphIndex;
        }

        //-- Environment -----------------------------------------------------------
        [[nodiscard]] Scene* GetScene() const
        {
            return m_Runtime.m_Scene;
        }
        [[nodiscard]] f32 GetDeltaTime() const
        {
            return m_Runtime.m_DeltaTime;
        }
        [[nodiscard]] UUID GetEntityID() const;
        [[nodiscard]] VisualScriptInstance& GetInstance() const
        {
            return m_Instance;
        }
        [[nodiscard]] RuntimeContext& GetRuntime() const
        {
            return m_Runtime;
        }
        [[nodiscard]] const CompiledNode& GetCompiledNode() const;

        /// The payload / other-entity carried by the event currently being
        /// dispatched. Read by the event entry nodes to fill their data outputs.
        [[nodiscard]] PinValue GetCurrentEventPayload() const;
        [[nodiscard]] UUID GetCurrentEventOther() const;

        //-- Variables -------------------------------------------------------------
        [[nodiscard]] PinValue GetVariable(std::string_view name, bool* outFound = nullptr) const;
        bool SetVariable(std::string_view name, PinValue value) const;

        //-- Side channels ---------------------------------------------------------
        void Log(std::string message) const;
        void Emit(EmittedEvent event) const;
        void Error(std::string message) const;
        /// Deterministic uniform float in [0,1). Never rand().
        [[nodiscard]] f32 NextRandom() const;

        //-- Function calls --------------------------------------------------------
        /// Runs the function this Function.Call node targets, passing the call
        /// node's data inputs and copying the Return node's inputs back into the
        /// call node's outputs. Returns false when the call was refused
        /// (unresolved target, or re-entry — recursion is not supported).
        bool CallFunction() const;

        /// Function.Return only: hand `values` back to the in-flight call.
        void PublishReturnValues(const std::vector<PinValue>& values) const;

        /// Set by the VM before invoking a node body — the exec-descent depth
        /// this node sits at, so Trigger can bound recursion.
        void SetDepth(u32 depth)
        {
            m_Depth = depth;
        }

      private:
        VisualScriptInstance& m_Instance;
        RuntimeContext& m_Runtime;
        i32 m_GraphIndex;
        i32 m_NodeIndex;
        i32 m_EntryPin;
        u32 m_Depth = 0;
    };

    //==============================================================================
    /// Editor-debugger state carried on an instance. Inert (one bool test per
    /// node execution) until the panel turns tracing on for the entity it is
    /// looking at, so a shipping runtime pays essentially nothing for it.
    struct DebugState
    {
        /// Key for a node across every graph in the plan: graph index in the high
        /// 32 bits, the authored NodeId in the low 32. One flat set beats a
        /// per-graph map for the sizes involved.
        [[nodiscard]] static constexpr u64 MakeKey(i32 graphIndex, NodeId node)
        {
            return (static_cast<u64>(static_cast<u32>(graphIndex)) << 32) | static_cast<u64>(node);
        }

        /// Records which nodes ran, in order, so the canvas can fade a highlight
        /// out behind the execution. Only populated while m_TraceEnabled.
        std::unordered_map<u64, u64> m_ExecutionOrder;
        u64 m_ExecutionCounter = 0;
        bool m_TraceEnabled = false;

        std::unordered_set<u64> m_Breakpoints;
        /// Set when a breakpoint stopped the graph. The node was NOT executed —
        /// breaking happens before the body runs, which is what makes the pin
        /// values on screen the ones the node is about to consume.
        bool m_Paused = false;
        u64 m_PausedAt = 0;
        /// One tick of execution with breakpoints suppressed, then pause again.
        /// Node-granular stepping is deliberately absent: exec descent has no
        /// continuation to resume from, so it would mean re-running the tick and
        /// repeating every side effect before the breakpoint. See
        /// docs/agent-rules/visual-script-vm.md.
        bool m_StepOneTick = false;
    };

    //==============================================================================
    /// One entity's live copy of a compiled graph: its variable values, per-node
    /// scratch, memo stamps, and pending latent waits.
    class VisualScriptInstance
    {
      public:
        VisualScriptInstance() = default;
        VisualScriptInstance(Ref<VisualScriptPlan> plan, UUID owner);

        [[nodiscard]] bool IsValid() const
        {
            return m_Plan != nullptr;
        }
        [[nodiscard]] const VisualScriptPlan* GetPlan() const
        {
            return m_Plan.Raw();
        }
        [[nodiscard]] UUID GetOwner() const
        {
            return m_Owner;
        }
        void SetOwner(UUID owner)
        {
            m_Owner = owner;
        }

        //-- Blackboard ------------------------------------------------------------
        [[nodiscard]] PinValue GetVariable(std::string_view name, bool* outFound = nullptr) const;
        bool SetVariable(std::string_view name, PinValue value);
        [[nodiscard]] const std::vector<PinValue>& GetVariableValues() const
        {
            return m_VariableValues;
        }
        /// Applies the component's authored per-entity overrides. Non-finite
        /// values are rejected (the graph keeps the asset default) and reported.
        void ApplyVariableOverrides(const std::map<std::string, PinValue>& overrides);

        //-- Lifecycle -------------------------------------------------------------
        /// Fires Event.OnBeginPlay. Safe to call twice — the second is a no-op.
        void BeginPlay(RuntimeContext& runtime);
        /// Advances latent waits, then fires Event.OnUpdate.
        void Tick(RuntimeContext& runtime);
        /// Fires Event.OnEndPlay and drops every pending latent wait.
        void EndPlay(RuntimeContext& runtime);
        [[nodiscard]] bool HasBegunPlay() const
        {
            return m_BegunPlay;
        }

        /// Delivers a trigger: resumes any latent wait on it, then fires every
        /// matching entry node. Returns the number of entry points that ran.
        u32 DispatchEvent(const IncomingEvent& event, RuntimeContext& runtime);

        //-- Diagnostics -----------------------------------------------------------
        [[nodiscard]] u32 GetNodesExecutedThisTick() const
        {
            return m_NodesExecuted;
        }
        [[nodiscard]] bool DidExceedBudget() const
        {
            return m_BudgetExceeded;
        }
        [[nodiscard]] sizet GetPendingLatentCount() const
        {
            return m_PendingLatents.size();
        }
        [[nodiscard]] const std::vector<std::string>& GetErrors() const
        {
            return m_Errors;
        }
        void ClearErrors()
        {
            m_Errors.clear();
        }
        /// The last value written to a node's output pin. Test-facing, and what
        /// the editor debugger's pin-value watch reads.
        [[nodiscard]] PinValue PeekOutput(i32 graphIndex, NodeId nodeId, std::string_view pinName) const;

        //-- Editor debugger -------------------------------------------------------
        [[nodiscard]] DebugState& Debug()
        {
            return m_Debug;
        }
        [[nodiscard]] const DebugState& Debug() const
        {
            return m_Debug;
        }
        /// Clears the pause and lets the graph run again from the next tick.
        void DebugResume()
        {
            m_Debug.m_Paused = false;
            m_Debug.m_PausedAt = 0;
        }
        /// Runs exactly one more tick with breakpoints suppressed, then pauses.
        void DebugStepOneTick()
        {
            m_Debug.m_StepOneTick = true;
            m_Debug.m_Paused = false;
        }

      private:
        friend class NodeContext;

        struct GraphStorage
        {
            std::vector<PinValue> m_Values;
            std::vector<NodeState> m_States;
            std::vector<u64> m_PureStamp;
            /// Re-entry guard: recursion into the same function graph is refused
            /// rather than silently sharing one set of value slots.
            bool m_Running = false;
        };

        struct PendingLatent
        {
            i32 m_Graph = 0;
            i32 m_Node = -1;
            i32 m_ResumePin = -1;
            f32 m_Remaining = 0.0f;
            std::string m_EventName;
        };

        //-- Execution core --------------------------------------------------------
        // Not const: a pure node's first pull in an exec step RUNS it, which
        // writes its output slots and bumps its memo stamp.
        void ExecuteFrom(i32 graphIndex, i32 nodeIndex, i32 entryPin, RuntimeContext& runtime, u32 depth);
        [[nodiscard]] PinValue EvaluateInput(i32 graphIndex, i32 nodeIndex, sizet pin, RuntimeContext& runtime);
        [[nodiscard]] PinValue EvaluateOutput(i32 graphIndex, i32 nodeIndex, sizet pin, RuntimeContext& runtime);
        void BeginTickBookkeeping();
        void FireEntries(const std::string& key, const PinValue& payload, UUID otherEntity, RuntimeContext& runtime);
        void AdvanceLatents(RuntimeContext& runtime);
        /// Writes an arriving event's payload onto a resumed latent node's own
        /// "Payload" output pin, if it declares one. Resuming does not re-run the
        /// node body, so this is the only chance to publish it.
        void PublishResumePayload(i32 graphIndex, i32 nodeIndex, const PinValue& payload);
        [[nodiscard]] const CompiledGraph& GraphAt(i32 index) const;

        [[nodiscard]] bool ConsumeBudget();
        void ReportError(std::string message);

        Ref<VisualScriptPlan> m_Plan;
        UUID m_Owner{ 0 };
        std::vector<PinValue> m_VariableValues;
        /// Index 0 is the event graph; 1..N mirror m_Plan->GetFunctions().
        std::vector<GraphStorage> m_Storage;
        std::vector<PendingLatent> m_PendingLatents;
        /// Bumped on every exec-node execution. A pure node re-evaluates only
        /// when its stamp is older, so a diamond-shaped pure sub-graph is
        /// computed once per exec step instead of once per edge.
        u64 m_EvalStamp = 1;
        /// Recursion depth of the pure-pull evaluator, bounded the same way exec
        /// descent is. The compiler rejects pure cycles, so this is a backstop.
        u32 m_EvalDepth = 0;
        /// One entry per in-flight Function.Call, holding that call's results
        /// until the Return node fills them in. A stack because f may call g;
        /// re-entering the SAME function is refused (see CallFunction).
        std::vector<std::vector<PinValue>> m_ReturnStack;
        u32 m_NodesExecuted = 0;
        u32 m_Budget = 0;
        bool m_BudgetExceeded = false;
        bool m_BegunPlay = false;
        /// The event currently being dispatched, so an event node can publish
        /// its payload / other-entity outputs without threading them through
        /// every call.
        PinValue m_CurrentEventPayload{};
        UUID m_CurrentEventOther{ 0 };
        std::vector<std::string> m_Errors;
        DebugState m_Debug;

        /// Depth cap for Trigger recursion. Exec descent is genuine C++
        /// recursion (that is what makes Sequence and the loop nodes trivial),
        /// so a deeply chained graph must hit a bounded error rather than a
        /// stack overflow.
        static constexpr u32 kMaxExecDepth = 128;
        /// Cap on distinct errors kept per instance, so a graph erroring every
        /// node every tick cannot grow unboundedly.
        static constexpr sizet kMaxErrors = 32;
    };

} // namespace OloEngine::VisualScript
