#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphCompiler.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphTypes.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <sstream>
#include <unordered_map>

namespace OloEngine
{
    // ─────────────────────────────────────────────────────────────
    //  Variable naming
    // ─────────────────────────────────────────────────────────────

    std::string ShaderGraphCompiler::MakeVarName(const ShaderGraphNode& node, const ShaderGraphPin& pin)
    {
        // Produces e.g. "node_12345_Result"
        return "node_" + std::to_string(static_cast<u64>(node.ID)) + "_" + pin.Name;
    }

    // ─────────────────────────────────────────────────────────────
    //  Input expression resolution
    // ─────────────────────────────────────────────────────────────

    std::string ShaderGraphCompiler::ResolveInputExpression(
        const ShaderGraph& graph,
        const ShaderGraphPin& inputPin,
        const std::unordered_map<u64, std::string>& pinVarNames) const
    {
        const auto* link = graph.GetLinkForInputPin(inputPin.ID);
        if (!link)
        {
            // Not connected — use default value
            return inputPin.GetDefaultValueGLSL();
        }

        const auto* outputPin = graph.FindPin(link->OutputPinID);
        if (!outputPin)
            return inputPin.GetDefaultValueGLSL();

        auto it = pinVarNames.find(static_cast<u64>(outputPin->ID));
        if (it == pinVarNames.end())
            return inputPin.GetDefaultValueGLSL();

        // Apply type conversion if needed
        return GenerateTypeConversion(it->second, outputPin->Type, inputPin.Type);
    }

    // ─────────────────────────────────────────────────────────────
    //  Per-node code generation
    // ─────────────────────────────────────────────────────────────

