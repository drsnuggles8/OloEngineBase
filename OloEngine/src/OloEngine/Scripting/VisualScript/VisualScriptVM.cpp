#include "OloEnginePCH.h"
#include "VisualScriptVM.h"

#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <unordered_set>

namespace OloEngine::VisualScript
{
    namespace
    {
        // xorshift64*: deterministic, seeded per scene, and independent of the
        // C library's rand() state. The Random nodes must reproduce under a
        // replay (issue #452's seeded game-thread stream), which rand() cannot.
        f32 NextUniform(u64& state)
        {
            if (state == 0)
            {
                state = 0x9E3779B97F4A7C15ull;
            }
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            const u64 mixed = state * 0x2545F4914F6CDD1Dull;
            // 24 bits of mantissa: exactly representable in f32, never rounds to 1.0f.
            return static_cast<f32>((mixed >> 40) & 0xFFFFFFull) / 16777216.0f;
        }

        i32 FindPinIndex(const std::vector<PinDescriptor>& pins, std::string_view name, PinDirection direction)
        {
            for (sizet i = 0; i < pins.size(); ++i)
            {
                if (pins[i].m_Direction == direction && pins[i].m_Name == name)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }
    } // namespace

    //==============================================================================
    // Compilation
    //==============================================================================

    std::string VisualScriptPlan::MakeEventKey(std::string_view scope, std::string_view name)
    {
        std::string key(scope);
        key += ':';
        key += name;
        return key;
    }

    i32 VisualScriptPlan::FindVariableIndex(std::string_view name) const
    {
        const auto it = m_VariableIndex.find(std::string(name));
        return it == m_VariableIndex.end() ? -1 : it->second;
    }

    bool VisualScriptPlan::HasEventEntry(std::string_view key) const
    {
        const auto it = m_EventGraph.m_EventEntries.find(std::string(key));
        return it != m_EventGraph.m_EventEntries.end() && !it->second.empty();
    }

    namespace
    {
        struct GraphCompiler
        {
            const VisualScriptAsset& m_Asset;
            std::vector<CompileDiagnostic>& m_Errors;
            bool m_Failed = false;

            void Fail(const std::string& graph, NodeId node, std::string message)
            {
                m_Failed = true;
                m_Errors.push_back({ graph, node, std::move(message) });
            }

            // Compile one graph's nodes + wires. Two passes: nodes first (so link
            // resolution can look every endpoint up), then links.
            CompiledGraph Compile(const VisualScriptGraph& source)
            {
                const NodeRegistry& registry = NodeRegistry::Get();

                CompiledGraph out;
                out.m_Name = source.m_Name;
                out.m_Inputs = source.m_Inputs;
                out.m_Outputs = source.m_Outputs;
                out.m_Nodes.reserve(source.m_Nodes.size());

                std::unordered_map<NodeId, i32> nodeIndexById;
                nodeIndexById.reserve(source.m_Nodes.size());

                for (const VisualScriptNode& authored : source.m_Nodes)
                {
                    if (nodeIndexById.contains(authored.m_Id))
                    {
                        Fail(source.m_Name, authored.m_Id, "Duplicate node id");
                        continue;
                    }

                    const NodeTypeDescriptor* type = registry.Find(authored.m_TypeName);
                    if (type == nullptr)
                    {
                        Fail(source.m_Name, authored.m_Id, "Unknown node type '" + authored.m_TypeName + "'");
                        continue;
                    }

                    CompiledNode node;
                    node.m_SourceId = authored.m_Id;
                    node.m_Type = type;
                    node.m_Properties = authored.m_Properties;
                    node.m_Pins = type->m_ResolvePins ? type->m_ResolvePins(authored, m_Asset) : type->m_Pins;
                    node.m_PinInfo.resize(node.m_Pins.size());

                    for (sizet pin = 0; pin < node.m_Pins.size(); ++pin)
                    {
                        const PinDescriptor& descriptor = node.m_Pins[pin];
                        CompiledPin& info = node.m_PinInfo[pin];

                        if (IsDataPin(descriptor.m_Type) && descriptor.m_Direction == PinDirection::Output)
                        {
                            info.m_ValueSlot = static_cast<i32>(out.m_ValueSlotCount++);
                        }
                        if (IsDataPin(descriptor.m_Type) && descriptor.m_Direction == PinDirection::Input)
                        {
                            const auto override = authored.m_PinDefaults.find(descriptor.m_Name);
                            PinValue literal = override != authored.m_PinDefaults.end()
                                                   ? override->second.ConvertTo(descriptor.m_Type)
                                                   : descriptor.m_DefaultValue;
                            // A NaN literal poisons every downstream node and makes
                            // Branch take neither obvious side; clamp at the door.
                            if (literal.SanitizeNonFinite())
                            {
                                OLO_CORE_WARN("[VisualScript] Non-finite literal on {}::{} pin '{}' clamped to 0",
                                              source.m_Name, authored.m_TypeName, descriptor.m_Name);
                            }
                            info.m_Literal = std::move(literal);
                        }
                    }

                    if (HasFlag(type->m_Flags, NodeFlags::Event))
                    {
                        out.m_EventEntries[EventKeyFor(authored, *type)].push_back(static_cast<i32>(out.m_Nodes.size()));
                    }
                    if (authored.m_TypeName == NodeTypes::kFunctionEntry)
                    {
                        out.m_EntryNode = static_cast<i32>(out.m_Nodes.size());
                    }
                    else if (authored.m_TypeName == NodeTypes::kFunctionReturn)
                    {
                        out.m_ReturnNode = static_cast<i32>(out.m_Nodes.size());
                    }
                    else if (authored.m_TypeName == NodeTypes::kFunctionCall)
                    {
                        const std::string& target = authored.GetProperty(NodeProps::kFunctionName, s_Empty);
                        i32 functionIndex = -1;
                        for (sizet f = 0; f < m_Asset.m_Functions.size(); ++f)
                        {
                            if (m_Asset.m_Functions[f].m_Name == target)
                            {
                                functionIndex = static_cast<i32>(f);
                                break;
                            }
                        }
                        if (functionIndex < 0)
                        {
                            Fail(source.m_Name, authored.m_Id, "Function.Call targets unknown function '" + target + "'");
                        }
                        node.m_FunctionIndex = functionIndex;
                    }

                    nodeIndexById.emplace(authored.m_Id, static_cast<i32>(out.m_Nodes.size()));
                    out.m_Nodes.push_back(std::move(node));
                }

                ResolveLinks(source, out, nodeIndexById);
                DetectPureCycles(source, out);
                return out;
            }

