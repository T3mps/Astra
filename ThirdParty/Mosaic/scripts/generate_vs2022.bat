@echo off
REM Generate the Mosaic Visual Studio 2022 solution using the vendored
REM premake5.exe. Self-contained -- depends on no other repo. Emits a .sln
REM (not .slnx), which GitHub Actions windows-latest MSBuild can build.
"%~dp0..\vendor\premake5\premake5.exe" vs2022
