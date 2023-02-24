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
		const auto state = GLFWAPI::glfwGetKey(window, static_cast<i32>(key));
		return GLFW_PRESS == state;
	}

	bool Input::IsMouseButtonPressed(const MouseCode button)
	{
		auto* const window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		const auto state = GLFWAPI::glfwGetMouseButton(window, static_cast<i32>(button));
		return GLFW_PRESS == state;
	}

	glm::vec2 Input::GetMousePosition()
	{
		auto* const window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		f64 xpos {};
		f64 ypos {};
		GLFWAPI::glfwGetCursorPos(window, &xpos, &ypos);

		return { static_cast<f32>(xpos), static_cast<f32>(ypos) };
	}

	f32 Input::GetMouseX()
	{
		return GetMousePosition().x;
	}

	f32 Input::GetMouseY()
	{
		return GetMousePosition().y;
	}
}