            static std::string EventKeyFor(const VisualScriptNode& authored, const NodeTypeDescriptor& type)
            {
                if (authored.m_TypeName == NodeTypes::kCustomEvent)
                {
                    return VisualScriptPlan::MakeEventKey("Custom", authored.GetProperty(NodeProps::kEventName, s_Empty));
                }
                if (authored.m_TypeName == NodeTypes::kOnGameplayEvent)
                {
                    return VisualScriptPlan::MakeEventKey("Bus", authored.GetProperty(NodeProps::kEventName, s_Empty));
                }
                return VisualScriptPlan::MakeEventKey("Engine", type.m_TypeName);
            }

            void ResolveLinks(const VisualScriptGraph& source, CompiledGraph& out, const std::unordered_map<NodeId, i32>& nodeIndexById)
            {
                for (const VisualScriptLink& link : source.m_Links)
                {
                    const auto sourceIt = nodeIndexById.find(link.m_SourceNode);
                    const auto targetIt = nodeIndexById.find(link.m_TargetNode);
                    if (sourceIt == nodeIndexById.end() || targetIt == nodeIndexById.end())
                    {
                        Fail(source.m_Name, link.m_SourceNode, "Link references a node that does not exist");
                        continue;
                    }

                    CompiledNode& sourceNode = out.m_Nodes[static_cast<sizet>(sourceIt->second)];
                    CompiledNode& targetNode = out.m_Nodes[static_cast<sizet>(targetIt->second)];

                    const i32 sourcePin = FindPinIndex(sourceNode.m_Pins, link.m_SourcePin, PinDirection::Output);
                    const i32 targetPin = FindPinIndex(targetNode.m_Pins, link.m_TargetPin, PinDirection::Input);
                    if (sourcePin < 0 || targetPin < 0)
                    {
                        Fail(source.m_Name, link.m_SourceNode,
                             "Dangling link endpoint: '" + link.m_SourcePin + "' -> '" + link.m_TargetPin + "'");
                        continue;
                    }

                    const PinType sourceType = sourceNode.m_Pins[static_cast<sizet>(sourcePin)].m_Type;
                    const PinType targetType = targetNode.m_Pins[static_cast<sizet>(targetPin)].m_Type;
                    if (CheckLinkCompatibility(sourceType, targetType) == LinkCompatibility::Incompatible)
                    {
                        Fail(source.m_Name, link.m_SourceNode,
                             std::string("Incompatible link ") + PinTypeToString(sourceType) + " -> " + PinTypeToString(targetType));
                        continue;
                    }

                    if (IsExecPin(sourceType))
                    {
                        sourceNode.m_PinInfo[static_cast<sizet>(sourcePin)].m_ExecTargets.push_back({ targetIt->second, targetPin });
                    }
                    else
                    {
                        CompiledPin& info = targetNode.m_PinInfo[static_cast<sizet>(targetPin)];
                        if (info.m_SourceNode >= 0)
                        {
                            // Two writers into one input has no defined winner —
                            // reject rather than let load order decide.
                            Fail(source.m_Name, link.m_TargetNode,
                                 "Input pin '" + link.m_TargetPin + "' has more than one incoming link");
                            continue;
                        }
                        info.m_SourceNode = sourceIt->second;
                        info.m_SourcePin = sourcePin;
                    }
                }
            }

