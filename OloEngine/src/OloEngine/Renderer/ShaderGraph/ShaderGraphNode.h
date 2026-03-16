#pragma once

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphTypes.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphPin.h"

#include <glm/glm.hpp>
#include <functional>
#include <string>
#include <vector>

namespace OloEngine
{
    /// Base structure for all shader graph nodes.
    /// Each concrete node type is identified by its TypeName string
    /// and knows how to generate its GLSL code fragment.
    struct ShaderGraphNode
    {
        UUID ID;
        std::string TypeName;
        ShaderGraphNodeCategory Category = ShaderGraphNodeCategory::Math;
        glm::vec2 EditorPosition = glm::vec2(0.0f);
        std::vector<ShaderGraphPin> Inputs;
        std::vector<ShaderGraphPin> Outputs;

        /// Name of a user-defined parameter (only used by parameter nodes)
        std::string ParameterName;

        /// User-defined GLSL expression (only used by CustomFunction nodes)
        std::string CustomFunctionBody;

        /// Workgroup size (only used by ComputeOutput nodes)
        glm::ivec3 WorkgroupSize = glm::ivec3(16, 16, 1);

        /// Buffer binding index (only used by ComputeBufferInput/Store nodes)
        int BufferBinding = 0;

        virtual ~ShaderGraphNode() = default;

        /// Find a pin by its UUID
        ShaderGraphPin* FindPin(UUID pinID);
        const ShaderGraphPin* FindPin(UUID pinID) const;

        /// Find a pin by name and direction
        ShaderGraphPin* FindPinByName(const std::string& name, ShaderGraphPinDirection direction);
        const ShaderGraphPin* FindPinByName(const std::string& name, ShaderGraphPinDirection direction) const;
    };

    // ─────────────────────────────────────────────────────────────
    //  Node Factory
    // ─────────────────────────────────────────────────────────────

    /// Creates a fully initialized node of the given type name.
    /// Returns nullptr if the type name is unknown.
    Scope<ShaderGraphNode> CreateShaderGraphNode(const std::string& typeName);

    /// Returns all registered node type names
    std::vector<std::string> GetAllNodeTypeNames();

    /// Returns node type names filtered by category
    std::vector<std::string> GetNodeTypeNamesByCategory(ShaderGraphNodeCategory category);

    // ─────────────────────────────────────────────────────────────
    //  Concrete Node Type Names (constants)
    // ─────────────────────────────────────────────────────────────

    namespace ShaderGraphNodeTypes
    {
        // Output
        inline constexpr const char* PBROutput = "PBROutput";

        // Input / Parameter
        inline constexpr const char* FloatParameter = "FloatParameter";
        inline constexpr const char* FloatConstant = "FloatConstant";
        inline constexpr const char* Vec3Parameter = "Vec3Parameter";
        inline constexpr const char* Vec3Constant = "Vec3Constant";
        inline constexpr const char* ColorParameter = "ColorParameter";
        inline constexpr const char* Vec4Parameter = "Vec4Parameter";
        inline constexpr const char* Texture2DParameter = "Texture2DParameter";
        inline constexpr const char* Time = "Time";
        inline constexpr const char* UV = "UV";
        inline constexpr const char* WorldPosition = "WorldPosition";
        inline constexpr const char* WorldNormal = "WorldNormal";

        // Math
        inline constexpr const char* Add = "Add";
        inline constexpr const char* Subtract = "Subtract";
        inline constexpr const char* Multiply = "Multiply";
        inline constexpr const char* Divide = "Divide";
        inline constexpr const char* Lerp = "Lerp";
        inline constexpr const char* Clamp = "Clamp";
        inline constexpr const char* Dot = "Dot";
        inline constexpr const char* Cross = "Cross";
        inline constexpr const char* Normalize = "Normalize";
        inline constexpr const char* Power = "Power";
        inline constexpr const char* Abs = "Abs";
        inline constexpr const char* Floor = "Floor";
        inline constexpr const char* Ceil = "Ceil";
        inline constexpr const char* Fract = "Fract";
        inline constexpr const char* Sin = "Sin";
        inline constexpr const char* Cos = "Cos";
        inline constexpr const char* OneMinus = "OneMinus";
        inline constexpr const char* Split = "Split";
        inline constexpr const char* Combine = "Combine";
        inline constexpr const char* Step = "Step";
        inline constexpr const char* Smoothstep = "Smoothstep";

        // Texture
        inline constexpr const char* SampleTexture2D = "SampleTexture2D";
        inline constexpr const char* NormalMap = "NormalMap";

        // Utility
        inline constexpr const char* Fresnel = "Fresnel";
        inline constexpr const char* TilingOffset = "TilingOffset";

        // Custom
        inline constexpr const char* CustomFunction = "CustomFunction";

        // Compute
        inline constexpr const char* ComputeOutput = "ComputeOutput";
        inline constexpr const char* ComputeBufferInput = "ComputeBufferInput";
        inline constexpr const char* ComputeBufferStore = "ComputeBufferStore";
        inline constexpr const char* WorkgroupID = "WorkgroupID";
        inline constexpr const char* LocalInvocationID = "LocalInvocationID";
        inline constexpr const char* GlobalInvocationID = "GlobalInvocationID";
    } // namespace ShaderGraphNodeTypes

} // namespace OloEngine
