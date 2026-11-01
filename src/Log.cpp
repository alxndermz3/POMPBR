#include "Log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace
{
	std::mutex g_Lock;
	std::string g_Path;
}

std::string Log::PluginDirectory()
{
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(&Log::PluginDirectory), &self);
	char path[MAX_PATH] = {};
	GetModuleFileNameA(self, path, MAX_PATH);
	std::string dir(path);
	return dir.substr(0, dir.find_last_of("\\/"));
}

void Log::Init()
{
	std::lock_guard<std::mutex> lock(g_Lock);
	g_Path = PluginDirectory() + "\\POM.log";
	if (FILE* f = fopen(g_Path.c_str(), "w"))
		fclose(f);
}

void Log::Write(const char* format, ...)
{
	char message[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	SYSTEMTIME t;
	GetLocalTime(&t);

	std::lock_guard<std::mutex> lock(g_Lock);
	if (g_Path.empty())
		return;
	if (FILE* f = fopen(g_Path.c_str(), "a"))
	{
		fprintf(f, "%02d:%02d:%02d  %s\n", t.wHour, t.wMinute, t.wSecond, message);
		fclose(f);
	}
}
