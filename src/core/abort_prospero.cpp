// Ported from TheSimpsonsGame-PS5-Builder (LoZazaMastro and contributors),
// tools/rexglue-sdk/src/core/abort_prospero.cpp at 59e7086, BSD 3-Clause
// License (see LICENSE); without its heap-failure hook (the port's own malloc).
//
// Keep the program's abort call site before libkernel replaces RIP with its
// own signal/abort context. The native abort still determines termination. A
// title links with --wrap=abort for this to see the calls (PS5_EdgeOfTimeTitle's
// reeot/reeot.cmake).
#include <cstdint>

#include <rex/platform/fault_prospero.h>

extern "C" [[noreturn]] void __real_abort();

extern "C" [[noreturn]] __attribute__((noinline)) void __wrap_abort() {
  const auto caller = reinterpret_cast<uintptr_t>(
      __builtin_extract_return_addr(__builtin_return_address(0)));
  rex::platform::WriteProsperoAbortCaller(caller);
  __real_abort();
}
