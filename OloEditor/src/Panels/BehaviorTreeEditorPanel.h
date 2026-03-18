#pragma once

#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
	class Scene;

	class BehaviorTreeEditorPanel
	{
	public:
		BehaviorTreeEditorPanel() = default;
		explicit BehaviorTreeEditorPanel(const Ref<Scene>& context);

		void SetContext(const Ref<Scene>& context);
		void OnImGuiRender();

	private:
		Ref<Scene> m_Context;
	};
} // namespace OloEngine
