#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include <string>

namespace OloEngine
{
	class AnimationGraphAsset : public Asset
	{
	public:
		AnimationGraphAsset() = default;

		static AssetType GetStaticType() { return AssetType::AnimationGraph; }
		AssetType GetAssetType() const override { return GetStaticType(); }

		void SetFilePath(const std::string& path) { m_FilePath = path; }
		[[nodiscard]] const std::string& GetFilePath() const { return m_FilePath; }

		void SetGraph(const Ref<AnimationGraph>& graph) { m_Graph = graph; }
		[[nodiscard]] Ref<AnimationGraph> GetGraph() const { return m_Graph; }

	private:
		std::string m_FilePath;
		Ref<AnimationGraph> m_Graph;
	};
} // namespace OloEngine
