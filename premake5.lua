workspace "OloEngine"
	architecture "x64"

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

include "OloEngine/vendor/GLFW"


project "OloEngine"
	location "OloEngine"
	kind "SharedLib"
	language "C++"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	pchheader "OloEnginePCH.h"
	pchsource "OloEngine/src/OloEnginePCH.cpp"

	files
	{
		"%{prj.name}/src/**.h",
		"%{prj.name}/src/**.cpp"
	}

	includedirs
	{
		"%{prj.name}/src",
		"%{prj.name}/vendor/spdlog/include",
		"%{IncludeDir.GLFW}"
	}
	
	links
	{
		"GLFW",
		"opengl32.lib"
	}

	filter "system:windows"
		cppdialect "C++20"
		staticruntime "On"
		systemversion "latest"

		defines
		{
			"OLO_PLATFORM_WINDOWS",
			"OLO_BUILD_DLL"
		}

		postbuildcommands
		{
			("{COPY} %{cfg.buildtarget.relpath} ../bin/" .. outputdir .. "/Sandbox")
		}
		
	filter "configurations:Debug"
		defines "OLO_DEBUG"
		symbols "On"

	filter "configurations:Release"
		defines "OLO_RELEASE"
		optimize "On"

	filter "configurations:Dist"
		defines "OLO_DIST"
		optimize "On"


project "Sandbox"
	location "Sandbox"
	kind "ConsoleApp"

	language "C++"

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
		"OloEngine/src"
	}

	links
	{
		"OloEngine"
	}

	filter "system:windows"
		cppdialect "C++20"
		staticruntime "On"
		systemversion "latest"

		defines
		{
			"OLO_PLATFORM_WINDOWS"
		}

	filter "configurations:Debug"
		defines "OLO_DEBUG"
		symbols "On"

	filter "configurations:Release"
		defines "OLO_RELEASE"
		optimize "On"

	filter "configurations:Dist"
		defines "OLO_DIST"
		optimize "On"