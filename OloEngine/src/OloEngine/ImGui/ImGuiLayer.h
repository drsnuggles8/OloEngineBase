#pragma once

#include "OloEngine/Core/Layer.h"

#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Events/KeyEvent.h"
#include "OloEngine/Events/MouseEvent.h"

namespace OloEngine
{
	class ImGuiLayer : public Layer
	{
	public:
		ImGuiLayer();
		~ImGuiLayer() = default;

		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnImGuiRender() override;

		void Begin();
		void End();
	private:
		float m_Time = 0.0f;
	};
}