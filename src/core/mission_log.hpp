#pragma once

#include <cstdarg>
#include <cstdio>

namespace sl_open::diagnostics
{
inline void mission_log(const char* format, ...)
{
	std::fputs("[mission] ", stderr);
	std::va_list arguments;
	va_start(arguments, format);
	std::vfprintf(stderr, format, arguments);
	va_end(arguments);
	std::fputc('\n', stderr);
}
}
