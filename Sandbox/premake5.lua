project "Sandbox"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++20"
	staticruntime "on"	

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"src/**.h",
		"src/**.cpp"
	}

	includedirs
	{
		"%{wks.location}/OloEngine/vendor/spdlog/include",
		"%{wks.location}/OloEngine/src",
		"%{wks.location}/OloEngine/vendor",
		"%{IncludeDir.glm}",
		"%{IncludeDir.entt}"
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
