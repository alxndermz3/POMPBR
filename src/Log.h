#pragma once
#include <string>

// POM.log next to the plugin, rewritten every game start.
namespace Log
{
	// The folder the plugin was loaded from (REDACTED\Plugins).
	std::string PluginDirectory();
	void Init();
	void Write(const char* format, ...);
}
