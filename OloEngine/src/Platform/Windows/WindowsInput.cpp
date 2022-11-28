// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "OloEngine/Core/Input.h"

#include "OloEngine/Core/Application.h"
#include <GLFW/glfw3.h>

namespace OloEngine
{
	bool Input::IsKeyPressed(const KeyCode key)
	{
		auto* const window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		const auto state = GLFWAPI::glfwGetKey(window, static_cast<int32_t>(key));
		return GLFW_PRESS == state;
	}

	bool Input::IsMouseButtonPressed(const MouseCode button)
	{
		auto* const window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		const auto state = GLFWAPI::glfwGetMouseButton(window, static_cast<int32_t>(button));
		return GLFW_PRESS == state;
	}

	glm::vec2 Input::GetMousePosition()
	{
		auto* const window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		double xpos {};
		double ypos {};
		GLFWAPI::glfwGetCursorPos(window, &xpos, &ypos);

		return { static_cast<float>(xpos), static_cast<float>(ypos) };
	}

	float Input::GetMouseX()
	{
		return GetMousePosition().x;
	}

	float Input::GetMouseY()
	{
		return GetMousePosition().y;
	}
}
