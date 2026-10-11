// ps5/tests' stand-in for rex/logging.h: the tests link memory_ps5.cpp alone,
// without the runtime's logging, so its errors go to standard error, and on
// the console into the test's results file too (a payload's standard error
// reaches only the terminal that sent it).
#pragma once
#include <cstdio>
#include <string>

#include <fmt/format.h>

#ifndef MEMORY_PS5_TEST_RESULTS
#if defined(MEMORY_PS5_TEST_EMBEDDED)
#define MEMORY_PS5_TEST_RESULTS "/app0/rexglue-memory-test.txt"  // the title's folder
#else
#define MEMORY_PS5_TEST_RESULTS "/data/rexglue-memory-test.txt"
#endif
#endif

inline void rex_test_error(const std::string& line) {
  std::fputs((line + "\n").c_str(), stderr);
#if defined(__PROSPERO__)
  if (FILE* file = std::fopen(MEMORY_PS5_TEST_RESULTS, "a")) {
    std::fprintf(file, "ERROR %s\n", line.c_str());
    std::fclose(file);
  }
#endif
}

#define REXSYS_ERROR(...) rex_test_error(fmt::format(__VA_ARGS__))
