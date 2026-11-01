#include <Windows.h>

#include "Engine.h"
#include "Log.h"
#include "MinHook.h"
#include "Overlay.h"

namespace
{
	DWORD WINAPI MainThread(LPVOID)
	{
		Log::Init();
		// Let the game finish creating its device and the other plugins install their hooks first.
		Sleep(8000);
		const MH_STATUS init = MH_Initialize();
		if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
		{
			Log::Write("MinHook failed to initialize: %s", MH_StatusToString(init));
			return 0;
		}
		if (!Pom::Engine::Install())
		{
			Log::Write("not installed: see the lines above");
			return 0;
		}
		Log::Write("ready: CTRL+F8 toggles, settings in POM.ini (applied on save), textures in Plugins\\POM");
		Pom::Overlay::Start();
		return 0;
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		// Off the loader lock.
		CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
	}
	return TRUE;
}
