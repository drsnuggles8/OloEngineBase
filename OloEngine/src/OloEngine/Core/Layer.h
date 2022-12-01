#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Events/Event.h"

#include <string>

namespace OloEngine
{
	class Layer
	{
	public:
		explicit Layer(std::string  name = "Layer");
		virtual ~Layer() = default;

		virtual void OnAttach() {}
		virtual void OnDetach() {}
		virtual void OnUpdate(Timestep const) {}
		virtual void OnImGuiRender() {}
		virtual void OnEvent(Event&) {}

		[[nodiscard("Store this!")]] const std::string& GetName() const { return m_DebugName; }
	private:
		std::string m_DebugName;
	};
}
