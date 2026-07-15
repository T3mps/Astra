-- Mosaic standalone workspace -- the shared Starworks Core (pluggable seams +
-- primitives), zero external dependencies. Builds the library and its Catch2
-- test suite from the vendored dep; self-contained (the vendored premake5.exe
-- generates this). Static-CRT throughout -- there is no DLL boundary here; each
-- consumer (Astra / Manifold2D / Arcane) builds Mosaic via its own project.

workspace "Mosaic"
    architecture "x64"
    configurations { "Debug", "Release", "Dist" }
    startproject "MosaicTests"
    location "."

    outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

    IncludeDir = {}
    IncludeDir["Mosaic"] = "include"
    IncludeDir["Catch2"] = "vendor/Catch2/src"

    -- The vendored Catch2 wrapper honors these globals (static CRT default).
    THIRDPARTY_STATICRUNTIME    = "on"
    THIRDPARTY_PROJECT_LOCATION = "ide"

    filter "system:windows"
        systemversion "latest"
    filter "system:linux"                    -- Part B scaffold (exercised later)
        buildoptions { "-mavx2", "-ffp-contract=off", "-fno-fast-math", "-Wall", "-Wextra" }
        links { "pthread" }
    filter {}

    group "Dependencies"
        include "vendor/Catch2"
    group ""

    project "Mosaic"
        kind "StaticLib"
        language "C++"
        cppdialect "C++23"
        location "ide"
        staticruntime "on"
        floatingpoint "Strict"               -- determinism (Simd/Wide) -- /fp:fast banned
        targetdir ("bin/" .. outputdir .. "/%{prj.name}")
        objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

        files { "include/**.hpp", "include/**.inl", "src/**.cpp" }
        includedirs { "%{IncludeDir.Mosaic}" }
        defines { "_CRT_SECURE_NO_WARNINGS" }

        filter "system:windows"
            buildoptions { "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
        filter "configurations:Debug"
            runtime "Debug"   symbols "on"                  defines { "MOSAIC_DEBUG" }
        filter "configurations:Release"
            runtime "Release" optimize "speed" symbols "on" defines { "MOSAIC_RELEASE", "NDEBUG" }
        filter "configurations:Dist"
            runtime "Release" optimize "speed" symbols "off" defines { "MOSAIC_DIST", "NDEBUG" }
        filter {}

    project "MosaicTests"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++23"
        location "ide"
        staticruntime "on"
        floatingpoint "Strict"
        targetdir ("bin/" .. outputdir .. "/%{prj.name}")
        objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

        files { "tests/**.hpp", "tests/**.inl", "tests/**.cpp" }
        includedirs { "%{IncludeDir.Mosaic}", "%{IncludeDir.Catch2}", "tests" }
        links { "Mosaic", "Catch2" }
        defines { "_CRT_SECURE_NO_WARNINGS" }

        filter "system:windows"
            buildoptions { "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
        filter "configurations:Debug"
            runtime "Debug"   symbols "on"                  defines { "MOSAIC_DEBUG" }
        filter "configurations:Release"
            runtime "Release" optimize "speed" symbols "on"
        filter "configurations:Dist"
            runtime "Release" optimize "speed" symbols "off"
        filter {}
