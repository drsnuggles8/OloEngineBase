#pragma once

#include <glm/glm.hpp>

#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

namespace OloEngine {

	class Input
	{
	public:
		static bool IsKeyPressed(KeyCode key);

		static bool IsMouseButtonPressed(MouseCode button);
		static glm::vec2 GetMousePosition();
		static float GetMouseX();
		static float GetMouseY();
	};
}
