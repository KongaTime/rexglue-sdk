// ps5/tests' stand-in for rex/logging.h: the tests link memory_ps5.cpp alone,
// without the runtime's logging, so its errors go to standard error.
#pragma once
#include <cstdio>
#include <fmt/format.h>
#define REXSYS_ERROR(...) (std::fputs((fmt::format(__VA_ARGS__) + "\n").c_str(), stderr))
