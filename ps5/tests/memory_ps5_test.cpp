/**
 * @file        ps5/tests/memory_ps5_test.cpp
 * @brief       src/core/memory_ps5.cpp, tested as Memory::Initialize uses it
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Lays the guest arena out exactly as Memory::Initialize does
 * (src/system/xmemory.cpp: one 4.5 GiB mapping, nine views from 1 << 36 up),
 * then commits, aliases, protects, decommits and tears it down through the
 * rex::memory API, checking each step by touching the memory.
 *
 * Built three ways (ps5/tests/README.md):
 * - on the build host, against PS5_PayloadSDK's platform layer and its host
 *   model of the console's kernel (run-host.sh), which can also make kernel
 *   calls fail to check that every failure unwinds;
 * - as a payload for the console (Makefile), against the real kernel, which
 *   answers what the model cannot: whether the console accepts no-access
 *   protection on these shared views, how long backing the guest's physical
 *   memory takes, and how much direct memory the process may have. Every line
 *   also goes to /data/rexglue-memory-test.txt as it is measured;
 * - inside a title (MEMORY_PS5_TEST_EMBEDDED: memory_ps5_test_main() in place of
 *   main()), whose process has the console's 12 GiB of direct memory where a
 *   payload's has next to none. The title names its results file
 *   (MEMORY_PS5_TEST_RESULTS).
 *
 * Every touch of guest memory goes through put/holds/faults, so a commit that
 * failed is reported, not crashed into; a fault anywhere else stops the test
 * with its address.
 */

#include <rex/memory/utils.h>

#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>

#if !defined(__PROSPERO__)
extern "C" {
#include "host_kernel.h"
}
#endif

#ifndef MEMORY_PS5_TEST_RESULTS
#if defined(MEMORY_PS5_TEST_EMBEDDED)
#define MEMORY_PS5_TEST_RESULTS "/app0/rexglue-memory-test.txt"  // the title's folder
#else
#define MEMORY_PS5_TEST_RESULTS "/data/rexglue-memory-test.txt"
#endif
#endif

using namespace rex::memory;

