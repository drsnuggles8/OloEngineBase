#pragma once

namespace OloEngine
{
	typedef enum class KeyCode : uint16_t
	{
		// From glfw3.h
		Space = 32,
		Apostrophe = 39, /* ' */
		Comma = 44, /* , */
		Minus = 45, /* - */
		Period = 46, /* . */
		Slash = 47, /* / */

		D0 = 48, /* 0 */
		D1 = 49, /* 1 */
		D2 = 50, /* 2 */
		D3 = 51, /* 3 */
		D4 = 52, /* 4 */
		D5 = 53, /* 5 */
		D6 = 54, /* 6 */
		D7 = 55, /* 7 */
		D8 = 56, /* 8 */
		D9 = 57, /* 9 */

		Semicolon = 59, /* ; */
		Equal = 61, /* = */

		A = 65,
		B = 66,
		C = 67,
		D = 68,
		E = 69,
		F = 70,
		G = 71,
		H = 72,
		I = 73,
		J = 74,
		K = 75,
		L = 76,
		M = 77,
		N = 78,
		O = 79,
		P = 80,
		Q = 81,
		R = 82,
		S = 83,
		T = 84,
		U = 85,
		V = 86,
		W = 87,
		X = 88,
		Y = 89,
		Z = 90,

		LeftBracket = 91,  /* [ */
		Backslash = 92,  /* \ */
		RightBracket = 93,  /* ] */
		GraveAccent = 96,  /* ` */

		World1 = 161, /* non-US #1 */
		World2 = 162, /* non-US #2 */

		/* Function keys */
		Escape = 256,
		Enter = 257,
		Tab = 258,
		Backspace = 259,
		Insert = 260,
		Delete = 261,
		Right = 262,
		Left = 263,
		Down = 264,
		Up = 265,
		PageUp = 266,
		PageDown = 267,
		Home = 268,
		End = 269,
		CapsLock = 280,
		ScrollLock = 281,
		NumLock = 282,
		PrintScreen = 283,
		Pause = 284,
		F1 = 290,
		F2 = 291,
		F3 = 292,
		F4 = 293,
		F5 = 294,
		F6 = 295,
		F7 = 296,
		F8 = 297,
		F9 = 298,
		F10 = 299,
		F11 = 300,
		F12 = 301,
		F13 = 302,
		F14 = 303,
		F15 = 304,
		F16 = 305,
		F17 = 306,
		F18 = 307,
		F19 = 308,
		F20 = 309,
		F21 = 310,
		F22 = 311,
		F23 = 312,
		F24 = 313,
		F25 = 314,

		/* Keypad */
		KP0 = 320,
		KP1 = 321,
		KP2 = 322,
		KP3 = 323,
		KP4 = 324,
		KP5 = 325,
		KP6 = 326,
		KP7 = 327,
		KP8 = 328,
		KP9 = 329,
		KPDecimal = 330,
		KPDivide = 331,
		KPMultiply = 332,
		KPSubtract = 333,
		KPAdd = 334,
		KPEnter = 335,
		KPEqual = 336,

		LeftShift = 340,
		LeftControl = 341,
		LeftAlt = 342,
		LeftSuper = 343,
		RightShift = 344,
		RightControl = 345,
		RightAlt = 346,
		RightSuper = 347,
		Menu = 348
	} Key;

	inline std::ostream& operator<<(std::ostream& os, KeyCode keyCode)
	{
		os << static_cast<int32_t>(keyCode);
		return os;
	}
}

