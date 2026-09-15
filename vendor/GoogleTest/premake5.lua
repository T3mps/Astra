project "GoogleTest"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "on"
    location "../../ide"  -- Project files go to ide folder
    
    targetdir ("../../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../../bin-int/" .. outputdir .. "/%{prj.name}")
    
    files
    {
        "googletest/src/gtest-all.cc",
        "googlemock/src/gmock-all.cc"
    }
    
    includedirs
    {
        "googletest",
        "googletest/include",
        "googlemock",
        "googlemock/include"
    }
    
    filter "system:windows"
        systemversion "latest"
        defines { "_SILENCE_TR1_NAMESPACE_DEPRECATION_WARNING", "GTEST_HAS_PTHREAD=0" }  -- no pthread on Windows
    
    filter "system:linux or system:macosx"
        defines { "GTEST_HAS_PTHREAD=1" }

    -- Astra's --no-rtti lane: keep GTEST_HAS_RTTI consistent between this
    -- library and the RTTI-off AstraTest translation units.
    filter "options:no-rtti"
        rtti "off"
        
    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"
        optimize "off"
        
    filter "configurations:Release"
        runtime "Release"
        optimize "speed"
        symbols "on"
        
    filter "configurations:Dist"
        runtime "Release"
        optimize "full"
        symbols "off"