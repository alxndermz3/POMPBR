@echo off
rem Builds the plugin and copies it into REDACTED\Plugins (close the game first: a loaded plugin is locked).
rem Plugins\POM isn't touched: it holds what's been converted in game.
rem Set VSROOT and REDACTED if they aren't where they are here.
if not defined VSROOT set "VSROOT=%USERPROFILE%\Documents\Visual Studio"
if not defined REDACTED set "REDACTED=%USERPROFILE%\Desktop\Everything\Games\REDACTED"
"%VSROOT%\MSBuild\Current\Bin\MSBuild.exe" "%~dp0POM.vcxproj" -p:Configuration=Release -p:Platform=Win32 -v:minimal -nologo
if errorlevel 1 exit /b 1
copy /y "%~dp0build\POM.dll" "%REDACTED%\Plugins\POM.Red32n"