            // Only PURE nodes are pull-evaluated recursively, so only a cycle
            // among pure nodes can spin forever. An exec node's output is read
            // from its stored value slot, which terminates by construction — a
            // feedback wire between two exec nodes is legal and useful.
            void DetectPureCycles(const VisualScriptGraph& source, const CompiledGraph& out)
            {
                enum class Mark : u8
                {
                    Unvisited,
                    InProgress,
                    Done
                };
                std::vector<Mark> marks(out.m_Nodes.size(), Mark::Unvisited);

                const auto isPure = [&out](i32 index)
                {
                    return index >= 0 && out.m_Nodes[static_cast<sizet>(index)].m_Type != nullptr && HasFlag(out.m_Nodes[static_cast<sizet>(index)].m_Type->m_Flags, NodeFlags::Pure);
                };

                // Iterative DFS: a graph deep enough to overflow the C++ stack
                // here would be authored, not generated, but the compiler must
                // still refuse it cleanly rather than crash the editor.
                std::vector<i32> stack;
                for (sizet root = 0; root < out.m_Nodes.size(); ++root)
                {
                    if (!isPure(static_cast<i32>(root)) || marks[root] != Mark::Unvisited)
                    {
                        continue;
                    }
                    stack.push_back(static_cast<i32>(root));
                    while (!stack.empty())
                    {
                        const i32 current = stack.back();
                        Mark& mark = marks[static_cast<sizet>(current)];
                        if (mark == Mark::Done)
                        {
                            stack.pop_back();
                            continue;
                        }
                        if (mark == Mark::InProgress)
                        {
                            mark = Mark::Done;
                            stack.pop_back();
                            continue;
                        }
                        mark = Mark::InProgress;

                        const CompiledNode& node = out.m_Nodes[static_cast<sizet>(current)];
                        for (const CompiledPin& info : node.m_PinInfo)
                        {
                            if (!isPure(info.m_SourceNode))
                            {
                                continue;
                            }
                            const Mark sourceMark = marks[static_cast<sizet>(info.m_SourceNode)];
                            if (sourceMark == Mark::InProgress)
                            {
                                Fail(source.m_Name, node.m_SourceId, "Cycle among pure (data-only) nodes");
                                return;
                            }
                            if (sourceMark == Mark::Unvisited)
                            {
                                stack.push_back(info.m_SourceNode);
                            }
                        }
                    }
                }
            }

            static inline const std::string s_Empty{};
        };
    } // namespace

    Ref<VisualScriptPlan> VisualScriptPlan::Compile(const VisualScriptAsset& asset, std::vector<CompileDiagnostic>& outErrors)
    {
        NodeRegistry::EnsureStandardLibrary();

        Ref<VisualScriptPlan> plan = Ref<VisualScriptPlan>::Create();
        plan->m_NodeBudgetPerTick = asset.m_NodeBudgetPerTick == 0 ? 10000 : asset.m_NodeBudgetPerTick;
        plan->m_Variables = asset.m_Variables;
        for (sizet i = 0; i < plan->m_Variables.size(); ++i)
        {
            PinValue& value = plan->m_Variables[i].m_DefaultValue;
            value = value.ConvertTo(plan->m_Variables[i].m_Type);
            (void)value.SanitizeNonFinite();
            plan->m_VariableIndex.emplace(plan->m_Variables[i].m_Name, static_cast<i32>(i));
        }
        if (plan->m_VariableIndex.size() != plan->m_Variables.size())
        {
            outErrors.push_back({ "", kInvalidNodeId, "Duplicate blackboard variable name" });
            return nullptr;
        }

        GraphCompiler compiler{ asset, outErrors };
        plan->m_EventGraph = compiler.Compile(asset.m_EventGraph);
        plan->m_Functions.reserve(asset.m_Functions.size());
        for (const VisualScriptGraph& function : asset.m_Functions)
        {
            plan->m_Functions.push_back(compiler.Compile(function));
        }

        if (compiler.m_Failed)
        {
            return nullptr;
        }
        return plan;
    }

    //==============================================================================
    // Instance
    //==============================================================================

    VisualScriptInstance::VisualScriptInstance(Ref<VisualScriptPlan> plan, UUID owner)
        : m_Plan(std::move(plan)), m_Owner(owner)
    {
        if (!m_Plan)
        {
            return;
        }

        m_VariableValues.reserve(m_Plan->GetVariables().size());
        for (const VisualScriptVariable& variable : m_Plan->GetVariables())
        {
            m_VariableValues.push_back(variable.m_DefaultValue);
        }

        m_Storage.resize(1 + m_Plan->GetFunctions().size());
        const auto sizeStorage = [](GraphStorage& storage, const CompiledGraph& graph)
        {
            storage.m_Values.assign(graph.m_ValueSlotCount, PinValue{});
            storage.m_States.assign(graph.m_Nodes.size(), NodeState{});
            storage.m_PureStamp.assign(graph.m_Nodes.size(), 0);
        };
        sizeStorage(m_Storage[0], m_Plan->GetEventGraph());
        for (sizet i = 0; i < m_Plan->GetFunctions().size(); ++i)
        {
            sizeStorage(m_Storage[i + 1], m_Plan->GetFunctions()[i]);
        }

        m_Budget = m_Plan->GetNodeBudgetPerTick();
    }

    const CompiledGraph& VisualScriptInstance::GraphAt(i32 index) const
    {
        return index <= 0 ? m_Plan->GetEventGraph() : m_Plan->GetFunctions()[static_cast<sizet>(index - 1)];
    }