namespace {

int failures;

void say(const char* format, ...) {
  char line[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof line, format, args);
  va_end(args);
  std::printf("%s\n", line);
  std::fflush(stdout);
#if defined(__PROSPERO__)
  if (FILE* file = std::fopen(MEMORY_PS5_TEST_RESULTS, "a")) {
    std::fprintf(file, "%s\n", line);
    std::fclose(file);
  }
#endif
}

bool check(bool ok, const char* what) {
  say("%s %s", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    failures++;
  }
  return ok;
}

double now_ms() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// Faults are expected only inside put/holds/faults
sigjmp_buf fault_jump;
volatile sig_atomic_t fault_armed;

void on_fault(int signal, siginfo_t* info, void*) {
  if (fault_armed) {
    fault_armed = 0;
    siglongjmp(fault_jump, 1);
  }
  say("STOPPED: an unexpected fault (signal %d) at %p", signal, info->si_addr);
  _exit(2);
}

// Writes s at p; false when the write faults
bool put(uint8_t* p, const char* s) {
  if (sigsetjmp(fault_jump, 1)) {
    return false;
  }
  fault_armed = 1;
  std::memcpy(p, s, std::strlen(s) + 1);
  fault_armed = 0;
  return true;
}

// Whether p holds s; false when it does not, or the read faults
bool holds(const uint8_t* p, const char* s) {
  if (sigsetjmp(fault_jump, 1)) {
    return false;
  }
  fault_armed = 1;
  const bool same = std::memcmp(p, s, std::strlen(s) + 1) == 0;
  fault_armed = 0;
  return same;
}

bool faults(volatile uint8_t* p, bool write) {
  if (sigsetjmp(fault_jump, 1)) {
    return true;
  }
  fault_armed = 1;
  if (write) {
    *p = 0x5a;
  } else {
    (void)*p;
  }
  fault_armed = 0;
  return false;
}

uint64_t objects() {
  ps5_shm_stats stats;
  ps5_shm_live(&stats);
  return stats.objects;
}

// The process's direct memory: its size, the largest free block, and how
// much of it 64 KiB allocations get before one fails (all given back)
void report_direct_memory() {
  const int64_t size = sceKernelGetDirectMemorySize();
  int64_t free_start = 0;
  size_t free_bytes = 0;
  const int32_t queried =
      sceKernelAvailableDirectMemorySize(0, size, 0x10000, &free_start, &free_bytes);
  size_t flexible = 0;
  sceKernelAvailableFlexibleMemorySize(&flexible);
  say("direct memory: %lld MiB; largest free block %zu MiB (query 0x%08x); flexible free %zu MiB",
      static_cast<long long>(size >> 20), free_bytes >> 20, static_cast<uint32_t>(queried),
      flexible >> 20);

  constexpr size_t kUnit = 0x10000;
  constexpr size_t kCap = size_t(2) << 30;  // stop at 2 GiB: enough to know
  std::vector<int64_t> units;
  units.reserve(kCap / kUnit);
  int32_t refused = 0;
  while (units.size() * kUnit < kCap) {
    int64_t start = -1;
    refused = sceKernelAllocateDirectMemory(0, size, kUnit, kUnit, PS5_KERNEL_DIRECT_TYPE_CPU,
                                            &start);
    if (refused != 0) {
      break;
    }
    units.push_back(start);
  }
  say("this process took %zu MiB of direct memory in 64 KiB allocations%s (0x%08x)",
      (units.size() * kUnit) >> 20,
      refused ? " before one was refused" : ", the test's cap", static_cast<uint32_t>(refused));
  for (const int64_t start : units) {
    sceKernelReleaseDirectMemory(start, kUnit);
  }
}

// src/system/xmemory.cpp's map_info
const struct {
  uint64_t start, end, target;
} map_info[] = {
    {0x00000000, 0x3FFFFFFF, 0x0000000000000000ull}, {0x40000000, 0x7EFFFFFF, 0x0000000040000000ull},
    {0x7F000000, 0x7FFFFFFF, 0x0000000100000000ull}, {0x80000000, 0x8FFFFFFF, 0x0000000080000000ull},
    {0x90000000, 0x9FFFFFFF, 0x0000000080000000ull}, {0xA0000000, 0xBFFFFFFF, 0x0000000100000000ull},
    {0xC0000000, 0xDFFFFFFF, 0x0000000100000000ull}, {0xE0000000, 0xFFFFFFFF, 0x0000000100001000ull},
    {0x100000000, 0x11FFFFFFF, 0x0000000100000000ull},
};
constexpr size_t kViews = sizeof(map_info) / sizeof(map_info[0]);

// Everything between mapping the views and unmapping them; returns early, with
// the reason, when a step the rest needs fails
void arena_checks(uint8_t* base) {
  uint8_t* const phys = base + 0x100000000ull;
  const size_t page = page_size();
  PageAccess access;
  size_t length;

  check(faults(base + 0x40000000, false), "an uncommitted page of a view faults");
  check(QueryProtect(base + 0x40000000, length, access) && access == PageAccess::kNoAccess,
        "QueryProtect says no access there");

  // Memory::Initialize commits all 512 MiB of physical memory
#if !defined(__PROSPERO__)
  // MEMORY_TEST_REFUSE_CHUNK=<n>: the host model refuses the nth chunk's
  // direct memory, as the console refused the 28th to a payload (2026-10-11)
  if (const char* nth = std::getenv("MEMORY_TEST_REFUSE_CHUNK")) {
    host_fail(HOST_CALL_ALLOCATE, std::atoi(nth));
  }
#endif
  const double commit_start = now_ms();
  const bool physical =
      AllocFixed(phys, 0x20000000, AllocationType::kCommit, PageAccess::kReadWrite) == phys;
  say("the 512 MiB commit took %.1f ms and made %llu direct-memory objects",
      now_ms() - commit_start, static_cast<unsigned long long>(objects()));
  if (!check(physical, "commit the physical heap's 512 MiB")) {
    say("stopping the arena checks: the rest needs physical memory");
    return;
  }
  check(objects() == 256, "it is backed in 256 chunks of 2 MiB");

  check(put(phys + 0x123400, "physical"), "write physical memory through the raw view");
  check(holds(base + 0xA0123400, "physical"), "0xA0000000 sees physical memory");
  check(holds(base + 0xC0123400, "physical"), "0xC0000000 sees physical memory");
  check(holds(base + 0xE0123400, "physical"), "0xE0000000 sees physical memory");
  check(holds(base + 0x7F123400, "physical"), "0x7F000000 sees physical memory");
  check(put(base + 0xC0200000, "through C0") && holds(phys + 0x200000, "through C0"),
        "a write through 0xC0000000 lands in physical memory");

  // A virtual heap commit backs only its own chunk
  const uint64_t before = objects();
  if (check(AllocFixed(base + 0x40010000, 0x10000, AllocationType::kCommit,
                       PageAccess::kReadWrite) == base + 0x40010000,
            "commit 64 KiB in the 0x40000000 heap")) {
    check(objects() == before + 1, "it backs one chunk");
    check(!faults(base + 0x40010000, true), "the committed page is writable");
    check(QueryProtect(base + 0x40010000, length, access) && access == PageAccess::kReadWrite,
          "QueryProtect says read-write");
  }
  check(faults(base + 0x40400000, false), "a page in another, unbacked chunk still faults");

  if (check(AllocFixed(base + 0x80100000, 0x10000, AllocationType::kCommit,
                       PageAccess::kReadWrite) != nullptr,
            "commit in the 0x80000000 xex heap")) {
    check(put(base + 0x80100000, "xex") && holds(base + 0x90100000, "xex"),
          "0x90000000 sees the 0x80000000 heap's memory");
  }

  // Protection is each view's own
  PageAccess old = PageAccess::kNoAccess;
  if (check(Protect(base + 0xA0300000, page, PageAccess::kReadOnly, &old) &&
                old == PageAccess::kReadWrite,
            "Protect a 0xA0000000 page read-only, reporting it was read-write")) {
    check(faults(base + 0xA0300000, true), "writing it faults");
    check(!faults(base + 0xA0300000, false), "reading it does not");
    check(!faults(phys + 0x300000, true), "the same memory is still writable through the raw view");
    check(QueryProtect(base + 0xA0300000, length, access) && access == PageAccess::kReadOnly &&
              length == page,
          "QueryProtect says read-only, for one page");
    check(Protect(base + 0xA0300000, page, old, nullptr) && !faults(base + 0xA0300000, true),
          "restoring the old protection makes it writable again");
  }

  // No access on a view of shared memory: what MMIO windows and decommits need,
  // and what PS5_RPCS3 saw the console refuse on some views
  check(AllocFixed(base + 0x7FC80000, 0x80000, AllocationType::kCommit,
                   PageAccess::kNoAccess) != nullptr &&
            faults(base + 0x7FC80000, false),
        "an MMIO window committed with no access faults");
  if (put(base + 0x40010000, "kept")) {
    check(DeallocFixed(base + 0x40010000, 0x10000, DeallocationType::kDecommit) &&
              faults(base + 0x40010000, false),
          "decommit takes the access away");
    check(AllocFixed(base + 0x40010000, 0x10000, AllocationType::kCommit,
                     PageAccess::kReadWrite) &&
              holds(base + 0x40010000, "kept"),
          "recommitting keeps the contents");
  }

  // For the record: what the kernel says of a view (memory_ps5.cpp keeps its own)
  {
    void* start = nullptr;
    void* end = nullptr;
    uint32_t protection = 0;
    const int32_t result =
        sceKernelQueryMemoryProtection(base + 0xA0300000, &start, &end, &protection);
    say("(record) sceKernelQueryMemoryProtection on a view page: 0x%08x, %p-%p, protection 0x%x",
        static_cast<uint32_t>(result), start, end, protection);
  }

#if !defined(__PROSPERO__)
  // The host model can make kernel calls fail: a failure while a new chunk is
  // mapped into its views unwinds completely
  {
    const uint64_t held = objects();
    host_fail(HOST_CALL_MAP, 3);  // the zeroing map, the 0x80000000 view, then 0x90000000's
    check(AllocFixed(base + 0x80400000, 0x10000, AllocationType::kCommit,
                     PageAccess::kReadWrite) == nullptr,
          "a commit whose second view mapping fails reports failure");
    check(objects() == held, "its chunk's direct memory is released");
    check(faults(base + 0x80400000, false) && faults(base + 0x90400000, false),
          "both views are reservations again");
    host_fail(HOST_CALL_ALLOCATE, 1);
    check(AllocFixed(base + 0x80400000, 0x10000, AllocationType::kCommit,
                     PageAccess::kReadWrite) == nullptr &&
              objects() == held,
          "a failed allocation leaves nothing behind");
    check(AllocFixed(base + 0x80400000, 0x10000, AllocationType::kCommit,
                     PageAccess::kReadWrite) != nullptr,
          "the same commit then succeeds");
    check(put(base + 0x80400000, "again") && holds(base + 0x90400000, "again"),
          "and both views see the chunk");
  }
#endif
}

}  // namespace

