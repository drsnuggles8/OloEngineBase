include "./vendor/premake/premake_customization/solution_items.lua"

workspace "OloEngine"
	architecture "x86_64"
	startproject "Olo-Editor"

	configurations
	{
		"Debug",
		"Release",
		"Dist"
	}

	solution_items
	{
		".editorconfig"
	}

		flags
	{
		"MultiProcessorCompile"
	}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Include directories relative to root folder (solution directory)
IncludeDir = {}
IncludeDir["GLFW"] = "%{wks.location}/OloEngine/vendor/GLFW/include"
IncludeDir["Glad"] = "%{wks.location}/OloEngine/vendor/Glad/include"
IncludeDir["ImGui"] = "%{wks.location}/OloEngine/vendor/imgui"
IncludeDir["glm"] = "%{wks.location}/OloEngine/vendor/glm"
IncludeDir["stb_image"] = "%{wks.location}/OloEngine/vendor/stb_image"
IncludeDir["entt"] = "%{wks.location}/OloEngine/vendor/entt/include"

group "Dependencies"
	include "vendor/premake"
	include "OloEngine/vendor/GLFW"
	include "OloEngine/vendor/Glad"
	include "OloEngine/vendor/imgui"
group ""

include "OloEngine"
include "Sandbox"
include "Olo-Editor"
