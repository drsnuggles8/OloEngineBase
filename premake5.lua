workspace "OloEngine"
	architecture "x86_64"
	startproject "Sandbox"

	configurations
	{
		"Debug",
		"Release",
		"Dist"
	}

	flags
	{
		"MultiProcessorCompile"
	}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Include directories relative to root folder (solution directory)
IncludeDir = {}
IncludeDir["GLFW"] = "OloEngine/vendor/GLFW/include"
IncludeDir["Glad"] = "OloEngine/vendor/Glad/include"
IncludeDir["ImGui"] = "OloEngine/vendor/imgui"
IncludeDir["glm"] = "OloEngine/vendor/glm"
IncludeDir["stb_image"] = "OloEngine/vendor/stb_image"

group "Dependencies"
	include "OloEngine/vendor/GLFW"
	include "OloEngine/vendor/Glad"
	include "OloEngine/vendor/imgui"
group ""

project "OloEngine"
	location "OloEngine"
	kind "StaticLib"
	language "C++"
	cppdialect "C++20"
	staticruntime "on"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	pchheader "OloEnginePCH.h"
	pchsource "OloEngine/src/OloEnginePCH.cpp"

	files
	{
		"%{prj.name}/src/**.h",
		"%{prj.name}/src/**.cpp",
		"%{prj.name}/vendor/stb_image/**.h",
		"%{prj.name}/vendor/stb_image/**.cpp",
		"%{prj.name}/vendor/glm/glm/**.hpp",
		"%{prj.name}/vendor/glm/glm/**.inl"
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
		"GLFW_INCLUDE_NONE"
	}

	includedirs
	{
		"%{prj.name}/src",
		"%{prj.name}/vendor/spdlog/include",
		"%{IncludeDir.GLFW}",
		"%{IncludeDir.Glad}",
		"%{IncludeDir.ImGui}",
		"%{IncludeDir.glm}",
		"%{IncludeDir.stb_image}"
	}
	
	links
	{
		"GLFW",
		"Glad",
		"opengl32.lib",
		"ImGui"
	}

	filter "system:windows"
		systemversion "latest"

		
	filter "configurations:Debug"
		defines "OLO_DEBUG"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		defines "OLO_RELEASE"
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		defines "OLO_DIST"
		runtime "Release"
		optimize "on"


project "Sandbox"
	location "Sandbox"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++20"
	staticruntime "on"	

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"%{prj.name}/src/**.h",
		"%{prj.name}/src/**.cpp"
	}

	includedirs
	{
		"OloEngine/vendor/spdlog/include",
		"OloEngine/src",
		"OloEngine/vendor",
		"%{IncludeDir.glm}"
	}

	links
	{
		"OloEngine"
	}

	filter "system:windows"
		systemversion "latest"

	filter "configurations:Debug"
		defines "OLO_DEBUG"		
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		defines "OLO_RELEASE"
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		defines "OLO_DIST"
		runtime "Release"
		optimize "on"