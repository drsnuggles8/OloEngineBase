#include "OloEnginePCH.h"

#include <misc/cpp/imgui_stdlib.cpp>

// #define _CRT_SECURE_NO_WARNINGS
#define IMGUI_IMPL_OPENGL_LOADER_GLAD
#include <backends/imgui_impl_glfw.cpp>
#include <backends/imgui_impl_opengl3.cpp>

// The Vulkan renderer backend lives in its OWN TU (ImGuiBuildVulkan.cpp,
// #691): sharing this unity TU with the GLFW/OpenGL3 backends
// poisoned its include state (VkResult enum redefinition against the pinned
// vulkan_core.h), and a dedicated TU that includes <volk.h> FIRST is the
// same include-order rule every Platform/Vulkan TU already follows
// (ADR 0011 amendment 41).
