newoption {
    trigger = "sanitize",
    value = "MODE",
    description = "Build AstraTest with a sanitizer (Linux/clang; used by the CI sanitizer lane)",
    allowed = {
        { "address", "AddressSanitizer + UndefinedBehaviorSanitizer" },
        { "thread",  "ThreadSanitizer" },
    }
}

workspace "Astra"
    architecture "x64"
    configurations { "Debug", "Release", "Dist" }
    startproject "AstraBenchmark"
    location "."  -- Solution file stays in root

    outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
    
    IncludeDir = {}
    IncludeDir["Astra"] = "include"
    IncludeDir["Mosaic"] = "vendor/Mosaic/include"
    IncludeDir["ImGui"] = "vendor/imgui"
    IncludeDir["GLFW"] = "vendor/glfw/include"
    IncludeDir["GoogleTest"] = "vendor/GoogleTest/googletest/include"
    IncludeDir["GoogleMock"] = "vendor/GoogleTest/googlemock/include"
    IncludeDir["GoogleBenchmark"] = "vendor/GoogleBenchmark/include"
    
    group "Dependencies"
        include "vendor/GoogleTest"
        include "vendor/GoogleBenchmark"
        include "vendor/imgui"
        include "vendor/glfw"
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

            removefiles
            {
                "tests/Compile/**.hpp",
                "tests/Compile/**.cpp"
            }

            includedirs
            {
                "%{IncludeDir.Astra}",
                "%{IncludeDir.Mosaic}",
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
                    "/arch:AVX",            -- Enable up to AVX (includes SSE4.2) - matching benchmark
                    "/diagnostics:column",  -- Show column info in errors
                    "/diagnostics:caret",   -- Show carets pointing to errors
                    "/bigobj",              -- Allow larger object files (helps with templates)
                    "/fp:fast",             -- Fast floating point - matching benchmark
                    "/openmp:experimental"  -- Enable OpenMP SIMD support
                }
                defines { 
                    "__SSE2__",             -- Define SSE2 support - matching benchmark
                    "__SSE4_2__"            -- Define SSE4.2 support (for CRC32) - matching benchmark
                }
                
            filter "system:linux"
                links { "pthread" }
                buildoptions {
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-fdiagnostics-color=always",
                    "-mavx"                 -- Enable AVX paths in Core/Simd.hpp (parity with /arch:AVX on MSVC)
                }

            filter "system:macosx"
                buildoptions {
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-fdiagnostics-color=always",
                    "-mavx"                 -- Enable AVX paths in Core/Simd.hpp (parity with /arch:AVX on MSVC)
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
                rtti "on"  -- GoogleTest requires RTTI
                defines { "ASTRA_BUILD_RELEASE", "NDEBUG", "GTEST_HAS_EXCEPTIONS=0" }
                
            filter "configurations:Dist"
                runtime "Release"
                optimize "full"
                symbols "off"
                exceptionhandling "off"
                rtti "on"
                defines { "ASTRA_BUILD_DIST", "NDEBUG", "GTEST_HAS_EXCEPTIONS=0" }

            -- Sanitizer lane (opt-in via --sanitize; Linux/clang). Option-gated so a
            -- normal generation with no --sanitize is byte-identical. staticruntime is
            -- forced off because static libstdc++ conflicts with the sanitizer runtimes'
            -- link-ordering requirement. Both compile AND link need the -fsanitize flags.
            filter { "options:sanitize=address" }
                staticruntime "off"
                buildoptions { "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all" }
                linkoptions  { "-fsanitize=address,undefined" }

            filter { "options:sanitize=thread" }
                staticruntime "off"
                buildoptions { "-fsanitize=thread" }
                linkoptions  { "-fsanitize=thread" }

            filter {}

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
                "%{IncludeDir.Mosaic}",
                "%{IncludeDir.GoogleBenchmark}",
                "benchmark",  -- For local includes
                "tests"       -- Reference pool reuse in benchmark tasks
            }
            
            links
            {
                "GoogleBenchmark"
            }
            
            defines
            {
                "BENCHMARK_STATIC_DEFINE"   -- Required when linking static benchmark lib
            }
            
            filter "system:windows"
                systemversion "latest"
                links { "shlwapi" }         -- Required by Google Benchmark for SHGetValueA
                buildoptions { 
                    "/Zc:__cplusplus",      -- Enable proper __cplusplus macro
                    "/arch:AVX",            -- Enable up to AVX (includes SSE4.2)
                    "/diagnostics:column",  -- Show column info in errors
                    "/diagnostics:caret",   -- Show carets pointing to errors
                    "/bigobj",              -- Allow larger object files
                    "/fp:fast",             -- Fast floating point
                    "/openmp:experimental"  -- Enable OpenMP SIMD support
                }
                defines { 
                    "__SSE2__",             -- Define SSE2 support
                    "__SSE4_2__"            -- Define SSE4.2 support (for CRC32)
                }
                
            filter "system:linux"
                links { "pthread" }
                -- NOTE: -march=native below is intentionally non-portable; benchmarks are
                -- local-machine-only and are not built in CI cross-compiler jobs.
                buildoptions {
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-fdiagnostics-color=always",
                    "-march=native",        -- Use native CPU features
                    "-msse2",               -- Enable SSE2
                    "-msse4.2",             -- Enable SSE4.2 (includes CRC32)
                    "-mpclmul",             -- Sometimes needed for CRC32
                    "-ffast-math",          -- Fast floating point
                    "-funroll-loops",       -- Unroll loops
                    "-ftree-vectorize",     -- Auto-vectorization
                    "-fopenmp"              -- Enable OpenMP SIMD support
                }
                
            filter "system:macosx"
                -- NOTE: -march=native below is intentionally non-portable; benchmarks are
                -- local-machine-only and are not built in CI cross-compiler jobs.
                buildoptions {
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-fdiagnostics-color=always",
                    "-march=native",        -- Use native CPU features
                    "-msse2",               -- Enable SSE2
                    "-msse4.2",             -- Enable SSE4.2
                    "-ffast-math",          -- Fast floating point
                    "-funroll-loops",       -- Unroll loops
                    "-ftree-vectorize",     -- Auto-vectorization
                    "-fopenmp"              -- Enable OpenMP SIMD support
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

        project "AstraStudio"
            kind "ConsoleApp"
            language "C++"
            cppdialect "C++20"
            staticruntime "on"
            location "ide"
            targetdir ("bin/" .. outputdir .. "/%{prj.name}")
            objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
            files { "studio/**.hpp", "studio/**.cpp" }
            includedirs {
                "%{IncludeDir.Astra}", "%{IncludeDir.Mosaic}",
                "%{IncludeDir.ImGui}", "%{IncludeDir.ImGui}/backends", "%{IncludeDir.GLFW}"
            }
            links { "ImGui", "GLFW" }
            filter "system:windows"
                systemversion "latest"
                links { "opengl32", "gdi32" }
                buildoptions { "/Zc:__cplusplus", "/arch:AVX", "/bigobj", "/fp:fast" }
                defines { "__SSE2__", "__SSE4_2__" }
            filter "system:linux"
                links { "GL", "X11", "pthread", "dl" }
                buildoptions { "-mavx" }
            filter "configurations:Debug"
                runtime "Debug"
                symbols "on"
                optimize "off"
                exceptionhandling "off"
                rtti "off"
                defines { "ASTRA_BUILD_DEBUG", "_DEBUG" }
            filter "configurations:Release"
                runtime "Release"
                optimize "speed"
                symbols "on"
                exceptionhandling "off"
                rtti "off"
                defines { "ASTRA_BUILD_RELEASE", "NDEBUG" }
            filter "configurations:Dist"
                runtime "Release"
                optimize "full"
                symbols "off"
                exceptionhandling "off"
                rtti "off"
                defines { "ASTRA_BUILD_DIST", "NDEBUG" }
            filter {}

        -- Compile checks: alternate entity widths must keep building.
        local function astraCompileCheck(name, sourceFile)
            project(name)
                kind "ConsoleApp"
                language "C++"
                cppdialect "C++20"
                staticruntime "on"
                location "ide"
                targetdir ("bin/" .. outputdir .. "/%{prj.name}")
                objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
                files { sourceFile }
                includedirs { "%{IncludeDir.Astra}", "%{IncludeDir.Mosaic}" }
                filter "system:windows"
                    systemversion "latest"
                    buildoptions { "/Zc:__cplusplus", "/arch:AVX", "/bigobj" }
                    defines { "__SSE2__", "__SSE4_2__" }
                    links { "advapi32" }
                filter "system:linux"
                    links { "pthread" }
                    buildoptions { "-mavx" }
                filter "configurations:Debug"
                    runtime "Debug"
                    symbols "on"
                    defines { "ASTRA_BUILD_DEBUG" }
                filter "configurations:Release or configurations:Dist"
                    runtime "Release"
                    optimize "speed"
                    defines { "NDEBUG" }
                filter {}
        end
        astraCompileCheck("AstraCompile16", "tests/Compile/Entity16Main.cpp")
        astraCompileCheck("AstraCompile64", "tests/Compile/Entity64Main.cpp")

    group ""
    