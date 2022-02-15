#pragma once

#include "OloEngine/Core/PlatformDetection.h"

#ifdef OLO_PLATFORM_WINDOWS
	#ifndef NOMINMAX
		// See github.com/skypjack/entt/wiki/Frequently-Asked-Questions#warning-c4003-the-min-the-max-and-the-macro
		#define NOMINMAX
	#endif
#endif

#include <iostream>
#include <memory>
#include <utility>
#include <algorithm>
#include <functional>

#include <string>
#include <sstream>
#include <array>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "OloEngine/Core/Base.h"

#include "OloEngine/Core/Log.h"

#include "OloEngine/Debug/Instrumentor.h"

#ifdef OLO_PLATFORM_WINDOWS
	#include <Windows.h>
#endif
