workspace "OloEngine"
	architecture "x64"
	startproject "Sandbox"

	configurations
	{
		"Debug",
		"Release",
		"Dist"
	}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Include directories relative to root folder (solution directory)
IncludeDir = {}
IncludeDir["GLFW"] = "OloEngine/vendor/GLFW/include"
IncludeDir["Glad"] = "OloEngine/vendor/Glad/include"
IncludeDir["ImGui"] = "OloEngine/vendor/imgui"
IncludeDir["glm"] = "OloEngine/vendor/glm"

group "Dependencies"
	include "OloEngine/vendor/GLFW"
	include "OloEngine/vendor/Glad"
	include "OloEngine/vendor/imgui"
group ""

project "OloEngine"
	location "OloEngine"
	kind "SharedLib"
	language "C++"
	cppdialect "C++20"
	staticruntime "off"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	pchheader "OloEnginePCH.h"
	pchsource "OloEngine/src/OloEnginePCH.cpp"

	files
	{
		"%{prj.name}/src/**.h",
		"%{prj.name}/src/**.cpp",
		"%{prj.name}/vendor/glm/glm/**.hpp",
		"%{prj.name}/vendor/glm/glm/**.inl"
	}

	includedirs
	{
		"%{prj.name}/src",
		"%{prj.name}/vendor/spdlog/include",
		"%{IncludeDir.GLFW}",
		"%{IncludeDir.Glad}",
		"%{IncludeDir.ImGui}",
		"%{IncludeDir.glm}"
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

		defines
		{
			"OLO_PLATFORM_WINDOWS",
			"OLO_BUILD_DLL",
			"GLFW_INCLUDE_NONE"
		}

		postbuildcommands
		{
			("{COPY} %{cfg.buildtarget.relpath} \"../bin/" .. outputdir .. "/Sandbox/\"")
		}
		
	filter "configurations:Debug"
		defines "OLO_DEBUG"
		runtime "Debug"
		symbols "On"

	filter "configurations:Release"
		defines "OLO_RELEASE"
		runtime "Release"
		optimize "On"

	filter "configurations:Dist"
		defines "OLO_DIST"
		runtime "Release"
		optimize "On"


project "Sandbox"
	location "Sandbox"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++20"
	staticruntime "off"	

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
		"%{IncludeDir.glm}"
	}

	links
	{
		"OloEngine"
	}

	filter "system:windows"
		systemversion "latest"

		defines
		{
			"OLO_PLATFORM_WINDOWS"
		}

	filter "configurations:Debug"
		defines "OLO_DEBUG"		
		runtime "Debug"
		symbols "On"

	filter "configurations:Release"
		defines "OLO_RELEASE"
		runtime "Release"
		optimize "On"

	filter "configurations:Dist"
		defines "OLO_DIST"
		runtime "Release"
		optimize "On"