    PinValue VisualScriptInstance::GetVariable(std::string_view name, bool* outFound) const
    {
        if (!m_Plan)
        {
            if (outFound != nullptr)
            {
                *outFound = false;
            }
            return {};
        }
        const i32 index = m_Plan->FindVariableIndex(name);
        if (outFound != nullptr)
        {
            *outFound = index >= 0;
        }
        return index < 0 ? PinValue{} : m_VariableValues[static_cast<sizet>(index)];
    }

    bool VisualScriptInstance::SetVariable(std::string_view name, PinValue value)
    {
        if (!m_Plan)
        {
            return false;
        }
        const i32 index = m_Plan->FindVariableIndex(name);
        if (index < 0)
        {
            return false;
        }
        PinValue converted = value.ConvertTo(m_Plan->GetVariables()[static_cast<sizet>(index)].m_Type);
        if (converted.SanitizeNonFinite())
        {
            ReportError("Non-finite value written to variable '" + std::string(name) + "'; clamped to 0");
        }
        m_VariableValues[static_cast<sizet>(index)] = std::move(converted);
        return true;
    }

    void VisualScriptInstance::ApplyVariableOverrides(const std::map<std::string, PinValue>& overrides)
    {
        for (const auto& [name, value] : overrides)
        {
            if (!SetVariable(name, value))
            {
                // Not an error: an override left behind after the author renamed
                // or deleted the variable. Warn once so it is fixable, and keep
                // running on the asset default.
                OLO_CORE_WARN("[VisualScript] Entity {} overrides unknown variable '{}'", static_cast<u64>(m_Owner), name);
            }
        }
    }

    bool VisualScriptInstance::ConsumeBudget()
    {
        if (m_Budget == 0)
        {
            if (!m_BudgetExceeded)
            {
                m_BudgetExceeded = true;
                ReportError("Per-tick node budget exhausted; graph halted for this tick");
                OLO_CORE_WARN("[VisualScript] Entity {} exhausted its {}-node per-tick budget; halting the graph for this tick",
                              static_cast<u64>(m_Owner), m_Plan ? m_Plan->GetNodeBudgetPerTick() : 0);
            }
            return false;
        }
        --m_Budget;
        ++m_NodesExecuted;
        return true;
    }

    void VisualScriptInstance::ReportError(std::string message)
    {
        if (m_Errors.size() >= kMaxErrors)
        {
            return;
        }
        m_Errors.push_back(std::move(message));
    }

    void VisualScriptInstance::BeginTickBookkeeping()
    {
        m_Budget = m_Plan ? m_Plan->GetNodeBudgetPerTick() : 0;
        m_NodesExecuted = 0;
        m_BudgetExceeded = false;
    }

    void VisualScriptInstance::BeginPlay(RuntimeContext& runtime)
    {
        if (!m_Plan || m_BegunPlay)
        {
            return;
        }
        m_BegunPlay = true;
        if (m_Debug.m_Paused)
        {
            return;
        }
        BeginTickBookkeeping();
        FireEntries(VisualScriptPlan::MakeEventKey("Engine", NodeTypes::kOnBeginPlay), PinValue{}, UUID(0), runtime);
    }

    void VisualScriptInstance::Tick(RuntimeContext& runtime)
    {
        if (!m_Plan || !m_BegunPlay)
        {
            return;
        }
        // Paused at a breakpoint: the graph is frozen while the rest of the scene
        // keeps running, so the author can inspect it against a live world.
        if (m_Debug.m_Paused)
        {
            return;
        }
        BeginTickBookkeeping();
        // Latents first: a Delay that expires this frame should run its
        // continuation before this frame's OnUpdate, so a graph that alternates
        // between the two sees them in authored order rather than one tick late.
        AdvanceLatents(runtime);
        FireEntries(VisualScriptPlan::MakeEventKey("Engine", NodeTypes::kOnUpdate), PinValue{}, UUID(0), runtime);

        // A step tick is exactly one tick with breakpoints suppressed; re-arm the
        // pause at the end of it so the next tick stops again.
        if (m_Debug.m_StepOneTick)
        {
            m_Debug.m_StepOneTick = false;
            m_Debug.m_Paused = true;
        }
    }

    void VisualScriptInstance::EndPlay(RuntimeContext& runtime)
    {
        if (!m_Plan || !m_BegunPlay)
        {
            return;
        }
        BeginTickBookkeeping();
        FireEntries(VisualScriptPlan::MakeEventKey("Engine", NodeTypes::kOnEndPlay), PinValue{}, UUID(0), runtime);
        m_PendingLatents.clear();
        m_BegunPlay = false;
    }

