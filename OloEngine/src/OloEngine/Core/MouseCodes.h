#pragma once

namespace OloEngine
{
	typedef enum class MouseCode : uint16_t
	{
		// From glfw3.h
		Button0 = 0,
		Button1 = 1,
		Button2 = 2,
		Button3 = 3,
		Button4 = 4,
		Button5 = 5,
		Button6 = 6,
		Button7 = 7,

		ButtonLast = Button7,
		ButtonLeft = Button0,
		ButtonRight = Button1,
		ButtonMiddle = Button2
	} Mouse;

	inline std::ostream& operator<<(std::ostream& os, MouseCode mouseCode)
	{
		os << static_cast<int32_t>(mouseCode);
		return os;
	}
}

#define OLO_MOUSE_BUTTON_0      ::OloEngine::Mouse::Button0
#define OLO_MOUSE_BUTTON_1      ::OloEngine::Mouse::Button1
#define OLO_MOUSE_BUTTON_2      ::OloEngine::Mouse::Button2
#define OLO_MOUSE_BUTTON_3      ::OloEngine::Mouse::Button3
#define OLO_MOUSE_BUTTON_4      ::OloEngine::Mouse::Button4
#define OLO_MOUSE_BUTTON_5      ::OloEngine::Mouse::Button5
#define OLO_MOUSE_BUTTON_6      ::OloEngine::Mouse::Button6
#define OLO_MOUSE_BUTTON_7      ::OloEngine::Mouse::Button7
#define OLO_MOUSE_BUTTON_LAST   ::OloEngine::Mouse::ButtonLast
#define OLO_MOUSE_BUTTON_LEFT   ::OloEngine::Mouse::ButtonLeft
#define OLO_MOUSE_BUTTON_RIGHT  ::OloEngine::Mouse::ButtonRight
#define OLO_MOUSE_BUTTON_MIDDLE ::OloEngine::Mouse::ButtonMiddle