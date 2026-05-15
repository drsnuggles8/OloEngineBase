// =============================================================================
// ShaderStageInterfaceTest.cpp
//
// Catches the silent class of OloEditor breakage where a shader's vertex
// outputs and fragment inputs disagree on `layout(location = N)` or type.
// GLSL doesn't link vertex/fragment stages at compile time (they compile
// independently), so a mismatched interface produces a shader that builds
// fine but reads garbage from the wrong interpolant slot at draw time.
//
// Concrete failure modes this catches
// -----------------------------------
//   1. Vertex declares `layout(location = 2) out vec3 v_Normal`, fragment
//      declares `layout(location = 2) in vec2 v_Normal`. Type drift.
//   2. Vertex declares `layout(location = 3) out vec3 v_WorldPos`, fragment
//      declares `layout(location = 4) in vec3 v_WorldPos`. Location drift.
//   3. Fragment declares `layout(location = 5) in float v_Foo` for an
//      interpolant the vertex stage never writes. Reads undefined.
//
// Scope
// -----
//   Only shaders with both `#type vertex` and `#type fragment` stages are
//   checked. Geometry and tessellation stages re-pack interpolants and
//   would need a dedicated stage-pair check; out of scope here.
//
// Classification: shaderpipe.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <spirv_cross/spirv_cross.hpp>

#include "ShaderHarness.h"

#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace SH = ShaderHarness;

        struct StageVar
        {
            u32 Location;
            spirv_cross::SPIRType::BaseType BaseType;
            u32 VecSize;
            u32 Columns;
            std::string Name;
        };

        std::string DescribeType(const StageVar& v)
        {
            // Render the variable type as a developer would write it in GLSL.
            std::string base;
            switch (v.BaseType)
            {
                case spirv_cross::SPIRType::Float:
                    base = "float";
                    break;
                case spirv_cross::SPIRType::Int:
                    base = "int";
                    break;
                case spirv_cross::SPIRType::UInt:
                    base = "uint";
                    break;
                case spirv_cross::SPIRType::Boolean:
                    base = "bool";
                    break;
                default:
                    base = "?";
                    break;
            }
            if (v.Columns > 1)
            {
                std::ostringstream os;
                os << "mat" << v.Columns << "x" << v.VecSize;
                return os.str();
            }
            if (v.VecSize == 1)
                return base;
            if (v.BaseType == spirv_cross::SPIRType::Float)
                return "vec" + std::to_string(v.VecSize);
            if (v.BaseType == spirv_cross::SPIRType::Int)
                return "ivec" + std::to_string(v.VecSize);
            if (v.BaseType == spirv_cross::SPIRType::UInt)
                return "uvec" + std::to_string(v.VecSize);
            return base + std::to_string(v.VecSize);
        }

        std::map<u32, StageVar> CollectStageVars(
            const spirv_cross::Compiler& refl,
            const spirv_cross::SmallVector<spirv_cross::Resource>& resources)
        {
            std::map<u32, StageVar> byLocation;
            for (const auto& res : resources)
            {
                // Built-ins (gl_Position, gl_VertexID, …) don't decorate a
                // Location and aren't relevant here.
                if (!refl.has_decoration(res.id, spv::DecorationLocation))
                    continue;
                const u32 loc = refl.get_decoration(res.id, spv::DecorationLocation);
                const auto& type = refl.get_type(res.type_id);
                byLocation[loc] = StageVar{
                    loc,
                    type.basetype,
                    type.vecsize,
                    type.columns,
                    res.name,
                };
            }
            return byLocation;
        }

        struct InterfaceFailure
        {
            std::string ShaderPath;
            std::string Detail;
        };
    } // namespace

    TEST(ShaderStageInterface, VertexOutputsMatchFragmentInputs)
    {
        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const auto shaders = SH::EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty());

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        u32 pairsChecked = 0;
        std::vector<InterfaceFailure> failures;

        for (const auto& path : shaders)
        {
            const std::string source = SH::ReadWholeFile(path);
            auto stages = SH::SplitByType(source);

            // Find vertex + fragment stages within the same file. Skip
            // files with geometry / tess stages (interpolant re-packing is
            // out of scope).
            std::optional<std::string> vertexSrc;
            std::optional<std::string> fragmentSrc;
            bool hasGeomOrTess = false;
            for (const auto& [kind, src] : stages)
            {
                if (kind == shaderc_glsl_vertex_shader)
                    vertexSrc = src;
                else if (kind == shaderc_glsl_fragment_shader)
                    fragmentSrc = src;
                else if (kind == shaderc_glsl_geometry_shader ||
                         kind == shaderc_glsl_tess_control_shader ||
                         kind == shaderc_glsl_tess_evaluation_shader)
                    hasGeomOrTess = true;
            }
            if (hasGeomOrTess || !vertexSrc || !fragmentSrc)
                continue;

            auto vRes = SH::CompileStageToSpv(path, *vertexSrc, shaderc_glsl_vertex_shader, root, compiler);
            auto fRes = SH::CompileStageToSpv(path, *fragmentSrc, shaderc_glsl_fragment_shader, root, compiler);
            if (vRes.GetCompilationStatus() != shaderc_compilation_status_success ||
                fRes.GetCompilationStatus() != shaderc_compilation_status_success)
                continue;

            try
            {
                spirv_cross::Compiler vRefl(std::vector<u32>(vRes.cbegin(), vRes.cend()));
                spirv_cross::Compiler fRefl(std::vector<u32>(fRes.cbegin(), fRes.cend()));

                const auto vOuts = CollectStageVars(vRefl, vRefl.get_shader_resources().stage_outputs);
                const auto fIns = CollectStageVars(fRefl, fRefl.get_shader_resources().stage_inputs);
                ++pairsChecked;

                // Every fragment input must have a matching vertex output
                // at the same location with the same scalar/vector type.
                for (const auto& [loc, fIn] : fIns)
                {
                    const auto it = vOuts.find(loc);
                    if (it == vOuts.end())
                    {
                        std::ostringstream oss;
                        oss << "Fragment input at location " << loc
                            << " (name='" << fIn.Name << "', type=" << DescribeType(fIn)
                            << ") has no matching vertex output. Reads undefined interpolant.";
                        failures.push_back({ path.generic_string(), oss.str() });
                        continue;
                    }
                    const StageVar& vOut = it->second;
                    if (vOut.BaseType != fIn.BaseType || vOut.VecSize != fIn.VecSize ||
                        vOut.Columns != fIn.Columns)
                    {
                        std::ostringstream oss;
                        oss << "Interface type mismatch at location " << loc
                            << ": vertex out '" << vOut.Name << "' is "
                            << DescribeType(vOut)
                            << ", fragment in '" << fIn.Name << "' is "
                            << DescribeType(fIn) << ".";
                        failures.push_back({ path.generic_string(), oss.str() });
                    }
                }
            }
            catch (...)
            {
                // Reflection errors are the reflection test's problem.
            }
        }

        EXPECT_GT(pairsChecked, 0u);

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " vertex/fragment interface mismatch(es):\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.ShaderPath << ":\n  " << f.Detail << "\n";
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests
