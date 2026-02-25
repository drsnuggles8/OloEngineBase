#pragma once

#include "OloEngine/Core/PlatformDetection.h"

#ifdef OLO_PLATFORM_WINDOWS
#ifndef NOMINMAX
// See github.com/skypjack/entt/wiki/Frequently-Asked-Questions#warning-c4003-the-min-the-max-and-the-macro
#define NOMINMAX
#endif
#endif

#include <any>
#include <array>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/FilesystemUtils.h"
#include "OloEngine/Math/GLMFormatter.h"
#include "OloEngine/Debug/Instrumentor.h"

#ifdef OLO_PLATFORM_WINDOWS
#include "Platform/Windows/WindowsHWrapper.h"
#endif
