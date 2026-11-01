@echo off
rem Compiles src\*.hlsl into headers, which are checked in, so plugin builds don't need this step.
rem Prologue.hlsl comes out once per kind of shader: light, twin, moved lookup. src\Dxbc.cpp picks one per shader.
rem Pbr.hlsl comes out once per place src\Brdf.cpp splices it into.
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\fxc.exe"
if not exist "%~dp0src\prologue" mkdir "%~dp0src\prologue"
if not exist "%~dp0src\pbr" mkdir "%~dp0src\pbr"
for %%L in (0 1 2) do for %%T in (0 1) do for %%M in (0 1) do (
	%FXC% /nologo /T ps_4_0 /E main /DLIGHT=%%L /DTWIN=%%T /DMOVE_LOOKUP=%%M /Vn g_Prologue_%%L_%%T_%%M /Fh "%~dp0src\prologue\p_%%L_%%T_%%M.h" "%~dp0src\Prologue.hlsl" >nul || exit /b 1
)
for %%S in (0 1 2 3 4 5 6 7) do (
	%FXC% /nologo /T ps_4_0 /E main /DSITE=%%S /Vn g_Site_%%S /Fh "%~dp0src\pbr\site_%%S.h" "%~dp0src\Pbr.hlsl" >nul || exit /b 1
)
%FXC% /nologo /T ps_4_0 /E main /Vn g_Highlight /Fh "%~dp0src\Highlight.h" "%~dp0src\Highlight.hlsl" >nul
