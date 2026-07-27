project "GLFW"
    kind "StaticLib"
    language "C"
    staticruntime "on"
    location "../../ide"
    targetdir ("../../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../../bin-int/" .. outputdir .. "/%{prj.name}")
    files {
        "include/GLFW/*.h",
        "src/context.c", "src/init.c", "src/input.c", "src/monitor.c",
        "src/platform.c", "src/vulkan.c", "src/window.c",
        "src/egl_context.c", "src/osmesa_context.c",
        "src/null_init.c", "src/null_joystick.c", "src/null_monitor.c", "src/null_window.c"
    }
    filter "system:windows"
        systemversion "latest"
        defines { "_GLFW_WIN32", "_CRT_SECURE_NO_WARNINGS" }
        files {
            "src/win32_init.c", "src/win32_joystick.c", "src/win32_module.c",
            "src/win32_monitor.c", "src/win32_thread.c", "src/win32_time.c",
            "src/win32_window.c", "src/wgl_context.c"
        }
    filter "system:linux"
        defines { "_GLFW_X11" }
        files {
            "src/x11_init.c", "src/x11_monitor.c", "src/x11_window.c",
            "src/xkb_unicode.c", "src/glx_context.c", "src/linux_joystick.c",
            "src/posix_module.c", "src/posix_poll.c", "src/posix_thread.c", "src/posix_time.c"
        }
    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"
    filter "configurations:Release or configurations:Dist"
        runtime "Release"
        optimize "speed"
    filter {}
