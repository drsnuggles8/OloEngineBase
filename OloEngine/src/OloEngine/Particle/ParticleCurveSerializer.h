#pragma once

#include "OloEngine/Particle/ParticleCurve.h"

#include <yaml-cpp/yaml.h>

namespace OloEngine::ParticleCurveSerializer
{
	inline void Serialize(YAML::Emitter& out, const std::string& name, const ParticleCurve& curve)
	{
		out << YAML::Key << name << YAML::Value << YAML::BeginMap;
		u32 safeCount = std::min(curve.KeyCount, static_cast<u32>(curve.Keys.size()));
		out << YAML::Key << "KeyCount" << YAML::Value << safeCount;
		out << YAML::Key << "Keys" << YAML::Value << YAML::BeginSeq;
		for (u32 i = 0; i < safeCount; ++i)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "Time" << YAML::Value << curve.Keys[i].Time;
			out << YAML::Key << "Value" << YAML::Value << curve.Keys[i].Value;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::EndMap;
	}

	inline void Deserialize(const YAML::Node& node, ParticleCurve& curve)
	{
		if (!node || !node.IsMap())
		{
			return;
		}
		if (auto kc = node["KeyCount"]; kc)
		{
			curve.KeyCount = std::min(kc.as<u32>(), static_cast<u32>(curve.Keys.size()));
		}
		if (auto keys = node["Keys"]; keys && keys.IsSequence())
		{
			u32 count = std::min(static_cast<u32>(keys.size()), static_cast<u32>(curve.Keys.size()));
			curve.KeyCount = count;
			for (u32 i = 0; i < count; ++i)
			{
				if (auto timeNode = keys[i]["Time"]; timeNode)
				{
					curve.Keys[i].Time = timeNode.as<f32>(curve.Keys[i].Time);
				}
				if (auto valueNode = keys[i]["Value"]; valueNode)
				{
					curve.Keys[i].Value = valueNode.as<f32>(curve.Keys[i].Value);
				}
			}
		}
	}

	inline void Serialize4(YAML::Emitter& out, const std::string& name, const ParticleCurve4& curve)
	{
		out << YAML::Key << name << YAML::Value << YAML::BeginMap;
		Serialize(out, "R", curve.R);
		Serialize(out, "G", curve.G);
		Serialize(out, "B", curve.B);
		Serialize(out, "A", curve.A);
		out << YAML::EndMap;
	}

	inline void Deserialize4(const YAML::Node& node, ParticleCurve4& curve)
	{
		if (!node || !node.IsMap())
		{
			return;
		}
		Deserialize(node["R"], curve.R);
		Deserialize(node["G"], curve.G);
		Deserialize(node["B"], curve.B);
		Deserialize(node["A"], curve.A);
	}
} // namespace OloEngine::ParticleCurveSerializer
