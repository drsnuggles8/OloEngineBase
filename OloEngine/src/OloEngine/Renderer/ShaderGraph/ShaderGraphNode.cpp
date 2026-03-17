#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphNode.h"

#include <algorithm>
#include <unordered_map>

namespace OloEngine
{
    // ─────────────────────────────────────────────────────────────
    //  ShaderGraphNode member functions
    // ─────────────────────────────────────────────────────────────

    ShaderGraphPin* ShaderGraphNode::FindPin(UUID pinID)
    {
        OLO_PROFILE_FUNCTION();

        for (auto& pin : Inputs)
        {
            if (pin.ID == pinID)
                return &pin;
        }
        for (auto& pin : Outputs)
        {
            if (pin.ID == pinID)
                return &pin;
        }
        return nullptr;
    }

    const ShaderGraphPin* ShaderGraphNode::FindPin(UUID pinID) const
    {
        OLO_PROFILE_FUNCTION();

        for (const auto& pin : Inputs)
        {
            if (pin.ID == pinID)
                return &pin;
        }
        for (const auto& pin : Outputs)
        {
            if (pin.ID == pinID)
                return &pin;
        }
        return nullptr;
    }

    ShaderGraphPin* ShaderGraphNode::FindPinByName(const std::string& name, ShaderGraphPinDirection direction)
    {
        OLO_PROFILE_FUNCTION();

        auto& pins = (direction == ShaderGraphPinDirection::Input) ? Inputs : Outputs;
        for (auto& pin : pins)
        {
            if (pin.Name == name)
                return &pin;
        }
        return nullptr;
    }

    const ShaderGraphPin* ShaderGraphNode::FindPinByName(const std::string& name, ShaderGraphPinDirection direction) const
    {
        OLO_PROFILE_FUNCTION();

        const auto& pins = (direction == ShaderGraphPinDirection::Input) ? Inputs : Outputs;
        for (const auto& pin : pins)
        {
            if (pin.Name == name)
                return &pin;
        }
        return nullptr;
    }

    // ─────────────────────────────────────────────────────────────
    //  Helper: create pins for a node
    // ─────────────────────────────────────────────────────────────

    static ShaderGraphPin MakeInput(UUID nodeID, const std::string& name, ShaderGraphPinType type, ShaderGraphPinValue defaultValue = {})
    {
        ShaderGraphPin pin;
        pin.ID = UUID();
        pin.Name = name;
        pin.Type = type;
        pin.Direction = ShaderGraphPinDirection::Input;
        pin.NodeID = nodeID;
        pin.DefaultValue = defaultValue;
        return pin;
    }

    static ShaderGraphPin MakeOutput(UUID nodeID, const std::string& name, ShaderGraphPinType type)
    {
        ShaderGraphPin pin;
        pin.ID = UUID();
        pin.Name = name;
        pin.Type = type;
        pin.Direction = ShaderGraphPinDirection::Output;
        pin.NodeID = nodeID;
        return pin;
    }

    // ─────────────────────────────────────────────────────────────
    //  Node initializers (populate pins for each type)
    // ─────────────────────────────────────────────────────────────

    static void InitPBROutput(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Output;
        node.Inputs.push_back(MakeInput(node.ID, "Albedo", ShaderGraphPinType::Vec3, glm::vec3(1.0f)));
        node.Inputs.push_back(MakeInput(node.ID, "Metallic", ShaderGraphPinType::Float, 0.0f));
        node.Inputs.push_back(MakeInput(node.ID, "Roughness", ShaderGraphPinType::Float, 0.5f));
        node.Inputs.push_back(MakeInput(node.ID, "Normal", ShaderGraphPinType::Vec3, glm::vec3(0.0f, 0.0f, 1.0f)));
        node.Inputs.push_back(MakeInput(node.ID, "AO", ShaderGraphPinType::Float, 1.0f));
        node.Inputs.push_back(MakeInput(node.ID, "Emissive", ShaderGraphPinType::Vec3, glm::vec3(0.0f)));
        node.Inputs.push_back(MakeInput(node.ID, "Alpha", ShaderGraphPinType::Float, 1.0f));
    }