    void VisualScriptInstance::PublishResumePayload(i32 graphIndex, i32 nodeIndex, const PinValue& payload)
    {
        const CompiledGraph& graph = GraphAt(graphIndex);
        if (nodeIndex < 0 || static_cast<sizet>(nodeIndex) >= graph.m_Nodes.size())
        {
            return;
        }
        const CompiledNode& node = graph.m_Nodes[static_cast<sizet>(nodeIndex)];
        const i32 pin = FindPinIndex(node.m_Pins, "Payload", PinDirection::Output);
        if (pin < 0)
        {
            return;
        }
        const i32 slot = node.m_PinInfo[static_cast<sizet>(pin)].m_ValueSlot;
        if (slot < 0)
        {
            return;
        }
        PinValue converted = payload.ConvertTo(node.m_Pins[static_cast<sizet>(pin)].m_Type);
        (void)converted.SanitizeNonFinite();
        m_Storage[static_cast<sizet>(graphIndex)].m_Values[static_cast<sizet>(slot)] = std::move(converted);
    }

    u32 VisualScriptInstance::DispatchEvent(const IncomingEvent& event, RuntimeContext& runtime)
    {
        if (!m_Plan || !m_BegunPlay || m_Debug.m_Paused)
        {
            return 0;
        }

        u32 ran = 0;

        // A parked branch must not be lost because this tick's budget happened to
        // be spent before the event arrived: ExecuteFrom silently bails when the
        // budget is gone, and the record would already have been erased. Leaving
        // it parked costs one more tick and keeps the wait alive.
        if (m_Budget == 0)
        {
            return 0;
        }

        // Resume every latent wait parked on this key. Snapshot first: a resumed
        // branch may suspend again, appending to m_PendingLatents mid-loop.
        std::vector<PendingLatent> resumed;
        for (sizet i = m_PendingLatents.size(); i > 0; --i)
        {
            PendingLatent& pending = m_PendingLatents[i - 1];
            if (pending.m_EventName == event.m_Key)
            {
                resumed.push_back(pending);
                m_PendingLatents.erase(m_PendingLatents.begin() + static_cast<std::ptrdiff_t>(i - 1));
            }
        }
        for (const PendingLatent& pending : resumed)
        {
            m_CurrentEventPayload = event.m_Payload;
            m_CurrentEventOther = event.m_OtherEntity;
            const CompiledGraph& graph = GraphAt(pending.m_Graph);
            const CompiledNode& node = graph.m_Nodes[static_cast<sizet>(pending.m_Node)];

            // Publish the arriving payload onto the waiting node's own output pin.
            // Resuming jumps straight to the resume pin's targets and never re-runs
            // the node body, so without this the documented "Payload" output of
            // Flow.WaitForEvent would read as an empty value forever.
            PublishResumePayload(pending.m_Graph, pending.m_Node, event.m_Payload);

            for (const ExecTarget& target : node.m_PinInfo[static_cast<sizet>(pending.m_ResumePin)].m_ExecTargets)
            {
                ExecuteFrom(pending.m_Graph, target.m_Node, target.m_Pin, runtime, 0);
            }
            ++ran;
        }

        const auto& entries = m_Plan->GetEventGraph().m_EventEntries;
        if (const auto it = entries.find(event.m_Key); it != entries.end())
        {
            FireEntries(event.m_Key, event.m_Payload, event.m_OtherEntity, runtime);
            ran += static_cast<u32>(it->second.size());
        }
        return ran;
    }

    void VisualScriptInstance::FireEntries(const std::string& key, const PinValue& payload, UUID otherEntity, RuntimeContext& runtime)
    {
        const auto& entries = m_Plan->GetEventGraph().m_EventEntries;
        const auto it = entries.find(key);
        if (it == entries.end())
        {
            return;
        }
        m_CurrentEventPayload = payload;
        m_CurrentEventOther = otherEntity;
        for (const i32 nodeIndex : it->second)
        {
            ExecuteFrom(0, nodeIndex, -1, runtime, 0);
        }
    }

    void VisualScriptInstance::AdvanceLatents(RuntimeContext& runtime)
    {
        if (m_PendingLatents.empty())
        {
            return;
        }

        std::vector<PendingLatent> ready;
        for (sizet i = m_PendingLatents.size(); i > 0; --i)
        {
            PendingLatent& pending = m_PendingLatents[i - 1];
            if (!pending.m_EventName.empty())
            {
                continue; // event waits are resumed by DispatchEvent, not by time
            }
            pending.m_Remaining -= runtime.m_DeltaTime;
            if (pending.m_Remaining <= 0.0f)
            {
                ready.push_back(pending);
                m_PendingLatents.erase(m_PendingLatents.begin() + static_cast<std::ptrdiff_t>(i - 1));
            }
        }

        // Reverse so the resume order matches suspend order — the scan above
        // walks backwards to make erasure cheap, which would otherwise make two
        // Delays that expire on the same frame fire in the wrong order.
        std::ranges::reverse(ready);
        for (const PendingLatent& pending : ready)
        {
            const CompiledGraph& graph = GraphAt(pending.m_Graph);
            const CompiledNode& node = graph.m_Nodes[static_cast<sizet>(pending.m_Node)];
            for (const ExecTarget& target : node.m_PinInfo[static_cast<sizet>(pending.m_ResumePin)].m_ExecTargets)
            {
                ExecuteFrom(pending.m_Graph, target.m_Node, target.m_Pin, runtime, 0);
            }
        }
    }

