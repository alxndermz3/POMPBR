@echo off
rem Builds build\pom_test.exe, build\pbr_test.exe and build\panel_test.exe. What each checks is at the top of its file in tests\.
rem Set VSROOT if Visual Studio isn't in Documents.
if not defined VSROOT set "VSROOT=%USERPROFILE%\Documents\Visual Studio"
call "%VSROOT%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
if not exist "%~dp0build\test-obj" mkdir "%~dp0build\test-obj"
if not exist "%~dp0build\pbr-obj" mkdir "%~dp0build\pbr-obj"
if not exist "%~dp0build\panel-obj" mkdir "%~dp0build\panel-obj"
cl /nologo /EHsc /O2 /MT /std:c++17 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /I"%~dp0src" /Fo"%~dp0build\test-obj\\" /Fe"%~dp0build\pom_test.exe" "%~dp0tests\pom_test.cpp" "%~dp0src\Dxbc.cpp" "%~dp0src\DxbcFormat.cpp" "%~dp0src\Brdf.cpp" "%~dp0src\PomFile.cpp" "%~dp0src\Bake.cpp" "%~dp0src\Decode.cpp" "%~dp0src\Picker.cpp" || exit /b 1
cl /nologo /EHsc /O2 /MT /std:c++17 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /I"%~dp0src" /Fo"%~dp0build\pbr-obj\\" /Fe"%~dp0build\pbr_test.exe" "%~dp0tests\pbr_test.cpp" "%~dp0tests\pbr_log.cpp" "%~dp0src\Reflect.cpp" "%~dp0src\Dxbc.cpp" "%~dp0src\DxbcFormat.cpp" "%~dp0src\Brdf.cpp" || exit /b 1
cl /nologo /EHsc /O2 /MT /std:c++17 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /I"%~dp0src" /I"%~dp0external\imgui" /I"%~dp0external\imgui\backends" /Fo"%~dp0build\panel-obj\\" /Fe"%~dp0build\panel_test.exe" "%~dp0tests\panel_test.cpp" "%~dp0src\Panel.cpp" "%~dp0src\Bake.cpp" "%~dp0src\PomFile.cpp" "%~dp0external\imgui\imgui.cpp" "%~dp0external\imgui\imgui_draw.cpp" "%~dp0external\imgui\imgui_tables.cpp" "%~dp0external\imgui\imgui_widgets.cpp" "%~dp0external\imgui\backends\imgui_impl_dx11.cpp" || exit /b 1