    std::string ShaderGraphCompiler::GenerateNodeCode(
        const ShaderGraph& graph,
        const ShaderGraphNode& node,
        std::unordered_map<u64, std::string>& pinVarNames) const
    {
        std::ostringstream code;
        const auto& typeName = node.TypeName;

        // ── Input / parameter nodes (declare variables from built-ins or uniforms) ──

        if (typeName == ShaderGraphNodeTypes::FloatParameter || typeName == ShaderGraphNodeTypes::Vec3Parameter || typeName == ShaderGraphNodeTypes::Vec4Parameter || typeName == ShaderGraphNodeTypes::ColorParameter)
        {
            // These read from a uniform, variable already declared in uniform block
            // Just map the output pin to the uniform name
            if (!node.Outputs.empty())
                pinVarNames[static_cast<u64>(node.Outputs[0].ID)] = node.ParameterName;
            return {};
        }

        if (typeName == ShaderGraphNodeTypes::FloatConstant || typeName == ShaderGraphNodeTypes::Vec3Constant)
        {
            // Constant nodes emit their default value as a literal (no uniform)
            if (!node.Outputs.empty())
                pinVarNames[static_cast<u64>(node.Outputs[0].ID)] = node.Outputs[0].GetDefaultValueGLSL();
            return {};
        }

        if (typeName == ShaderGraphNodeTypes::Texture2DParameter)
        {
            // Sampler — pin maps to sampler name; no code emitted
            if (!node.Outputs.empty())
                pinVarNames[static_cast<u64>(node.Outputs[0].ID)] = node.ParameterName;
            return {};
        }

        if (typeName == ShaderGraphNodeTypes::Time)
        {
            if (!node.Outputs.empty())
                pinVarNames[static_cast<u64>(node.Outputs[0].ID)] = "u_Time";
            return {};
        }

        if (typeName == ShaderGraphNodeTypes::UV)
        {
            if (!node.Outputs.empty())
                pinVarNames[static_cast<u64>(node.Outputs[0].ID)] = "v_TexCoord";
            return {};
        }

        if (typeName == ShaderGraphNodeTypes::WorldPosition)
        {
            if (!node.Outputs.empty())
                pinVarNames[static_cast<u64>(node.Outputs[0].ID)] = "v_WorldPosition";
            return {};
        }

        if (typeName == ShaderGraphNodeTypes::WorldNormal)
        {
            if (!node.Outputs.empty())
                pinVarNames[static_cast<u64>(node.Outputs[0].ID)] = "v_Normal";
            return {};
        }

        // ── Math nodes ──

        auto resolveInput = [&](const std::string& pinName) -> std::string
        {
            const auto* pin = node.FindPinByName(pinName, ShaderGraphPinDirection::Input);
            if (!pin)
                return "0.0";
            return ResolveInputExpression(graph, *pin, pinVarNames);
        };

        auto emitOutputVar = [&](const std::string& pinName, ShaderGraphPinType type, const std::string& expr)
        {
            const auto* pin = node.FindPinByName(pinName, ShaderGraphPinDirection::Output);
            if (!pin)
                return;
            std::string var = MakeVarName(node, *pin);
            code << "    " << PinTypeToGLSL(type) << " " << var << " = " << expr << ";\n";
            pinVarNames[static_cast<u64>(pin->ID)] = var;
        };

        // Binary math ops
        if (typeName == ShaderGraphNodeTypes::Add)
        {
            std::string a = resolveInput("A"), b = resolveInput("B");
            emitOutputVar("Result", ShaderGraphPinType::Float, "(" + a + " + " + b + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Subtract)
        {
            std::string a = resolveInput("A"), b = resolveInput("B");
            emitOutputVar("Result", ShaderGraphPinType::Float, "(" + a + " - " + b + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Multiply)
        {
            std::string a = resolveInput("A"), b = resolveInput("B");
            emitOutputVar("Result", ShaderGraphPinType::Float, "(" + a + " * " + b + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Divide)
        {
            std::string a = resolveInput("A"), b = resolveInput("B");
            emitOutputVar("Result", ShaderGraphPinType::Float, "(" + a + " / " + b + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Lerp)
        {
            std::string a = resolveInput("A"), b = resolveInput("B"), t = resolveInput("T");
            emitOutputVar("Result", ShaderGraphPinType::Float, "mix(" + a + ", " + b + ", " + t + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Clamp)
        {
            std::string v = resolveInput("Value"), mn = resolveInput("Min"), mx = resolveInput("Max");
            emitOutputVar("Result", ShaderGraphPinType::Float, "clamp(" + v + ", " + mn + ", " + mx + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Dot)
        {
            std::string a = resolveInput("A"), b = resolveInput("B");
            emitOutputVar("Result", ShaderGraphPinType::Float, "dot(" + a + ", " + b + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Cross)
        {
            std::string a = resolveInput("A"), b = resolveInput("B");
            emitOutputVar("Result", ShaderGraphPinType::Vec3, "cross(" + a + ", " + b + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Normalize)
        {
            std::string v = resolveInput("Value");
            // Determine output type from the input pin's connected type
            const auto* inputPin = node.FindPinByName("Value", ShaderGraphPinDirection::Input);
            ShaderGraphPinType outType = ShaderGraphPinType::Vec3; // Default: normalize() returns a vector
            if (inputPin)
            {
                const auto* link = graph.GetLinkForInputPin(inputPin->ID);
                if (link)
                {
                    if (const auto* srcPin = graph.FindPin(link->OutputPinID))
                        outType = srcPin->Type;
                }
                else
                {
                    outType = inputPin->Type;
                }
            }
            // normalize() is undefined for scalars; emit as-is for Float
            if (outType == ShaderGraphPinType::Float)
                emitOutputVar("Result", ShaderGraphPinType::Float, v);
            else
                emitOutputVar("Result", outType, "normalize(" + v + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Power)
        {
            std::string base = resolveInput("Base"), exp = resolveInput("Exponent");
            emitOutputVar("Result", ShaderGraphPinType::Float, "pow(" + base + ", " + exp + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Abs)
        {
            std::string v = resolveInput("Value");
            emitOutputVar("Result", ShaderGraphPinType::Float, "abs(" + v + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Floor)
        {
            std::string v = resolveInput("Value");
            emitOutputVar("Result", ShaderGraphPinType::Float, "floor(" + v + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Ceil)
        {
            std::string v = resolveInput("Value");
            emitOutputVar("Result", ShaderGraphPinType::Float, "ceil(" + v + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Fract)
        {
            std::string v = resolveInput("Value");
            emitOutputVar("Result", ShaderGraphPinType::Float, "fract(" + v + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Sin)
        {
            std::string v = resolveInput("Value");
            emitOutputVar("Result", ShaderGraphPinType::Float, "sin(" + v + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Cos)
        {
            std::string v = resolveInput("Value");
            emitOutputVar("Result", ShaderGraphPinType::Float, "cos(" + v + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::OneMinus)
        {
            std::string v = resolveInput("Value");
            emitOutputVar("Result", ShaderGraphPinType::Float, "(1.0 - " + v + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Step)
        {
            std::string edge = resolveInput("Edge"), v = resolveInput("Value");
            emitOutputVar("Result", ShaderGraphPinType::Float, "step(" + edge + ", " + v + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Smoothstep)
        {
            std::string e0 = resolveInput("Edge0"), e1 = resolveInput("Edge1"), v = resolveInput("Value");
            emitOutputVar("Result", ShaderGraphPinType::Float, "smoothstep(" + e0 + ", " + e1 + ", " + v + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::Split)
        {
            std::string v = resolveInput("Value");
            emitOutputVar("R", ShaderGraphPinType::Float, v + ".x");
            emitOutputVar("G", ShaderGraphPinType::Float, v + ".y");
            emitOutputVar("B", ShaderGraphPinType::Float, v + ".z");
            emitOutputVar("A", ShaderGraphPinType::Float, v + ".w");
        }
        else if (typeName == ShaderGraphNodeTypes::Combine)
        {
            std::string r = resolveInput("R"), g = resolveInput("G"), b = resolveInput("B"), a = resolveInput("A");
            emitOutputVar("Vec3", ShaderGraphPinType::Vec3, "vec3(" + r + ", " + g + ", " + b + ")");
            emitOutputVar("Vec4", ShaderGraphPinType::Vec4, "vec4(" + r + ", " + g + ", " + b + ", " + a + ")");
        }

        // ── Texture nodes ──

        else if (typeName == ShaderGraphNodeTypes::SampleTexture2D)
        {
            std::string tex = resolveInput("Texture");
            std::string uv = resolveInput("UV");
            std::string sampleVar = MakeVarName(node, *node.FindPinByName("RGBA", ShaderGraphPinDirection::Output));

            // If the Texture input is unconnected, tex resolves to a non-sampler fallback — emit zero
            const auto* texPin = node.FindPinByName("Texture", ShaderGraphPinDirection::Input);
            bool texConnected = texPin && graph.GetLinkForInputPin(texPin->ID);
            if (texConnected)
                code << "    vec4 " << sampleVar << " = texture(" << tex << ", " << uv << ");\n";
            else
                code << "    vec4 " << sampleVar << " = vec4(0.0);\n";
            pinVarNames[static_cast<u64>(node.FindPinByName("RGBA", ShaderGraphPinDirection::Output)->ID)] = sampleVar;

            // Derive sub-outputs
            auto emitDerived = [&](const std::string& name, const std::string& swizzle, ShaderGraphPinType type)
            {
                const auto* pin = node.FindPinByName(name, ShaderGraphPinDirection::Output);
                if (pin)
                    pinVarNames[static_cast<u64>(pin->ID)] = sampleVar + swizzle;
            };
            emitDerived("RGB", ".rgb", ShaderGraphPinType::Vec3);
            emitDerived("R", ".r", ShaderGraphPinType::Float);
            emitDerived("G", ".g", ShaderGraphPinType::Float);
            emitDerived("B", ".b", ShaderGraphPinType::Float);
            emitDerived("A", ".a", ShaderGraphPinType::Float);
        }
        else if (typeName == ShaderGraphNodeTypes::NormalMap)
        {
            std::string tex = resolveInput("Texture");
            std::string uv = resolveInput("UV");
            std::string strength = resolveInput("Strength");
            std::string var = MakeVarName(node, *node.FindPinByName("Normal", ShaderGraphPinDirection::Output));

            const auto* texPin = node.FindPinByName("Texture", ShaderGraphPinDirection::Input);
            bool texConnected = texPin && graph.GetLinkForInputPin(texPin->ID);
            if (texConnected)
            {
                code << "    vec3 " << var << " = normalize(texture(" << tex << ", " << uv << ").rgb * 2.0 - 1.0);\n";
                code << "    " << var << ".xy *= " << strength << ";\n";
                code << "    " << var << " = normalize(" << var << ");\n";
            }
            else
            {
                code << "    vec3 " << var << " = vec3(0.0, 0.0, 1.0);\n"; // Default normal
            }
            pinVarNames[static_cast<u64>(node.FindPinByName("Normal", ShaderGraphPinDirection::Output)->ID)] = var;
        }

        // ── Utility nodes ──

        else if (typeName == ShaderGraphNodeTypes::Fresnel)
        {
            std::string normal = resolveInput("Normal");
            std::string viewDir = resolveInput("ViewDir");
            std::string power = resolveInput("Power");
            emitOutputVar("Result", ShaderGraphPinType::Float,
                          "pow(1.0 - max(dot(" + normal + ", " + viewDir + "), 0.0), " + power + ")");
        }
        else if (typeName == ShaderGraphNodeTypes::TilingOffset)
        {
            std::string uv = resolveInput("UV");
            std::string tiling = resolveInput("Tiling");
            std::string offset = resolveInput("Offset");
            emitOutputVar("Result", ShaderGraphPinType::Vec2, uv + " * " + tiling + " + " + offset);
        }

        // ── Custom Function node ──

        else if (typeName == ShaderGraphNodeTypes::CustomFunction)
        {
            // Substitute input names in the custom expression
            std::string body = node.CustomFunctionBody;
            for (const auto& inputPin : node.Inputs)
            {
                std::string resolved = ResolveInputExpression(graph, inputPin, pinVarNames);
                // Replace whole-word occurrences of the pin name with the resolved expression
                std::string search = inputPin.Name;
                std::string::size_type pos = 0;
                while ((pos = body.find(search, pos)) != std::string::npos)
                {
                    // Check for word boundaries (identifier characters include underscore)
                    auto isIdentChar = [](char c)
                    { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
                    bool leftOk = (pos == 0) || !isIdentChar(body[pos - 1]);
                    bool rightOk = (pos + search.size() >= body.size()) ||
                                   !isIdentChar(body[pos + search.size()]);
                    if (leftOk && rightOk)
                    {
                        body.replace(pos, search.size(), resolved);
                        pos += resolved.size();
                    }
                    else
                    {
                        pos += search.size();
                    }
                }
            }
            emitOutputVar("Result", ShaderGraphPinType::Float, body);
        }

        // ── Compute nodes ──

        else if (typeName == ShaderGraphNodeTypes::GlobalInvocationID)
        {
            if (!node.Outputs.empty())
                pinVarNames[static_cast<u64>(node.Outputs[0].ID)] = "vec3(gl_GlobalInvocationID)";
        }
        else if (typeName == ShaderGraphNodeTypes::WorkgroupID)
        {
            if (!node.Outputs.empty())
                pinVarNames[static_cast<u64>(node.Outputs[0].ID)] = "vec3(gl_WorkGroupID)";
        }
        else if (typeName == ShaderGraphNodeTypes::LocalInvocationID)
        {
            if (!node.Outputs.empty())
                pinVarNames[static_cast<u64>(node.Outputs[0].ID)] = "vec3(gl_LocalInvocationID)";
        }
        else if (typeName == ShaderGraphNodeTypes::ComputeBufferInput)
        {
            // Buffer read: maps output to buffer_NAME[gl_GlobalInvocationID.x]
            std::string bufName = node.ParameterName.empty() ? "inputBuffer" : node.ParameterName;
            if (!node.Outputs.empty())
            {
                std::string var = MakeVarName(node, node.Outputs[0]);
                code << "    float " << var << " = " << bufName << ".data[gl_GlobalInvocationID.x];\n";
                pinVarNames[static_cast<u64>(node.Outputs[0].ID)] = var;
            }
        }
        else if (typeName == ShaderGraphNodeTypes::ComputeBufferStore)
        {
            // Buffer write — handled in GenerateComputeShader as the "output"
            std::string bufName = node.ParameterName.empty() ? "outputBuffer" : node.ParameterName;
            std::string valueExpr = resolveInput("Value");
            code << "    " << bufName << ".data[gl_GlobalInvocationID.x] = " << valueExpr << ";\n";
        }

        // ── PBROutput / ComputeOutput — handled separately in generators ──

        return code.str();
    }

    // ─────────────────────────────────────────────────────────────
    //  Vertex shader generation
    // ─────────────────────────────────────────────────────────────

    std::string ShaderGraphCompiler::GenerateVertexShader() const
    {
        OLO_PROFILE_FUNCTION();

        return R"(#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec4 u_CameraPosition;
};

layout(std140, binding = 3) uniform ModelMatrices
{
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    float _pad0;
    float _pad1;
    float _pad2;
};

layout(location = 0) out vec3 v_WorldPosition;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_WorldPosition = worldPos.xyz;
    v_Normal = normalize(mat3(u_Normal) * a_Normal);
    v_TexCoord = a_TexCoord;
    gl_Position = u_ViewProjection * worldPos;
}
)";
    }

    // ─────────────────────────────────────────────────────────────
    //  Fragment shader generation
    // ─────────────────────────────────────────────────────────────

    std::string ShaderGraphCompiler::GenerateFragmentShader(
        const ShaderGraph& graph,
        const std::vector<const ShaderGraphNode*>& sortedNodes,
        std::vector<ShaderGraphParameterInfo>& outParameters) const
    {
        OLO_PROFILE_FUNCTION();

        std::ostringstream frag;
        frag << "#type fragment\n";
        frag << "#version 460 core\n\n";

        // MRT outputs
        frag << "layout(location = 0) out vec4 o_Color;\n";
        frag << "layout(location = 1) out int o_EntityID;\n";
        frag << "layout(location = 2) out vec2 o_ViewNormal;\n\n";

        // Varyings from vertex shader
        frag << "layout(location = 0) in vec3 v_WorldPosition;\n";
        frag << "layout(location = 1) in vec3 v_Normal;\n";
        frag << "layout(location = 2) in vec2 v_TexCoord;\n\n";

        // Camera UBO (for view direction, entity ID)
        frag << "layout(std140, binding = 0) uniform CameraMatrices\n";
        frag << "{\n";
        frag << "    mat4 u_ViewProjection;\n";
        frag << "    mat4 u_View;\n";
        frag << "    mat4 u_Projection;\n";
        frag << "    vec4 u_CameraPosition;\n";
        frag << "};\n\n";

        // Model UBO (for entity ID)
        frag << "layout(std140, binding = 3) uniform ModelMatrices\n";
        frag << "{\n";
        frag << "    mat4 u_Model;\n";
        frag << "    mat4 u_Normal;\n";
        frag << "    int u_EntityID;\n";
        frag << "    float _pad0;\n";
        frag << "    float _pad1;\n";
        frag << "    float _pad2;\n";
        frag << "};\n\n";

        // Collect and emit user parameter uniforms
        // Use a UBO for non-opaque uniforms, standalone binding for samplers
        int nextTextureBinding = ShaderBindingLayout::TEX_SHADER_GRAPH_0; // Start past engine-reserved texture slots

        // First pass: collect parameter info
        struct ParamInfo
        {
            std::string Name;
            ShaderGraphPinType Type;
            ShaderGraphPinValue Default;
        };
        std::vector<ParamInfo> scalarParams;
        std::vector<ParamInfo> textureParams;

        for (const auto* node : sortedNodes)
        {
            if (node->ParameterName.empty())
                continue;
            if (node->TypeName == ShaderGraphNodeTypes::Texture2DParameter)
            {
                textureParams.push_back({ node->ParameterName, ShaderGraphPinType::Texture2D, {} });
            }
            else if (node->TypeName == ShaderGraphNodeTypes::FloatParameter || node->TypeName == ShaderGraphNodeTypes::Vec3Parameter || node->TypeName == ShaderGraphNodeTypes::Vec4Parameter || node->TypeName == ShaderGraphNodeTypes::ColorParameter)
            {
                ShaderGraphPinValue defaultVal;
                if (!node->Outputs.empty())
                    defaultVal = node->Outputs[0].DefaultValue;
                scalarParams.push_back({ node->ParameterName, node->Outputs[0].Type, defaultVal });
            }
        }

        // Check if Time node is used
        bool usesTime = false;
        for (const auto* node : sortedNodes)
        {
            if (node->TypeName == ShaderGraphNodeTypes::Time)
            {
                usesTime = true;
                break;
            }
        }

        // Emit scalar parameter UBO (non-conflicting with engine bindings)
        if (!scalarParams.empty() || usesTime)
        {
            frag << "layout(std140, binding = " << ShaderBindingLayout::UBO_SHADER_GRAPH << ") uniform ShaderGraphParams\n";
            frag << "{\n";
            for (const auto& param : scalarParams)
            {
                frag << "    " << PinTypeToGLSL(param.Type) << " " << param.Name << ";\n";
                // std140 padding for vec3
                if (param.Type == ShaderGraphPinType::Vec3)
                    frag << "    float " << param.Name << "_pad;\n";

                outParameters.push_back({ param.Name, param.Type, param.Default });
            }
            if (usesTime)
            {
                frag << "    float u_Time;\n";
            }
            frag << "};\n\n";
        }

        // Emit texture samplers (GL 4.6 guarantees 80 combined units; engine uses 0-31)
        constexpr int maxShaderGraphTextures = 48; // slots 32-79
        if (static_cast<int>(textureParams.size()) > maxShaderGraphTextures)
        {
            OLO_CORE_ERROR("ShaderGraphCompiler: Too many texture parameters ({}, max {})", textureParams.size(), maxShaderGraphTextures);
        }
        for (const auto& param : textureParams)
        {
            frag << "layout(binding = " << nextTextureBinding << ") uniform sampler2D " << param.Name << ";\n";
            outParameters.push_back({ param.Name, ShaderGraphPinType::Texture2D, {} });
            nextTextureBinding++;
        }
        if (!textureParams.empty())
            frag << "\n";

        // Octahedral normal encoding function for SSAO MRT output
        frag << "vec2 octEncode(vec3 n)\n";
        frag << "{\n";
        frag << "    n /= (abs(n.x) + abs(n.y) + abs(n.z));\n";
        frag << "    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy);\n";
        frag << "    return n.xy * 0.5 + 0.5;\n";
        frag << "}\n\n";

        // Main function
        frag << "void main()\n";
        frag << "{\n";

        // Generate code for each node in topological order
        std::unordered_map<u64, std::string> pinVarNames;

        const ShaderGraphNode* outputNode = nullptr;
        for (const auto* node : sortedNodes)
        {
            if (node->TypeName == ShaderGraphNodeTypes::PBROutput)
            {
                outputNode = node;
                continue; // Handle last
            }
            std::string nodeCode = GenerateNodeCode(graph, *node, pinVarNames);
            if (!nodeCode.empty())
                frag << nodeCode;
        }

        // Write PBR output
        if (outputNode)
        {
            auto resolve = [&](const std::string& name) -> std::string
            {
                const auto* pin = outputNode->FindPinByName(name, ShaderGraphPinDirection::Input);
                if (!pin)
                    return "0.0";
                return ResolveInputExpression(graph, *pin, pinVarNames);
            };

            std::string albedo = resolve("Albedo");
            std::string metallic = resolve("Metallic");
            std::string roughness = resolve("Roughness");
            std::string ao = resolve("AO");
            std::string emissive = resolve("Emissive");
            std::string alpha = resolve("Alpha");
            std::string normal = resolve("Normal");

            // Simple direct lighting approximation (ambient + diffuse)
            // Full PBR is handled by the engine's PBR lighting pass
            frag << "\n    // PBR Output\n";
            frag << "    vec3 sg_albedo = " << albedo << ";\n";
            frag << "    float sg_metallic = " << metallic << ";\n";
            frag << "    float sg_roughness = " << roughness << ";\n";
            frag << "    float sg_ao = " << ao << ";\n";
            frag << "    vec3 sg_emissive = " << emissive << ";\n";
            frag << "    float sg_alpha = " << alpha << ";\n";
            frag << "    vec3 sg_normal = " << normal << ";\n";
            frag << "\n";
            frag << "    // Output color (engine lighting pass will apply full PBR)\n";
            frag << "    o_Color = vec4(sg_albedo * sg_ao + sg_emissive, sg_alpha);\n";
            frag << "    o_EntityID = u_EntityID;\n";
            frag << "    vec3 viewNormal = normalize(mat3(u_View) * sg_normal);\n";
            frag << "    o_ViewNormal = octEncode(viewNormal);\n";
        }

        frag << "}\n";

        return frag.str();
    }

    // ─────────────────────────────────────────────────────────────
    //  Main compile entry point
    // ─────────────────────────────────────────────────────────────

    ShaderGraphCompileResult ShaderGraphCompiler::Compile(const ShaderGraph& graph) const
    {
        OLO_PROFILE_FUNCTION();

        ShaderGraphCompileResult result;

        // Validate the graph
        auto validation = graph.Validate();
        if (!validation.IsValid)
        {
            result.Success = false;
            for (const auto& err : validation.Errors)
                result.ErrorLog += "Error: " + err + "\n";
            for (const auto& warn : validation.Warnings)
                result.ErrorLog += "Warning: " + warn + "\n";
            return result;
        }

        // Get topological order
        auto sortedNodes = graph.GetTopologicalOrder();
        if (sortedNodes.empty())
        {
            result.Success = false;
            result.ErrorLog = "Error: Failed to produce topological ordering (possible cycle)\n";
            return result;
        }

        // Determine graph type and generate accordingly
        if (IsComputeGraph(graph))
        {
            result.IsCompute = true;
            result.ShaderSource = GenerateComputeShader(graph, sortedNodes, result.ExposedParameters);
        }
        else
        {
            std::string vertexShader = GenerateVertexShader();
            std::string fragmentShader = GenerateFragmentShader(graph, sortedNodes, result.ExposedParameters);
            result.ShaderSource = vertexShader + "\n" + fragmentShader;
        }

        result.Success = true;

        // Append warnings
        for (const auto& warn : validation.Warnings)
            result.ErrorLog += "Warning: " + warn + "\n";

        return result;
    }

    // ─────────────────────────────────────────────────────────────
    //  Compute graph detection
    // ─────────────────────────────────────────────────────────────

    bool ShaderGraphCompiler::IsComputeGraph(const ShaderGraph& graph)
    {
        for (const auto& node : graph.GetNodes())
        {
            if (node->TypeName == ShaderGraphNodeTypes::ComputeOutput)
                return true;
        }
        return false;
    }

    // ─────────────────────────────────────────────────────────────
    //  Compute shader generation
    // ─────────────────────────────────────────────────────────────

    std::string ShaderGraphCompiler::GenerateComputeShader(
        const ShaderGraph& graph,
        const std::vector<const ShaderGraphNode*>& sortedNodes,
        std::vector<ShaderGraphParameterInfo>& outParams) const
    {
        OLO_PROFILE_FUNCTION();

        std::ostringstream cs;

        // Find the ComputeOutput node for workgroup size
        const ShaderGraphNode* outputNode = nullptr;
        for (const auto& node : graph.GetNodes())
        {
            if (node->TypeName == ShaderGraphNodeTypes::ComputeOutput)
            {
                outputNode = node.get();
                break;
            }
        }

        glm::ivec3 wgSize = outputNode ? outputNode->WorkgroupSize : glm::ivec3(16, 16, 1);

        cs << "#version 460 core\n\n";
        cs << "layout(local_size_x = " << wgSize.x
           << ", local_size_y = " << wgSize.y
           << ", local_size_z = " << wgSize.z << ") in;\n\n";

        // Declare SSBOs from BufferInput / BufferStore nodes
        for (const auto* node : sortedNodes)
        {
            if (node->TypeName == ShaderGraphNodeTypes::ComputeBufferInput ||
                node->TypeName == ShaderGraphNodeTypes::ComputeBufferStore)
            {
                std::string bufName = node->ParameterName.empty()
                                          ? (node->TypeName == ShaderGraphNodeTypes::ComputeBufferInput ? "inputBuffer" : "outputBuffer")
                                          : node->ParameterName;
                cs << "layout(std430, binding = " << node->BufferBinding << ") buffer SSBO_" << bufName << "\n";
                cs << "{\n";
                cs << "    float data[];\n";
                cs << "} " << bufName << ";\n\n";
            }
        }

        // Emit uniform declarations for exposed parameters (Float/Vec params)
        for (const auto* node : sortedNodes)
        {
            if (node->TypeName == ShaderGraphNodeTypes::FloatParameter ||
                node->TypeName == ShaderGraphNodeTypes::Vec3Parameter ||
                node->TypeName == ShaderGraphNodeTypes::Vec4Parameter ||
                node->TypeName == ShaderGraphNodeTypes::ColorParameter)
            {
                std::string uniformName = node->ParameterName;
                std::string type;
                if (node->TypeName == ShaderGraphNodeTypes::FloatParameter)
                    type = "float";
                else if (node->TypeName == ShaderGraphNodeTypes::Vec3Parameter || node->TypeName == ShaderGraphNodeTypes::ColorParameter)
                    type = "vec3";
                else
                    type = "vec4";

                cs << "uniform " << type << " " << uniformName << ";\n";

                ShaderGraphParameterInfo param;
                param.Name = node->ParameterName;
                if (node->TypeName == ShaderGraphNodeTypes::FloatParameter)
                    param.Type = ShaderGraphPinType::Float;
                else if (node->TypeName == ShaderGraphNodeTypes::Vec3Parameter || node->TypeName == ShaderGraphNodeTypes::ColorParameter)
                    param.Type = ShaderGraphPinType::Vec3;
                else
                    param.Type = ShaderGraphPinType::Vec4;
                outParams.push_back(param);
            }
        }
        cs << "\n";

        // Generate main()
        cs << "void main()\n{\n";

        // Build variable name map and emit node code
        std::unordered_map<u64, std::string> pinVarNames;
        for (const auto* node : sortedNodes)
        {
            if (node->TypeName == ShaderGraphNodeTypes::ComputeOutput)
                continue; // Output node has no code to emit

            std::string nodeCode = GenerateNodeCode(graph, *node, pinVarNames);
            if (!nodeCode.empty())
                cs << nodeCode;
        }

        cs << "}\n";

        return cs.str();
    }

} // namespace OloEngine