// From glfw3.h
#define OLO_KEY_SPACE           ::OloEngine::Key::Space
#define OLO_KEY_APOSTROPHE      ::OloEngine::Key::Apostrophe    /* ' */
#define OLO_KEY_COMMA           ::OloEngine::Key::Comma         /* , */
#define OLO_KEY_MINUS           ::OloEngine::Key::Minus         /* - */
#define OLO_KEY_PERIOD          ::OloEngine::Key::Period        /* . */
#define OLO_KEY_SLASH           ::OloEngine::Key::Slash         /* / */
#define OLO_KEY_0               ::OloEngine::Key::D0
#define OLO_KEY_1               ::OloEngine::Key::D1
#define OLO_KEY_2               ::OloEngine::Key::D2
#define OLO_KEY_3               ::OloEngine::Key::D3
#define OLO_KEY_4               ::OloEngine::Key::D4
#define OLO_KEY_5               ::OloEngine::Key::D5
#define OLO_KEY_6               ::OloEngine::Key::D6
#define OLO_KEY_7               ::OloEngine::Key::D7
#define OLO_KEY_8               ::OloEngine::Key::D8
#define OLO_KEY_9               ::OloEngine::Key::D9
#define OLO_KEY_SEMICOLON       ::OloEngine::Key::Semicolon     /* ; */
#define OLO_KEY_EQUAL           ::OloEngine::Key::Equal         /* = */
#define OLO_KEY_A               ::OloEngine::Key::A
#define OLO_KEY_B               ::OloEngine::Key::B
#define OLO_KEY_C               ::OloEngine::Key::C
#define OLO_KEY_D               ::OloEngine::Key::D
#define OLO_KEY_E               ::OloEngine::Key::E
#define OLO_KEY_F               ::OloEngine::Key::F
#define OLO_KEY_G               ::OloEngine::Key::G
#define OLO_KEY_H               ::OloEngine::Key::H
#define OLO_KEY_I               ::OloEngine::Key::I
#define OLO_KEY_J               ::OloEngine::Key::J
#define OLO_KEY_K               ::OloEngine::Key::K
#define OLO_KEY_L               ::OloEngine::Key::L
#define OLO_KEY_M               ::OloEngine::Key::M
#define OLO_KEY_N               ::OloEngine::Key::N
#define OLO_KEY_O               ::OloEngine::Key::O
#define OLO_KEY_P               ::OloEngine::Key::P
#define OLO_KEY_Q               ::OloEngine::Key::Q
#define OLO_KEY_R               ::OloEngine::Key::R
#define OLO_KEY_S               ::OloEngine::Key::S
#define OLO_KEY_T               ::OloEngine::Key::T
#define OLO_KEY_U               ::OloEngine::Key::U
#define OLO_KEY_V               ::OloEngine::Key::V
#define OLO_KEY_W               ::OloEngine::Key::W
#define OLO_KEY_X               ::OloEngine::Key::X
#define OLO_KEY_Y               ::OloEngine::Key::Y
#define OLO_KEY_Z               ::OloEngine::Key::Z
#define OLO_KEY_LEFT_BRACKET    ::OloEngine::Key::LeftBracket   /* [ */
#define OLO_KEY_BACKSLASH       ::OloEngine::Key::Backslash     /* \ */
#define OLO_KEY_RIGHT_BRACKET   ::OloEngine::Key::RightBracket  /* ] */
#define OLO_KEY_GRAVE_ACCENT    ::OloEngine::Key::GraveAccent   /* ` */
#define OLO_KEY_WORLD_1         ::OloEngine::Key::World1        /* non-US #1 */
#define OLO_KEY_WORLD_2         ::OloEngine::Key::World2        /* non-US #2 */

