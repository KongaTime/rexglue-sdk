// Ported from TheSimpsonsGame-PS5-Builder (LoZazaMastro and contributors),
// tools/rexglue-sdk/include/rex/platform/fault_prospero.h at 59e7086, BSD
// 3-Clause License (see LICENSE). Without the port's trace ring, stack dump
// and heap-failure hook; with a frame-pointer backtrace of the fork's own.
//
// PS5 terminal-fault diagnostics: a fault no ReXGlue handler takes, and
// abort(), are recorded in <prospero_state_root()>/logs/fault.log before the
// title ends.
#pragma once

#include <cstddef>
#include <cstdint>

#include <signal.h>

namespace rex::platform {

struct ProsperoFaultRecord {
  int32_t signal_number = 0;
  int32_t signal_code = 0;
  uint64_t fault_address = 0;
  uint64_t rip = 0;
  uint64_t rsp = 0;
  uint64_t rbp = 0;
  uint64_t flags = 0;
  uint64_t trap_number = 0;
  uint64_t page_fault_error = 0;
  // Instruction-encoding order: RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
  // R8, R9, R10, R11, R12, R13, R14, R15.
  uint64_t registers[16] = {};
  // Where the title's code starts (its link address 0), so an address in it
  // is found in the linked ELF at address - image_base
  uint64_t image_base = 0;
  uint64_t image_end = 0;
  bool context_present = false;
};

// Normal startup only. Opens a regular append-only destination and retains
// its descriptor for the process lifetime, including faults during shutdown.
// Failure returns false with errno set; no directory or permission changes.
bool OpenProsperoFaultLog(const char* path) noexcept;
bool ProsperoFaultLogAvailable() noexcept;

// Pure bounded formatting, with no libc formatting, heap or locks. Returns
// bytes written (no terminating NUL is required); never exceeds capacity.
size_t FormatProsperoFaultRecord(const ProsperoFaultRecord& record, char* destination,
                                 size_t capacity) noexcept;

// Signal context: records only the first terminal fault in this process, then
// a frame-pointer backtrace from its RBP within bounds of its RSP. Fixed
// buffers, a lock-free flag and bounded writes to the descriptor opened above
// (fd 2 when there is none). No open, allocation, mutex, stdio or unwinding.
void WriteProsperoFaultRecord(const ProsperoFaultRecord& record) noexcept;

// The link wrapper (--wrap=abort) calls this before the real abort, with the
// return address of the first explicit abort call in linked code.
void WriteProsperoAbortCaller(uintptr_t return_address) noexcept;

// Normal startup, from ExceptionHandler::Install: opens the log and records
// the code segment's bounds. A failure leaves fd 2 as the fallback.
void InitializeProsperoFaultDiagnostics(uint64_t image_base, uint64_t image_end);
// Signal context: a record of the fault in context (a ucontext_t)
void ReportProsperoFatalSignal(int signal_number, const siginfo_t* info,
                               const void* context) noexcept;

}  // namespace rex::platform
