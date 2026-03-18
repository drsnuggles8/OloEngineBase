#include "OloEnginePCH.h"
#include "FacialExpressionLibrary.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <yaml-cpp/yaml.h>

#include <fstream>

namespace OloEngine
{
    bool FacialExpressionLibrary::SaveExpression(const std::string& filePath, const FacialExpression& expr)
    {
        OLO_PROFILE_FUNCTION();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "FacialExpression" << YAML::BeginMap;
        out << YAML::Key << "Name" << YAML::Value << expr.Name;
        out << YAML::Key << "TargetWeights" << YAML::BeginMap;
        for (const auto& [target, weight] : expr.TargetWeights)
        {
            out << YAML::Key << target << YAML::Value << weight;
        }
        out << YAML::EndMap; // TargetWeights
        out << YAML::EndMap; // FacialExpression
        out << YAML::EndMap;

        if (!out.good())
        {
            OLO_CORE_ERROR("FacialExpressionLibrary: YAML emitter error for '{}': {}", filePath, out.GetLastError());
            return false;
        }

        std::ofstream fout(filePath);
        if (!fout)
        {
            OLO_CORE_ERROR("FacialExpressionLibrary: Failed to write '{}'", filePath);
            return false;
        }
        fout << out.c_str();
        if (!fout.good())
        {
            OLO_CORE_ERROR("FacialExpressionLibrary: I/O error writing '{}'", filePath);
            return false;
        }
        return true;
    }

    bool FacialExpressionLibrary::LoadExpression(const std::string& filePath)
    {
        OLO_PROFILE_FUNCTION();

        YAML::Node root;
        try
        {
            root = YAML::LoadFile(filePath);
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("FacialExpressionLibrary: Failed to parse '{}': {}", filePath, e.what());
            return false;
        }

        auto exprNode = root["FacialExpression"];
        if (!exprNode)
        {
            OLO_CORE_ERROR("FacialExpressionLibrary: Missing 'FacialExpression' key in '{}'", filePath);
            return false;
        }

        FacialExpression expr;
        expr.Name = exprNode["Name"].as<std::string>("");
        if (expr.Name.empty())
        {
            OLO_CORE_ERROR("FacialExpressionLibrary: Expression in '{}' has no name", filePath);
            return false;
        }

        if (auto weightsNode = exprNode["TargetWeights"]; weightsNode && weightsNode.IsMap())
        {
            for (auto it = weightsNode.begin(); it != weightsNode.end(); ++it)
            {
                expr.TargetWeights[it->first.as<std::string>()] = it->second.as<f32>(0.0f);
            }
        }

        RegisterExpression(expr);
        return true;
    }
} // namespace OloEngine