    void VisualScriptInstance::ExecuteFrom(i32 graphIndex, i32 nodeIndex, i32 entryPin, RuntimeContext& runtime, u32 depth)
    {
        if (depth > kMaxExecDepth)
        {
            ReportError("Exec chain deeper than " + std::to_string(kMaxExecDepth) + " nodes; halted");
            return;
        }
        if (!ConsumeBudget())
        {
            return;
        }

        const CompiledGraph& graph = GraphAt(graphIndex);
        if (nodeIndex < 0 || static_cast<sizet>(nodeIndex) >= graph.m_Nodes.size())
        {
            return;
        }
        const CompiledNode& node = graph.m_Nodes[static_cast<sizet>(nodeIndex)];
        if (node.m_Type == nullptr || !node.m_Type->m_Execute)
        {
            return;
        }

        // ── Editor debugger (issue #634) ──────────────────────────────────────
        // Both checks are one bool test each when the panel is not watching this
        // entity, which is every entity in a shipping run.
        if (m_Debug.m_TraceEnabled || !m_Debug.m_Breakpoints.empty())
        {
            const u64 key = DebugState::MakeKey(graphIndex, node.m_SourceId);
            if (m_Debug.m_TraceEnabled)
            {
                m_Debug.m_ExecutionOrder[key] = ++m_Debug.m_ExecutionCounter;
            }
            // Break BEFORE running the body, and suppress breakpoints entirely
            // during a step tick. Stopping before the node is what makes the pin
            // values the canvas shows the ones this node is about to consume,
            // rather than the ones it just produced.
            if (!m_Debug.m_StepOneTick && m_Debug.m_Breakpoints.contains(key))
            {
                m_Debug.m_Paused = true;
                m_Debug.m_PausedAt = key;
                // Returning unwinds the whole exec descent — the rest of this
                // run is abandoned, exactly as the budget guard does.
                return;
            }
        }

        // A new exec step invalidates every memoized pure value: a pure node may
        // read a variable a previous node just wrote. Memoizing WITHIN the step
        // is what keeps a diamond-shaped pure sub-graph from being recomputed
        // once per edge.
        ++m_EvalStamp;

        NodeContext context(*this, graphIndex, nodeIndex, entryPin, runtime);
        context.SetDepth(depth);
        node.m_Type->m_Execute(context);
    }

    PinValue VisualScriptInstance::EvaluateOutput(i32 graphIndex, i32 nodeIndex, sizet pin, RuntimeContext& runtime)
    {
        const CompiledGraph& graph = GraphAt(graphIndex);
        if (nodeIndex < 0 || static_cast<sizet>(nodeIndex) >= graph.m_Nodes.size())
        {
            return {};
        }
        const CompiledNode& node = graph.m_Nodes[static_cast<sizet>(nodeIndex)];
        if (pin >= node.m_PinInfo.size())
        {
            return {};
        }

        if (node.m_Type != nullptr && HasFlag(node.m_Type->m_Flags, NodeFlags::Pure) && node.m_Type->m_Evaluate)
        {
            GraphStorage& storage = m_Storage[static_cast<sizet>(graphIndex)];
            if (storage.m_PureStamp[static_cast<sizet>(nodeIndex)] != m_EvalStamp)
            {
                if (m_EvalDepth > kMaxExecDepth)
                {
                    ReportError("Pure evaluation deeper than " + std::to_string(kMaxExecDepth) + " nodes; halted");
                    return {};
                }
                // Stamp BEFORE evaluating: a pure cycle the compiler somehow let
                // through then reads a stale value instead of recursing forever.
                storage.m_PureStamp[static_cast<sizet>(nodeIndex)] = m_EvalStamp;
                ++m_EvalDepth;
                NodeContext context(*this, graphIndex, nodeIndex, -1, runtime);
                node.m_Type->m_Evaluate(context);
                --m_EvalDepth;
            }
        }

        const i32 slot = node.m_PinInfo[pin].m_ValueSlot;
        if (slot < 0)
        {
            return {};
        }
        return m_Storage[static_cast<sizet>(graphIndex)].m_Values[static_cast<sizet>(slot)];
    }

    PinValue VisualScriptInstance::EvaluateInput(i32 graphIndex, i32 nodeIndex, sizet pin, RuntimeContext& runtime)
    {
        const CompiledGraph& graph = GraphAt(graphIndex);
        const CompiledNode& node = graph.m_Nodes[static_cast<sizet>(nodeIndex)];
        if (pin >= node.m_PinInfo.size())
        {
            return {};
        }
        const CompiledPin& info = node.m_PinInfo[pin];
        const PinType wanted = node.m_Pins[pin].m_Type;
        if (info.m_SourceNode < 0)
        {
            return info.m_Literal;
        }
        return EvaluateOutput(graphIndex, info.m_SourceNode, static_cast<sizet>(info.m_SourcePin), runtime).ConvertTo(wanted);
    }

