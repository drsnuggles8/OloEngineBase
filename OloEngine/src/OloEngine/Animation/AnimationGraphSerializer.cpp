#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationGraphSerializer.h"
#include "OloEngine/Animation/AnimationStateMachine.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationTransition.h"
#include "OloEngine/Animation/BlendTree.h"
#include "OloEngine/Animation/AnimationLayer.h"
#include "OloEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <fstream>

namespace OloEngine
{
    static std::string ParameterTypeToString(AnimationParameterType type)
    {
        switch (type)
        {
            case AnimationParameterType::Float:
                return "Float";
            case AnimationParameterType::Int:
                return "Int";
            case AnimationParameterType::Bool:
                return "Bool";
            case AnimationParameterType::Trigger:
                return "Trigger";
        }
        return "Float";
    }

    static AnimationParameterType StringToParameterType(const std::string& str)
    {
        if (str == "Int")
            return AnimationParameterType::Int;
        if (str == "Bool")
            return AnimationParameterType::Bool;
        if (str == "Trigger")
            return AnimationParameterType::Trigger;
        return AnimationParameterType::Float;
    }

    static std::string ComparisonToString(TransitionCondition::Comparison op)
    {
        switch (op)
        {
            case TransitionCondition::Comparison::Greater:
                return "Greater";
            case TransitionCondition::Comparison::Less:
                return "Less";
            case TransitionCondition::Comparison::Equal:
                return "Equal";
            case TransitionCondition::Comparison::NotEqual:
                return "NotEqual";
            case TransitionCondition::Comparison::TriggerSet:
                return "TriggerSet";
        }
        return "Greater";
    }

    static TransitionCondition::Comparison StringToComparison(const std::string& str)
    {
        if (str == "Less")
            return TransitionCondition::Comparison::Less;
        if (str == "Equal")
            return TransitionCondition::Comparison::Equal;
        if (str == "NotEqual")
            return TransitionCondition::Comparison::NotEqual;
        if (str == "TriggerSet")
            return TransitionCondition::Comparison::TriggerSet;
        return TransitionCondition::Comparison::Greater;
    }

    static std::string BlendTypeToString(BlendTree::BlendType type)
    {
        switch (type)
        {
            case BlendTree::BlendType::Simple1D:
                return "Simple1D";
            case BlendTree::BlendType::SimpleDirectional2D:
                return "SimpleDirectional2D";
            case BlendTree::BlendType::FreeformDirectional2D:
                return "FreeformDirectional2D";
            case BlendTree::BlendType::FreeformCartesian2D:
                return "FreeformCartesian2D";
        }
        return "Simple1D";
    }

    static BlendTree::BlendType StringToBlendType(const std::string& str)
    {
        if (str == "SimpleDirectional2D")
            return BlendTree::BlendType::SimpleDirectional2D;
        if (str == "FreeformDirectional2D")
            return BlendTree::BlendType::FreeformDirectional2D;
        if (str == "FreeformCartesian2D")
            return BlendTree::BlendType::FreeformCartesian2D;
        return BlendTree::BlendType::Simple1D;
    }

    static std::string BlendModeToString(AnimationLayer::BlendMode mode)
    {
        switch (mode)
        {
            case AnimationLayer::BlendMode::Override:
                return "Override";
            case AnimationLayer::BlendMode::Additive:
                return "Additive";
        }
        return "Override";
    }

    static AnimationLayer::BlendMode StringToBlendMode(const std::string& str)
    {
        if (str == "Additive")
            return AnimationLayer::BlendMode::Additive;
        return AnimationLayer::BlendMode::Override;
    }

    // ---- Serialization ----