    // --- Parameter Nodes ---

    static void InitFloatParameter(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Input;
        node.ParameterName = "u_Float";
        node.Outputs.push_back(MakeOutput(node.ID, "Value", ShaderGraphPinType::Float));
    }

    static void InitFloatConstant(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Input;
        node.Outputs.push_back(MakeOutput(node.ID, "Value", ShaderGraphPinType::Float));
    }

    static void InitVec3Parameter(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Input;
        node.ParameterName = "u_Color";
        node.Outputs.push_back(MakeOutput(node.ID, "Value", ShaderGraphPinType::Vec3));
    }

    static void InitVec3Constant(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Input;
        node.Outputs.push_back(MakeOutput(node.ID, "Value", ShaderGraphPinType::Vec3));
    }

    static void InitVec4Parameter(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Input;
        node.ParameterName = "u_Vec4";
        node.Outputs.push_back(MakeOutput(node.ID, "Value", ShaderGraphPinType::Vec4));
    }

    static void InitTexture2DParameter(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Input;
        node.ParameterName = "u_Texture";
        node.Outputs.push_back(MakeOutput(node.ID, "Value", ShaderGraphPinType::Texture2D));
    }

    static void InitTime(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Input;
        node.Outputs.push_back(MakeOutput(node.ID, "Time", ShaderGraphPinType::Float));
    }

    static void InitUV(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Input;
        node.Outputs.push_back(MakeOutput(node.ID, "UV", ShaderGraphPinType::Vec2));
    }

    static void InitWorldPosition(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Input;
        node.Outputs.push_back(MakeOutput(node.ID, "Position", ShaderGraphPinType::Vec3));
    }

    static void InitWorldNormal(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Input;
        node.Outputs.push_back(MakeOutput(node.ID, "Normal", ShaderGraphPinType::Vec3));
    }

    // --- Math Nodes ---