#if defined(MEMORY_PS5_TEST_EMBEDDED)
extern "C" int memory_ps5_test_main(void) {
#else
int main() {
#endif
#if defined(__PROSPERO__)
  // A fresh results file each run, readable and writable over FTP
  if (FILE* file = std::fopen(MEMORY_PS5_TEST_RESULTS, "w")) {
    std::fclose(file);
    chmod(MEMORY_PS5_TEST_RESULTS, 0666);
  }
#endif
  // The test's fault handler, and whatever was there before it (a title's own)
  // back at the end
  struct sigaction action {}, previous_segv {}, previous_bus {};
  action.sa_sigaction = on_fault;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, &previous_segv);
  sigaction(SIGBUS, &action, &previous_bus);

  say("==== memory_ps5_test ====");
  const size_t page = page_size();
  const size_t granularity = allocation_granularity();
  say("page size %zu, allocation granularity %zu", page, granularity);
  report_direct_memory();

  // Memory::Initialize: the mapping, then its views from 1 << 36 up
  const size_t mapping_size = (0x120000000ull + granularity + granularity - 1) & ~(granularity - 1);
  const auto handle =
      CreateFileMappingHandle("xenia_memory_test", mapping_size, PageAccess::kReadWrite, false);
  check(handle != kFileMappingHandleInvalid, "CreateFileMappingHandle(4.5 GiB) without commit");
  check(objects() == 0, "no direct memory backs it yet");

  uint8_t* base = nullptr;
  void* views[kViews] = {};
  const uint64_t mask = ~uint64_t(granularity - 1);
  for (size_t n = 36; n < 64 && !base; n++) {
    uint8_t* const candidate = reinterpret_cast<uint8_t*>(1ull << n);
    size_t mapped = 0;
    for (; mapped < kViews; mapped++) {
      views[mapped] =
          MapFileView(handle, candidate + map_info[mapped].start,
                      map_info[mapped].end - map_info[mapped].start + 1, PageAccess::kReadWrite,
                      map_info[mapped].target & mask);
      if (!views[mapped]) {
        break;
      }
    }
    if (mapped == kViews) {
      base = candidate;
      break;
    }
    for (size_t undo = 0; undo < mapped; undo++) {
      UnmapFileView(handle, views[undo], map_info[undo].end - map_info[undo].start + 1);
    }
  }
  say("arena at %p", static_cast<void*>(base));
  if (check(base == reinterpret_cast<uint8_t*>(1ull << 36), "all nine views map at 0x10_0000_0000")) {
    arena_checks(base);
  }

  // Anonymous memory
  auto* const anonymous = static_cast<uint8_t*>(
      AllocFixed(nullptr, 1 << 20, AllocationType::kReserveCommit, PageAccess::kReadWrite));
  check(anonymous && !faults(anonymous + 4096, true), "anonymous reserve+commit is writable");
  check(anonymous && DeallocFixed(anonymous, 0, DeallocationType::kRelease), "and released");
  auto* const reserved = static_cast<uint8_t*>(
      AllocFixed(nullptr, 1 << 20, AllocationType::kReserve, PageAccess::kNoAccess));
  check(reserved && faults(reserved, false), "an anonymous reservation faults");
  check(reserved &&
            AllocFixed(reserved + 0x10000, 0x10000, AllocationType::kCommit,
                       PageAccess::kReadWrite) &&
            !faults(reserved + 0x10000, true),
        "committing part of it makes that part writable");
  check(reserved && DeallocFixed(reserved, 0, DeallocationType::kRelease), "and released");

  // Memory's destructor
  if (base) {
    bool unmapped = true;
    for (size_t view = 0; view < kViews; view++) {
      unmapped &= UnmapFileView(handle, views[view], map_info[view].end - map_info[view].start + 1);
    }
    check(unmapped, "every view unmaps");
  }
  CloseFileMappingHandle(handle, "xenia_memory_test");
  ps5_shm_stats stats;
  ps5_shm_live(&stats);
  check(stats.objects == 0 && stats.views == 0 && stats.ranges == 0 && stats.committed_bytes == 0,
        "no direct-memory objects, views, ranges or committed units are left");

  say("==== %d failed ====", failures);
  sigaction(SIGSEGV, &previous_segv, nullptr);
  sigaction(SIGBUS, &previous_bus, nullptr);
  return failures != 0;
}
