--Premake5 build configuration for Astra project Cross-platform Linux And MAC GCC hotfixes
require "premake-LinkTimeModule"

workspace "Astra"

--------- END OF HOTFIX ---------
    configurations { "Debug", "Release", "Dist" }
    startproject "AstraBenchmark"
    location "."  -- Solution file stays in root

    outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
    
    IncludeDir = {}
    IncludeDir["Astra"] = "include"
    IncludeDir["GoogleTest"] = "vendor/GoogleTest/googletest/include"
    IncludeDir["GoogleMock"] = "vendor/GoogleTest/googlemock/include"
    IncludeDir["GoogleBenchmark"] = "vendor/GoogleBenchmark/include"
    
    group "Dependencies"
        include "vendor/GoogleTest"
        include "vendor/GoogleBenchmark"
    group ""
    
    group "Astra"
        project "Astra"
            kind "None"  -- Header-only library
            language "C++"
            cppdialect "C++20"
            location "ide"  -- Project files go to ide folder
            
            targetdir ("bin/" .. outputdir .. "/%{prj.name}")
            objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
            
            files
            {
                "include/Astra/**.hpp",
                "include/Astra/**.cpp"
            }
        
        project "AstraTest"
            kind "ConsoleApp"
            language "C++"
            cppdialect "C++20"
            staticruntime "on"
            location "ide"  -- Project files go to ide folder
            
            targetdir ("bin/" .. outputdir .. "/%{prj.name}")
            objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
            
            files
            {
                "tests/**.hpp",
                "tests/**.cpp"
            }
            
            includedirs
            {
                "%{IncludeDir.Astra}",
                "%{IncludeDir.GoogleTest}",
                "%{IncludeDir.GoogleMock}"
            }
            
            links
            {
                "GoogleTest"
            }
            
            filter "system:windows"
                systemversion "latest"
                buildoptions { 
                    "/Zc:__cplusplus",      -- Enable proper __cplusplus macro
                    "/diagnostics:column",  -- Show column info in errors
                    "/diagnostics:caret",   -- Show carets pointing to errors
                    "/bigobj"               -- Allow larger object files (helps with templates)
                }
                
            filter "system:linux"
                links { "pthread" }
                buildoptions { 
                    "-Wall", 
                    "-Wextra", 
                    "-Wpedantic",
                    "-fdiagnostics-color=always",
                    "-ftemplate-backtrace-limit=0"
                }
                
            filter "system:macosx"
                buildoptions { 
                    "-Wall", 
                    "-Wextra", 
                    "-Wpedantic",
                    "-fdiagnostics-color=always",
                    "-ftemplate-backtrace-limit=0"
                }
            
            filter "configurations:Debug"
                runtime "Debug"
                symbols "on"
                optimize "off"
                exceptionhandling "off"  -- Astra is exception-free
                rtti "on"                -- GoogleTest requires RTTI
                defines { "ASTRA_BUILD_DEBUG", "_DEBUG", "GTEST_HAS_EXCEPTIONS=0" }
                
            filter "configurations:Release"
                runtime "Release"
                optimize "speed"
                symbols "on"
                exceptionhandling "off"
                rtti "on"
                defines { "ASTRA_BUILD_RELEASE", "NDEBUG", "GTEST_HAS_EXCEPTIONS=0" }
                
            filter "configurations:Dist"
                runtime "Release"
                optimize "full"
                symbols "off"
                exceptionhandling "off"
                rtti "on"
                defines { "ASTRA_BUILD_DIST", "NDEBUG", "GTEST_HAS_EXCEPTIONS=0" }
        
        project "AstraBenchmark"
            kind "ConsoleApp"
            language "C++"
            cppdialect "C++20"
            staticruntime "on"
            location "ide"  -- Project files go to ide folder
            
            targetdir ("bin/" .. outputdir .. "/%{prj.name}")
            objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
            
            files
            {
                "benchmark/**.hpp",
                "benchmark/**.cpp"
            }
            
            includedirs
            {
                "%{IncludeDir.Astra}",
                "%{IncludeDir.GoogleBenchmark}",
                "benchmark"  -- For local includes
            }
            
            links
            {
                "GoogleBenchmark"
            }
            
            defines
            {
                "BENCHMARK_STATIC_DEFINE"  -- Required when linking static benchmark lib
            }
            
            filter "system:windows"
                systemversion "latest"
                links { "shlwapi" }  -- Required by Google Benchmark for SHGetValueA
                buildoptions { 
                    "/Zc:__cplusplus",      -- Enable proper __cplusplus macro
                    "/arch:AVX",            -- Enable up to AVX (includes SSE4.2)
                    "/diagnostics:column",  -- Show column info in errors
                    "/diagnostics:caret",   -- Show carets pointing to errors
                    "/bigobj",              -- Allow larger object files
                    "/fp:fast"              -- Fast floating point
                }
                defines { 
                    "__SSE2__",             -- Define SSE2 support
                    "__SSE4_2__"            -- Define SSE4.2 support (for CRC32)
                }
                
            filter "system:linux"
                links { "pthread" }
                buildoptions { 
                    "-Wall", 
                    "-Wextra", 
                    "-Wpedantic",
                    "-fdiagnostics-color=always",
                    "-ftemplate-backtrace-limit=0",
                    "-march=native",        -- Use native CPU features
                    "-msse2",               -- Enable SSE2
                    "-msse4.2",             -- Enable SSE4.2 (includes CRC32)
                    "-mpclmul",             -- Sometimes needed for CRC32
                    "-ffast-math",          -- Fast floating point
                    "-funroll-loops",       -- Unroll loops
                    "-ftree-vectorize"      -- Auto-vectorization
                }
                
            filter "system:macosx"
                buildoptions { 
                    "-Wall", 
                    "-Wextra", 
                    "-Wpedantic",
                    "-fdiagnostics-color=always",
                    "-ftemplate-backtrace-limit=0",
                    "-march=native",        -- Use native CPU features
                    "-msse2",               -- Enable SSE2
                    "-msse4.2",             -- Enable SSE4.2
                    "-ffast-math",          -- Fast floating point
                    "-funroll-loops",       -- Unroll loops
                    "-ftree-vectorize"      -- Auto-vectorization
                }
            
            filter "configurations:Debug"
                runtime "Debug"
                symbols "on"
                optimize "off"
                exceptionhandling "off"
                rtti "off"
                defines { "ASTRA_BUILD_DEBUG" }
                
            filter "configurations:Release"
                runtime "Release"
                optimize "full"
                symbols "on"  -- Keep symbols for profiling
                exceptionhandling "off"
                rtti "off"
                defines { "ASTRA_BUILD_RELEASE", "NDEBUG" }
                
                filter { "configurations:Release", "system:windows" }
                    buildoptions { "/O2", "/Oi", "/Ot", "/Oy", "/GL" }
                    linkoptions { "/LTCG", "/DEBUG" }
                    
                filter { "configurations:Release", "system:linux or system:macosx" }
                    buildoptions { "-O3", "-flto" }
                    linkoptions { "-flto" }
                
            filter "configurations:Dist"
                runtime "Release"
                optimize "full"
                symbols "off"
                exceptionhandling "off"
                rtti "off"
                defines { "ASTRA_BUILD_DIST", "NDEBUG" }
                
                filter { "configurations:Dist", "system:windows" }
                    buildoptions { "/O2", "/Oi", "/Ot", "/Oy", "/GL" }
                    linkoptions { "/LTCG" }
                    linktimeoptimization "On"
                    
                filter { "configurations:Dist", "system:linux or system:macosx" }
                    buildoptions { "-O3", "-flto", "-fomit-frame-pointer" }
                    linkoptions { "-flto", "-s" }  -- -s strips symbols
                    
    group ""
    