    static void SerializeBlendTree(YAML::Emitter& out, const Ref<BlendTree>& tree)
    {
        out << YAML::Key << "blend_tree";
        out << YAML::BeginMap;
        out << YAML::Key << "type" << YAML::Value << BlendTypeToString(tree->Type);
        out << YAML::Key << "parameter" << YAML::Value << tree->BlendParameterX;
        if (!tree->BlendParameterY.empty())
        {
            out << YAML::Key << "parameter_y" << YAML::Value << tree->BlendParameterY;
        }
        out << YAML::Key << "children" << YAML::Value << YAML::BeginSeq;
        for (auto const& child : tree->Children)
        {
            out << YAML::BeginMap;
            if (child.Clip)
            {
                out << YAML::Key << "clip" << YAML::Value << child.Clip->Name;
            }
            out << YAML::Key << "threshold" << YAML::Value << child.Threshold;
            if (tree->Type != BlendTree::BlendType::Simple1D)
            {
                out << YAML::Key << "position" << YAML::Value << YAML::Flow << YAML::BeginSeq
                    << child.Position.x << child.Position.y << YAML::EndSeq;
            }
            out << YAML::Key << "speed" << YAML::Value << child.Speed;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::EndMap;
    }

    static void SerializeState(YAML::Emitter& out, const AnimationState& state)
    {
        out << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << state.Name;
        out << YAML::Key << "speed" << YAML::Value << state.Speed;
        out << YAML::Key << "loop" << YAML::Value << state.Looping;

        if (state.Type == AnimationState::MotionType::SingleClip)
        {
            if (state.Clip)
            {
                out << YAML::Key << "clip" << YAML::Value << state.Clip->Name;
            }
        }
        else if (state.Type == AnimationState::MotionType::BlendTree && state.Tree)
        {
            SerializeBlendTree(out, state.Tree);
        }

        out << YAML::EndMap;
    }

    static void SerializeTransition(YAML::Emitter& out, const AnimationTransition& transition)
    {
        out << YAML::BeginMap;
        out << YAML::Key << "from" << YAML::Value << (transition.SourceState.empty() ? "*" : transition.SourceState);
        out << YAML::Key << "to" << YAML::Value << transition.DestinationState;
        out << YAML::Key << "blend_duration" << YAML::Value << transition.BlendDuration;

        if (transition.HasExitTime)
        {
            out << YAML::Key << "exit_time" << YAML::Value << transition.ExitTime;
            out << YAML::Key << "has_exit_time" << YAML::Value << true;
        }

        if (transition.CanTransitionToSelf)
        {
            out << YAML::Key << "can_transition_to_self" << YAML::Value << true;
        }

        if (!transition.Conditions.empty())
        {
            out << YAML::Key << "conditions" << YAML::Value << YAML::BeginSeq;
            for (auto const& cond : transition.Conditions)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "parameter" << YAML::Value << cond.ParameterName;
                out << YAML::Key << "op" << YAML::Value << ComparisonToString(cond.Op);
                if (cond.Op != TransitionCondition::Comparison::TriggerSet)
                {
                    out << YAML::Key << "float_threshold" << YAML::Value << cond.FloatThreshold;
                    out << YAML::Key << "int_threshold" << YAML::Value << cond.IntThreshold;
                    out << YAML::Key << "bool_value" << YAML::Value << cond.BoolValue;
                }
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        out << YAML::EndMap;
    }

    bool AnimationGraphSerializer::Serialize(const Ref<AnimationGraph>& graph, const std::string& filepath)
    {
        YAML::Emitter out;
        out << YAML::BeginMap;

        // Parameters
        out << YAML::Key << "parameters" << YAML::Value << YAML::BeginSeq;
        for (auto const& [name, param] : graph->Parameters.GetAll())
        {
            out << YAML::BeginMap;
            out << YAML::Key << "name" << YAML::Value << param.Name;
            out << YAML::Key << "type" << YAML::Value << ParameterTypeToString(param.ParamType);
            switch (param.ParamType)
            {
                case AnimationParameterType::Float:
                    out << YAML::Key << "default" << YAML::Value << param.FloatValue;
                    break;
                case AnimationParameterType::Int:
                    out << YAML::Key << "default" << YAML::Value << param.IntValue;
                    break;
                case AnimationParameterType::Bool:
                    out << YAML::Key << "default" << YAML::Value << param.BoolValue;
                    break;
                case AnimationParameterType::Trigger:
                    break;
            }
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        // Layers
        out << YAML::Key << "layers" << YAML::Value << YAML::BeginSeq;
        for (auto const& layer : graph->Layers)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "name" << YAML::Value << layer.Name;
            out << YAML::Key << "blend_mode" << YAML::Value << BlendModeToString(layer.Mode);
            out << YAML::Key << "weight" << YAML::Value << layer.Weight;

            if (!layer.AffectedBones.empty())
            {
                out << YAML::Key << "affected_bones" << YAML::Value << YAML::Flow << YAML::BeginSeq;
                for (auto const& bone : layer.AffectedBones)
                {
                    out << bone;
                }
                out << YAML::EndSeq;
            }

            if (layer.StateMachine)
            {
                auto const& sm = layer.StateMachine;

                out << YAML::Key << "default_state" << YAML::Value << sm->GetDefaultState();

                // States
                out << YAML::Key << "states" << YAML::Value << YAML::BeginSeq;
                for (auto const& [name, state] : sm->GetStates())
                {
                    SerializeState(out, state);
                }
                out << YAML::EndSeq;

                // Transitions
                out << YAML::Key << "transitions" << YAML::Value << YAML::BeginSeq;
                for (auto const& transition : sm->GetTransitions())
                {
                    SerializeTransition(out, transition);
                }
                out << YAML::EndSeq;
            }

            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap;

        std::ofstream fout(filepath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("AnimationGraphSerializer: Failed to open file '{}'", filepath);
            return false;
        }
        fout << out.c_str();
        return true;
    }

    // ---- Deserialization ----

    static Ref<BlendTree> DeserializeBlendTree(const YAML::Node& node)
    {
        auto tree = Ref<BlendTree>::Create();
        tree->Type = StringToBlendType(node["type"].as<std::string>("Simple1D"));
        tree->BlendParameterX = node["parameter"].as<std::string>("");
        if (node["parameter_y"])
        {
            tree->BlendParameterY = node["parameter_y"].as<std::string>("");
        }

        if (auto children = node["children"]; children && children.IsSequence())
        {
            for (auto const& childNode : children)
            {
                BlendTree::BlendChild child;
                // Clip name stored for lookup; actual Ref<AnimationClip> linking is done at load time
                child.Threshold = childNode["threshold"].as<f32>(0.0f);
                child.Speed = childNode["speed"].as<f32>(1.0f);
                if (childNode["position"] && childNode["position"].IsSequence())
                {
                    child.Position.x = childNode["position"][0].as<f32>(0.0f);
                    child.Position.y = childNode["position"][1].as<f32>(0.0f);
                }
                tree->Children.push_back(child);
            }
        }

        return tree;
    }

    static AnimationState DeserializeState(const YAML::Node& node)
    {
        AnimationState state;
        state.Name = node["name"].as<std::string>("");
        state.Speed = node["speed"].as<f32>(1.0f);
        state.Looping = node["loop"].as<bool>(true);

        if (node["blend_tree"])
        {
            state.Type = AnimationState::MotionType::BlendTree;
            state.Tree = DeserializeBlendTree(node["blend_tree"]);
        }
        else
        {
            state.Type = AnimationState::MotionType::SingleClip;
            // Clip is stored by name; linking to actual clip done at asset load time
        }

        return state;
    }

    static AnimationTransition DeserializeTransition(const YAML::Node& node)
    {
        AnimationTransition transition;
        std::string from = node["from"].as<std::string>("*");
        transition.SourceState = (from == "*") ? "" : from;
        transition.DestinationState = node["to"].as<std::string>("");
        transition.BlendDuration = node["blend_duration"].as<f32>(0.2f);
        transition.HasExitTime = node["has_exit_time"].as<bool>(false);
        transition.ExitTime = node["exit_time"].as<f32>(-1.0f);
        transition.CanTransitionToSelf = node["can_transition_to_self"].as<bool>(false);

        if (auto conditions = node["conditions"]; conditions && conditions.IsSequence())
        {
            for (auto const& condNode : conditions)
            {
                TransitionCondition cond;
                cond.ParameterName = condNode["parameter"].as<std::string>("");
                cond.Op = StringToComparison(condNode["op"].as<std::string>("Greater"));
                cond.FloatThreshold = condNode["float_threshold"].as<f32>(0.0f);
                cond.IntThreshold = condNode["int_threshold"].as<i32>(0);
                cond.BoolValue = condNode["bool_value"].as<bool>(false);
                transition.Conditions.push_back(cond);
            }
        }

        return transition;
    }

    Ref<AnimationGraph> AnimationGraphSerializer::Deserialize(const std::string& filepath)
    {
        std::ifstream fin(filepath);
        if (!fin.is_open())
        {
            OLO_CORE_ERROR("AnimationGraphSerializer: Failed to open file '{}'", filepath);
            return nullptr;
        }

        YAML::Node root = YAML::Load(fin);
        if (!root)
        {
            OLO_CORE_ERROR("AnimationGraphSerializer: Failed to parse YAML from '{}'", filepath);
            return nullptr;
        }

        auto graph = Ref<AnimationGraph>::Create();

        // Parameters
        if (auto params = root["parameters"]; params && params.IsSequence())
        {
            for (auto const& paramNode : params)
            {
                std::string name = paramNode["name"].as<std::string>("");
                auto type = StringToParameterType(paramNode["type"].as<std::string>("Float"));
                switch (type)
                {
                    case AnimationParameterType::Float:
                        graph->Parameters.DefineFloat(name, paramNode["default"].as<f32>(0.0f));
                        break;
                    case AnimationParameterType::Int:
                        graph->Parameters.DefineInt(name, paramNode["default"].as<i32>(0));
                        break;
                    case AnimationParameterType::Bool:
                        graph->Parameters.DefineBool(name, paramNode["default"].as<bool>(false));
                        break;
                    case AnimationParameterType::Trigger:
                        graph->Parameters.DefineTrigger(name);
                        break;
                }
            }
        }

        // Layers
        if (auto layers = root["layers"]; layers && layers.IsSequence())
        {
            for (auto const& layerNode : layers)
            {
                AnimationLayer layer;
                layer.Name = layerNode["name"].as<std::string>("Base Layer");
                layer.Mode = StringToBlendMode(layerNode["blend_mode"].as<std::string>("Override"));
                layer.Weight = layerNode["weight"].as<f32>(1.0f);

                if (auto affectedBones = layerNode["affected_bones"]; affectedBones && affectedBones.IsSequence())
                {
                    for (auto const& boneNode : affectedBones)
                    {
                        layer.AffectedBones.push_back(boneNode.as<std::string>());
                    }
                }

                auto sm = Ref<AnimationStateMachine>::Create();

                if (auto defaultState = layerNode["default_state"]; defaultState)
                {
                    sm->SetDefaultState(defaultState.as<std::string>(""));
                }

                if (auto states = layerNode["states"]; states && states.IsSequence())
                {
                    for (auto const& stateNode : states)
                    {
                        auto state = DeserializeState(stateNode);
                        sm->AddState(state);
                    }
                }

                if (auto transitions = layerNode["transitions"]; transitions && transitions.IsSequence())
                {
                    for (auto const& transNode : transitions)
                    {
                        auto transition = DeserializeTransition(transNode);
                        sm->AddTransition(transition);
                    }
                }

                layer.StateMachine = sm;
                graph->Layers.push_back(layer);
            }
        }

        return graph;
    }

    bool AnimationGraphSerializer::SerializeAsset(const Ref<AnimationGraphAsset>& asset, const std::string& filepath)
    {
        if (!asset || !asset->GetGraph())
        {
            return false;
        }
        return Serialize(asset->GetGraph(), filepath);
    }

    Ref<AnimationGraphAsset> AnimationGraphSerializer::DeserializeAsset(const std::string& filepath)
    {
        auto graph = Deserialize(filepath);
        if (!graph)
        {
            return nullptr;
        }

        auto asset = Ref<AnimationGraphAsset>::Create();
        asset->SetFilePath(filepath);
        asset->SetGraph(graph);
        return asset;
    }
} // namespace OloEngine
