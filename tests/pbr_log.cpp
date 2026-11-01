// Log::Write for what's tested outside the game: it goes to the console.
#include <cstdarg>
#include <cstdio>

#include "../src/Log.h"

namespace Log
{
	void Write(const char* format, ...)
	{
		va_list args;
		va_start(args, format);
		printf("  [log] ");
		vprintf(format, args);
		printf("\n");
		va_end(args);
	}
}