    static void InitBinaryMathNode(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "A", ShaderGraphPinType::Float, 0.0f));
        node.Inputs.push_back(MakeInput(node.ID, "B", ShaderGraphPinType::Float, 0.0f));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Float));
    }

    static void InitLerp(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "A", ShaderGraphPinType::Float, 0.0f));
        node.Inputs.push_back(MakeInput(node.ID, "B", ShaderGraphPinType::Float, 1.0f));
        node.Inputs.push_back(MakeInput(node.ID, "T", ShaderGraphPinType::Float, 0.5f));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Float));
    }

    static void InitClamp(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "Value", ShaderGraphPinType::Float, 0.0f));
        node.Inputs.push_back(MakeInput(node.ID, "Min", ShaderGraphPinType::Float, 0.0f));
        node.Inputs.push_back(MakeInput(node.ID, "Max", ShaderGraphPinType::Float, 1.0f));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Float));
    }

    static void InitDot(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "A", ShaderGraphPinType::Vec3, glm::vec3(0.0f)));
        node.Inputs.push_back(MakeInput(node.ID, "B", ShaderGraphPinType::Vec3, glm::vec3(0.0f)));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Float));
    }

    static void InitCross(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "A", ShaderGraphPinType::Vec3, glm::vec3(0.0f)));
        node.Inputs.push_back(MakeInput(node.ID, "B", ShaderGraphPinType::Vec3, glm::vec3(0.0f)));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Vec3));
    }

    static void InitUnaryMathNode(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "Value", ShaderGraphPinType::Float, 0.0f));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Float));
    }

    static void InitNormalize(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "Value", ShaderGraphPinType::Vec3, glm::vec3(0.0f)));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Vec3));
    }

    static void InitPower(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "Base", ShaderGraphPinType::Float, 0.0f));
        node.Inputs.push_back(MakeInput(node.ID, "Exponent", ShaderGraphPinType::Float, 1.0f));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Float));
    }

    static void InitSplit(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "Value", ShaderGraphPinType::Vec4, glm::vec4(0.0f)));
        node.Outputs.push_back(MakeOutput(node.ID, "R", ShaderGraphPinType::Float));
        node.Outputs.push_back(MakeOutput(node.ID, "G", ShaderGraphPinType::Float));
        node.Outputs.push_back(MakeOutput(node.ID, "B", ShaderGraphPinType::Float));
        node.Outputs.push_back(MakeOutput(node.ID, "A", ShaderGraphPinType::Float));
    }

    static void InitCombine(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "R", ShaderGraphPinType::Float, 0.0f));
        node.Inputs.push_back(MakeInput(node.ID, "G", ShaderGraphPinType::Float, 0.0f));
        node.Inputs.push_back(MakeInput(node.ID, "B", ShaderGraphPinType::Float, 0.0f));
        node.Inputs.push_back(MakeInput(node.ID, "A", ShaderGraphPinType::Float, 1.0f));
        node.Outputs.push_back(MakeOutput(node.ID, "Vec3", ShaderGraphPinType::Vec3));
        node.Outputs.push_back(MakeOutput(node.ID, "Vec4", ShaderGraphPinType::Vec4));
    }

    static void InitStep(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "Edge", ShaderGraphPinType::Float, 0.5f));
        node.Inputs.push_back(MakeInput(node.ID, "Value", ShaderGraphPinType::Float, 0.0f));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Float));
    }

    static void InitSmoothstep(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Math;
        node.Inputs.push_back(MakeInput(node.ID, "Edge0", ShaderGraphPinType::Float, 0.0f));
        node.Inputs.push_back(MakeInput(node.ID, "Edge1", ShaderGraphPinType::Float, 1.0f));
        node.Inputs.push_back(MakeInput(node.ID, "Value", ShaderGraphPinType::Float, 0.5f));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Float));
    }

    // --- Texture Nodes ---

    static void InitSampleTexture2D(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Texture;
        node.Inputs.push_back(MakeInput(node.ID, "Texture", ShaderGraphPinType::Texture2D));
        node.Inputs.push_back(MakeInput(node.ID, "UV", ShaderGraphPinType::Vec2, glm::vec2(0.0f)));
        node.Outputs.push_back(MakeOutput(node.ID, "RGBA", ShaderGraphPinType::Vec4));
        node.Outputs.push_back(MakeOutput(node.ID, "RGB", ShaderGraphPinType::Vec3));
        node.Outputs.push_back(MakeOutput(node.ID, "R", ShaderGraphPinType::Float));
        node.Outputs.push_back(MakeOutput(node.ID, "G", ShaderGraphPinType::Float));
        node.Outputs.push_back(MakeOutput(node.ID, "B", ShaderGraphPinType::Float));
        node.Outputs.push_back(MakeOutput(node.ID, "A", ShaderGraphPinType::Float));
    }

    static void InitNormalMap(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Texture;
        node.Inputs.push_back(MakeInput(node.ID, "Texture", ShaderGraphPinType::Texture2D));
        node.Inputs.push_back(MakeInput(node.ID, "UV", ShaderGraphPinType::Vec2, glm::vec2(0.0f)));
        node.Inputs.push_back(MakeInput(node.ID, "Strength", ShaderGraphPinType::Float, 1.0f));
        node.Outputs.push_back(MakeOutput(node.ID, "Normal", ShaderGraphPinType::Vec3));
    }

    // --- Utility Nodes ---

    static void InitFresnel(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Utility;
        node.Inputs.push_back(MakeInput(node.ID, "Normal", ShaderGraphPinType::Vec3, glm::vec3(0.0f, 0.0f, 1.0f)));
        node.Inputs.push_back(MakeInput(node.ID, "ViewDir", ShaderGraphPinType::Vec3, glm::vec3(0.0f, 0.0f, 1.0f)));
        node.Inputs.push_back(MakeInput(node.ID, "Power", ShaderGraphPinType::Float, 5.0f));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Float));
    }

    static void InitTilingOffset(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Utility;
        node.Inputs.push_back(MakeInput(node.ID, "UV", ShaderGraphPinType::Vec2, glm::vec2(0.0f)));
        node.Inputs.push_back(MakeInput(node.ID, "Tiling", ShaderGraphPinType::Vec2, glm::vec2(1.0f)));
        node.Inputs.push_back(MakeInput(node.ID, "Offset", ShaderGraphPinType::Vec2, glm::vec2(0.0f)));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Vec2));
    }

    // --- Custom Function Nodes ---

    static void InitCustomFunction(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Custom;
        node.Inputs.push_back(MakeInput(node.ID, "A", ShaderGraphPinType::Float, 0.0f));
        node.Inputs.push_back(MakeInput(node.ID, "B", ShaderGraphPinType::Float, 0.0f));
        node.Outputs.push_back(MakeOutput(node.ID, "Result", ShaderGraphPinType::Float));
        node.CustomFunctionBody = "A + B";
    }

    // --- Compute Nodes ---

    static void InitComputeOutput(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Output;
        node.WorkgroupSize = glm::ivec3(16, 16, 1);
        // ComputeOutput has no pins — it simply marks the graph as compute and defines workgroup size
    }

    static void InitComputeBufferInput(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Compute;
        node.ParameterName = "inputBuffer";
        node.BufferBinding = 0;
        node.Outputs.push_back(MakeOutput(node.ID, "Value", ShaderGraphPinType::Float));
    }

    static void InitComputeBufferStore(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Compute;
        node.ParameterName = "outputBuffer";
        node.BufferBinding = 1;
        node.Inputs.push_back(MakeInput(node.ID, "Value", ShaderGraphPinType::Float, 0.0f));
    }

    static void InitWorkgroupID(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Compute;
        node.Outputs.push_back(MakeOutput(node.ID, "ID", ShaderGraphPinType::Vec3));
    }

    static void InitLocalInvocationID(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Compute;
        node.Outputs.push_back(MakeOutput(node.ID, "ID", ShaderGraphPinType::Vec3));
    }

    static void InitGlobalInvocationID(ShaderGraphNode& node)
    {
        node.Category = ShaderGraphNodeCategory::Compute;
        node.Outputs.push_back(MakeOutput(node.ID, "ID", ShaderGraphPinType::Vec3));
    }

    // ─────────────────────────────────────────────────────────────
    //  Factory registry
    // ─────────────────────────────────────────────────────────────

    using NodeInitFn = void (*)(ShaderGraphNode&);

    struct NodeTypeInfo
    {
        NodeInitFn InitFn;
        ShaderGraphNodeCategory Category;
    };

    static const std::unordered_map<std::string, NodeTypeInfo>& GetNodeRegistry()
    {
        static const std::unordered_map<std::string, NodeTypeInfo> s_Registry = {
            // Output
            { ShaderGraphNodeTypes::PBROutput, { InitPBROutput, ShaderGraphNodeCategory::Output } },
            // Input / Parameter
            { ShaderGraphNodeTypes::FloatParameter, { InitFloatParameter, ShaderGraphNodeCategory::Input } },
            { ShaderGraphNodeTypes::FloatConstant, { InitFloatConstant, ShaderGraphNodeCategory::Input } },
            { ShaderGraphNodeTypes::Vec3Parameter, { InitVec3Parameter, ShaderGraphNodeCategory::Input } },
            { ShaderGraphNodeTypes::Vec3Constant, { InitVec3Constant, ShaderGraphNodeCategory::Input } },
            { ShaderGraphNodeTypes::ColorParameter, { InitVec3Parameter, ShaderGraphNodeCategory::Input } },
            { ShaderGraphNodeTypes::Vec4Parameter, { InitVec4Parameter, ShaderGraphNodeCategory::Input } },
            { ShaderGraphNodeTypes::Texture2DParameter, { InitTexture2DParameter, ShaderGraphNodeCategory::Input } },
            { ShaderGraphNodeTypes::Time, { InitTime, ShaderGraphNodeCategory::Input } },
            { ShaderGraphNodeTypes::UV, { InitUV, ShaderGraphNodeCategory::Input } },
            { ShaderGraphNodeTypes::WorldPosition, { InitWorldPosition, ShaderGraphNodeCategory::Input } },
            { ShaderGraphNodeTypes::WorldNormal, { InitWorldNormal, ShaderGraphNodeCategory::Input } },
            // Math
            { ShaderGraphNodeTypes::Add, { InitBinaryMathNode, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Subtract, { InitBinaryMathNode, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Multiply, { InitBinaryMathNode, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Divide, { InitBinaryMathNode, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Lerp, { InitLerp, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Clamp, { InitClamp, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Dot, { InitDot, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Cross, { InitCross, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Normalize, { InitNormalize, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Power, { InitPower, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Abs, { InitUnaryMathNode, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Floor, { InitUnaryMathNode, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Ceil, { InitUnaryMathNode, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Fract, { InitUnaryMathNode, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Sin, { InitUnaryMathNode, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Cos, { InitUnaryMathNode, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::OneMinus, { InitUnaryMathNode, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Split, { InitSplit, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Combine, { InitCombine, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Step, { InitStep, ShaderGraphNodeCategory::Math } },
            { ShaderGraphNodeTypes::Smoothstep, { InitSmoothstep, ShaderGraphNodeCategory::Math } },
            // Texture
            { ShaderGraphNodeTypes::SampleTexture2D, { InitSampleTexture2D, ShaderGraphNodeCategory::Texture } },
            { ShaderGraphNodeTypes::NormalMap, { InitNormalMap, ShaderGraphNodeCategory::Texture } },
            // Utility
            { ShaderGraphNodeTypes::Fresnel, { InitFresnel, ShaderGraphNodeCategory::Utility } },
            { ShaderGraphNodeTypes::TilingOffset, { InitTilingOffset, ShaderGraphNodeCategory::Utility } },
            // Custom
            { ShaderGraphNodeTypes::CustomFunction, { InitCustomFunction, ShaderGraphNodeCategory::Custom } },
            // Compute
            { ShaderGraphNodeTypes::ComputeOutput, { InitComputeOutput, ShaderGraphNodeCategory::Output } },
            { ShaderGraphNodeTypes::ComputeBufferInput, { InitComputeBufferInput, ShaderGraphNodeCategory::Compute } },
            { ShaderGraphNodeTypes::ComputeBufferStore, { InitComputeBufferStore, ShaderGraphNodeCategory::Compute } },
            { ShaderGraphNodeTypes::WorkgroupID, { InitWorkgroupID, ShaderGraphNodeCategory::Compute } },
            { ShaderGraphNodeTypes::LocalInvocationID, { InitLocalInvocationID, ShaderGraphNodeCategory::Compute } },
            { ShaderGraphNodeTypes::GlobalInvocationID, { InitGlobalInvocationID, ShaderGraphNodeCategory::Compute } },
        };
        return s_Registry;
    }

    Scope<ShaderGraphNode> CreateShaderGraphNode(const std::string& typeName)
    {
        OLO_PROFILE_FUNCTION();

        const auto& registry = GetNodeRegistry();
        auto it = registry.find(typeName);
        if (it == registry.end())
            return nullptr;

        auto node = CreateScope<ShaderGraphNode>();
        node->ID = UUID();
        node->TypeName = typeName;
        it->second.InitFn(*node);
        return node;
    }

    std::vector<std::string> GetAllNodeTypeNames()
    {
        OLO_PROFILE_FUNCTION();

        const auto& registry = GetNodeRegistry();
        std::vector<std::string> names;
        names.reserve(registry.size());
        for (const auto& [name, info] : registry)
            names.push_back(name);
        std::sort(names.begin(), names.end());
        return names;
    }

    std::vector<std::string> GetNodeTypeNamesByCategory(ShaderGraphNodeCategory category)
    {
        OLO_PROFILE_FUNCTION();

        const auto& registry = GetNodeRegistry();
        std::vector<std::string> names;
        for (const auto& [name, info] : registry)
        {
            if (info.Category == category)
                names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

} // namespace OloEngine