    PinValue VisualScriptInstance::PeekOutput(i32 graphIndex, NodeId nodeId, std::string_view pinName) const
    {
        if (!m_Plan || graphIndex < 0 || static_cast<sizet>(graphIndex) >= m_Storage.size())
        {
            return {};
        }
        const CompiledGraph& graph = GraphAt(graphIndex);
        for (sizet i = 0; i < graph.m_Nodes.size(); ++i)
        {
            const CompiledNode& node = graph.m_Nodes[i];
            if (node.m_SourceId != nodeId)
            {
                continue;
            }
            const i32 pin = FindPinIndex(node.m_Pins, pinName, PinDirection::Output);
            if (pin < 0)
            {
                return {};
            }
            const i32 slot = node.m_PinInfo[static_cast<sizet>(pin)].m_ValueSlot;
            return slot < 0 ? PinValue{} : m_Storage[static_cast<sizet>(graphIndex)].m_Values[static_cast<sizet>(slot)];
        }
        return {};
    }

    //==============================================================================
    // NodeContext
    //==============================================================================

    const CompiledNode& NodeContext::GetCompiledNode() const
    {
        return m_Instance.GraphAt(m_GraphIndex).m_Nodes[static_cast<sizet>(m_NodeIndex)];
    }

    PinValue NodeContext::GetInput(sizet pin) const
    {
        return m_Instance.EvaluateInput(m_GraphIndex, m_NodeIndex, pin, m_Runtime);
    }

    bool NodeContext::GetInputBool(sizet pin) const
    {
        return GetInput(pin).AsBool();
    }

    i64 NodeContext::GetInputInt(sizet pin) const
    {
        return GetInput(pin).AsInt();
    }

    f32 NodeContext::GetInputFloat(sizet pin) const
    {
        return GetInput(pin).AsFloat();
    }

    glm::vec3 NodeContext::GetInputVec3(sizet pin) const
    {
        return GetInput(pin).AsVec3();
    }

    std::string NodeContext::GetInputString(sizet pin) const
    {
        return GetInput(pin).AsString();
    }

    UUID NodeContext::GetInputEntity(sizet pin) const
    {
        const PinValue value = GetInput(pin);
        // An unwired Entity pin means "me" — the overwhelmingly common case, and
        // the alternative (silently addressing entity 0) is a no-op that looks
        // like a broken node.
        const UUID id = value.AsEntity();
        return static_cast<u64>(id) == 0 ? m_Instance.GetOwner() : id;
    }

    bool NodeContext::IsInputConnected(sizet pin) const
    {
        const CompiledNode& node = GetCompiledNode();
        return pin < node.m_PinInfo.size() && node.m_PinInfo[pin].m_SourceNode >= 0;
    }

    void NodeContext::SetOutput(sizet pin, PinValue value) const
    {
        const CompiledNode& node = GetCompiledNode();
        if (pin >= node.m_PinInfo.size())
        {
            return;
        }
        const i32 slot = node.m_PinInfo[pin].m_ValueSlot;
        if (slot < 0)
        {
            return;
        }
        PinValue converted = value.ConvertTo(node.m_Pins[pin].m_Type);
        (void)converted.SanitizeNonFinite();
        m_Instance.m_Storage[static_cast<sizet>(m_GraphIndex)].m_Values[static_cast<sizet>(slot)] = std::move(converted);
    }

    void NodeContext::Trigger(sizet pin) const
    {
        const CompiledNode& node = GetCompiledNode();
        if (pin >= node.m_PinInfo.size())
        {
            return;
        }
        // Copy the target list: executing a target can, through a Function.Call,
        // reach code that recompiles nothing but DOES re-enter this graph, and a
        // reference into m_PinInfo must not outlive that.
        const std::vector<ExecTarget> targets = node.m_PinInfo[pin].m_ExecTargets;
        for (const ExecTarget& target : targets)
        {
            m_Instance.ExecuteFrom(m_GraphIndex, target.m_Node, target.m_Pin, m_Runtime, m_Depth + 1);
        }
    }

    void NodeContext::SuspendForSeconds(sizet resumePin, f32 seconds) const
    {
        if (!std::isfinite(seconds) || seconds < 0.0f)
        {
            seconds = 0.0f;
        }
        VisualScriptInstance::PendingLatent pending;
        pending.m_Graph = m_GraphIndex;
        pending.m_Node = m_NodeIndex;
        pending.m_ResumePin = static_cast<i32>(resumePin);
        pending.m_Remaining = seconds;
        m_Instance.m_PendingLatents.push_back(std::move(pending));
    }

    void NodeContext::SuspendForEvent(sizet resumePin, std::string eventName) const
    {
        VisualScriptInstance::PendingLatent pending;
        pending.m_Graph = m_GraphIndex;
        pending.m_Node = m_NodeIndex;
        pending.m_ResumePin = static_cast<i32>(resumePin);
        pending.m_EventName = std::move(eventName);
        m_Instance.m_PendingLatents.push_back(std::move(pending));
    }

    bool NodeContext::IsBudgetExhausted() const
    {
        return m_Instance.m_Budget == 0;
    }

    bool NodeContext::BeginIteration() const
    {
        ++m_Instance.m_EvalStamp;
        return m_Instance.ConsumeBudget();
    }

    NodeState& NodeContext::State() const
    {
        return m_Instance.m_Storage[static_cast<sizet>(m_GraphIndex)].m_States[static_cast<sizet>(m_NodeIndex)];
    }

