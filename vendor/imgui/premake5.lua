project "ImGui"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "on"
    location "../../ide"
    targetdir ("../../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../../bin-int/" .. outputdir .. "/%{prj.name}")
    files {
        "imgui.cpp", "imgui_demo.cpp", "imgui_draw.cpp",
        "imgui_tables.cpp", "imgui_widgets.cpp",
        "backends/imgui_impl_glfw.cpp", "backends/imgui_impl_opengl3.cpp",
        "*.h", "backends/imgui_impl_glfw.h", "backends/imgui_impl_opengl3.h"
    }
    includedirs { ".", "../glfw/include" }
    filter "system:windows"
        systemversion "latest"
    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"
    filter "configurations:Release or configurations:Dist"
        runtime "Release"
        optimize "speed"
    filter {}