/* Function keys */
#define OLO_KEY_ESCAPE          ::OloEngine::Key::Escape
#define OLO_KEY_ENTER           ::OloEngine::Key::Enter
#define OLO_KEY_TAB             ::OloEngine::Key::Tab
#define OLO_KEY_BACKSPACE       ::OloEngine::Key::Backspace
#define OLO_KEY_INSERT          ::OloEngine::Key::Insert
#define OLO_KEY_DELETE          ::OloEngine::Key::Delete
#define OLO_KEY_RIGHT           ::OloEngine::Key::Right
#define OLO_KEY_LEFT            ::OloEngine::Key::Left
#define OLO_KEY_DOWN            ::OloEngine::Key::Down
#define OLO_KEY_UP              ::OloEngine::Key::Up
#define OLO_KEY_PAGE_UP         ::OloEngine::Key::PageUp
#define OLO_KEY_PAGE_DOWN       ::OloEngine::Key::PageDown
#define OLO_KEY_HOME            ::OloEngine::Key::Home
#define OLO_KEY_END             ::OloEngine::Key::End
#define OLO_KEY_CAPS_LOCK       ::OloEngine::Key::CapsLock
#define OLO_KEY_SCROLL_LOCK     ::OloEngine::Key::ScrollLock
#define OLO_KEY_NUM_LOCK        ::OloEngine::Key::NumLock
#define OLO_KEY_PRINT_SCREEN    ::OloEngine::Key::PrintScreen
#define OLO_KEY_PAUSE           ::OloEngine::Key::Pause
#define OLO_KEY_F1              ::OloEngine::Key::F1
#define OLO_KEY_F2              ::OloEngine::Key::F2
#define OLO_KEY_F3              ::OloEngine::Key::F3
#define OLO_KEY_F4              ::OloEngine::Key::F4
#define OLO_KEY_F5              ::OloEngine::Key::F5
#define OLO_KEY_F6              ::OloEngine::Key::F6
#define OLO_KEY_F7              ::OloEngine::Key::F7
#define OLO_KEY_F8              ::OloEngine::Key::F8
#define OLO_KEY_F9              ::OloEngine::Key::F9
#define OLO_KEY_F10             ::OloEngine::Key::F10
#define OLO_KEY_F11             ::OloEngine::Key::F11
#define OLO_KEY_F12             ::OloEngine::Key::F12
#define OLO_KEY_F13             ::OloEngine::Key::F13
#define OLO_KEY_F14             ::OloEngine::Key::F14
#define OLO_KEY_F15             ::OloEngine::Key::F15
#define OLO_KEY_F16             ::OloEngine::Key::F16
#define OLO_KEY_F17             ::OloEngine::Key::F17
#define OLO_KEY_F18             ::OloEngine::Key::F18
#define OLO_KEY_F19             ::OloEngine::Key::F19
#define OLO_KEY_F20             ::OloEngine::Key::F20
#define OLO_KEY_F21             ::OloEngine::Key::F21
#define OLO_KEY_F22             ::OloEngine::Key::F22
#define OLO_KEY_F23             ::OloEngine::Key::F23
#define OLO_KEY_F24             ::OloEngine::Key::F24
#define OLO_KEY_F25             ::OloEngine::Key::F25

/* Keypad */
#define OLO_KEY_KP_0            ::OloEngine::Key::KP0
#define OLO_KEY_KP_1            ::OloEngine::Key::KP1
#define OLO_KEY_KP_2            ::OloEngine::Key::KP2
#define OLO_KEY_KP_3            ::OloEngine::Key::KP3
#define OLO_KEY_KP_4            ::OloEngine::Key::KP4
#define OLO_KEY_KP_5            ::OloEngine::Key::KP5
#define OLO_KEY_KP_6            ::OloEngine::Key::KP6
#define OLO_KEY_KP_7            ::OloEngine::Key::KP7
#define OLO_KEY_KP_8            ::OloEngine::Key::KP8
#define OLO_KEY_KP_9            ::OloEngine::Key::KP9
#define OLO_KEY_KP_DECIMAL      ::OloEngine::Key::KPDecimal
#define OLO_KEY_KP_DIVIDE       ::OloEngine::Key::KPDivide
#define OLO_KEY_KP_MULTIPLY     ::OloEngine::Key::KPMultiply
#define OLO_KEY_KP_SUBTRACT     ::OloEngine::Key::KPSubtract
#define OLO_KEY_KP_ADD          ::OloEngine::Key::KPAdd
#define OLO_KEY_KP_ENTER        ::OloEngine::Key::KPEnter
#define OLO_KEY_KP_EQUAL        ::OloEngine::Key::KPEqual

#define OLO_KEY_LEFT_SHIFT      ::OloEngine::Key::LeftShift
#define OLO_KEY_LEFT_CONTROL    ::OloEngine::Key::LeftControl
#define OLO_KEY_LEFT_ALT        ::OloEngine::Key::LeftAlt
#define OLO_KEY_LEFT_SUPER      ::OloEngine::Key::LeftSuper
#define OLO_KEY_RIGHT_SHIFT     ::OloEngine::Key::RightShift
#define OLO_KEY_RIGHT_CONTROL   ::OloEngine::Key::RightControl
#define OLO_KEY_RIGHT_ALT       ::OloEngine::Key::RightAlt
#define OLO_KEY_RIGHT_SUPER     ::OloEngine::Key::RightSuper
#define OLO_KEY_MENU            ::OloEngine::Key::Menu