    std::string NodeContext::Property(std::string_view key, std::string_view fallback) const
    {
        const CompiledNode& node = GetCompiledNode();
        const auto it = node.m_Properties.find(std::string(key));
        return it == node.m_Properties.end() ? std::string(fallback) : it->second;
    }

    UUID NodeContext::GetEntityID() const
    {
        return m_Instance.GetOwner();
    }

    PinValue NodeContext::GetVariable(std::string_view name, bool* outFound) const
    {
        return m_Instance.GetVariable(name, outFound);
    }

    bool NodeContext::SetVariable(std::string_view name, PinValue value) const
    {
        return m_Instance.SetVariable(name, std::move(value));
    }

    void NodeContext::Log(std::string message) const
    {
        if (m_Runtime.m_LogSink != nullptr)
        {
            m_Runtime.m_LogSink->push_back(message);
        }
        OLO_CORE_TRACE("[VisualScript] {}", message);
    }

    void NodeContext::Emit(EmittedEvent event) const
    {
        if (m_Runtime.m_EventOutbox == nullptr)
        {
            Error("Event emitted with no outbox attached; dropped");
            return;
        }
        m_Runtime.m_EventOutbox->push_back(std::move(event));
    }

    void NodeContext::Error(std::string message) const
    {
        const CompiledNode& node = GetCompiledNode();
        m_Instance.ReportError(node.m_Type->m_TypeName + " (node " + std::to_string(node.m_SourceId) + "): " + message);
    }

    f32 NodeContext::NextRandom() const
    {
        if (m_Runtime.m_RandomState == nullptr)
        {
            // No stream attached (a bare unit-test harness). Return the midpoint
            // rather than a nondeterministic value — a silently varying result is
            // worse than an obviously constant one.
            return 0.5f;
        }
        return NextUniform(*m_Runtime.m_RandomState);
    }

    PinValue NodeContext::GetCurrentEventPayload() const
    {
        return m_Instance.m_CurrentEventPayload;
    }

    UUID NodeContext::GetCurrentEventOther() const
    {
        return m_Instance.m_CurrentEventOther;
    }

    bool NodeContext::CallFunction() const
    {
        const CompiledNode& callNode = GetCompiledNode();
        const i32 functionIndex = callNode.m_FunctionIndex;
        if (functionIndex < 0)
        {
            Error("Function.Call has no resolved target");
            return false;
        }

        const i32 functionGraph = functionIndex + 1;
        VisualScriptInstance::GraphStorage& storage = m_Instance.m_Storage[static_cast<sizet>(functionGraph)];
        if (storage.m_Running)
        {
            // Recursion would need a per-call frame; the VM allocates one set of
            // value slots per function per instance. Refusing loudly beats
            // silently sharing slots between the outer and inner call.
            Error("Recursive Function.Call is not supported");
            return false;
        }

        const CompiledGraph& function = m_Instance.GraphAt(functionGraph);
        if (function.m_EntryNode < 0)
        {
            Error("Target function has no Function.Entry node");
            return false;
        }

        // Marshal the call node's data inputs into the Entry node's output slots.
        // Both lists are built from the function's m_Inputs in the same order, so
        // parameter i is call-pin (1+i) and entry-pin (1+i).
        const CompiledNode& entryNode = function.m_Nodes[static_cast<sizet>(function.m_EntryNode)];
        const sizet paramCount = function.m_Inputs.size();
        for (sizet i = 0; i < paramCount; ++i)
        {
            const sizet callPin = 1 + i;
            const sizet entryPin = 1 + i;
            if (callPin >= callNode.m_Pins.size() || entryPin >= entryNode.m_Pins.size())
            {
                break;
            }
            const i32 slot = entryNode.m_PinInfo[entryPin].m_ValueSlot;
            if (slot < 0)
            {
                continue;
            }
            storage.m_Values[static_cast<sizet>(slot)] = GetInput(callPin).ConvertTo(entryNode.m_Pins[entryPin].m_Type);
        }

        storage.m_Running = true;
        m_Instance.m_ReturnStack.emplace_back(function.m_Outputs.size());
        m_Instance.ExecuteFrom(functionGraph, function.m_EntryNode, -1, m_Runtime, m_Depth + 1);
        std::vector<PinValue> results = std::move(m_Instance.m_ReturnStack.back());
        m_Instance.m_ReturnStack.pop_back();
        storage.m_Running = false;

        // Results land on the call node's output pins, which sit after its input
        // params and its "Then" exec output: 1 + paramCount + 1 + i.
        const sizet resultBase = 1 + paramCount + 1;
        for (sizet i = 0; i < results.size(); ++i)
        {
            SetOutput(resultBase + i, results[i]);
        }
        return true;
    }

    void NodeContext::PublishReturnValues(const std::vector<PinValue>& values) const
    {
        if (m_Instance.m_ReturnStack.empty())
        {
            Error("Function.Return reached outside a Function.Call");
            return;
        }
        std::vector<PinValue>& slot = m_Instance.m_ReturnStack.back();
        for (sizet i = 0; i < values.size() && i < slot.size(); ++i)
        {
            slot[i] = values[i];
        }
    }

} // namespace OloEngine::VisualScript
