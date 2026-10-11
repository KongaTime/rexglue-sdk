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
 * Built two ways (ps5/tests/README.md):
 * - on the build host, against PS5_PayloadSDK's platform layer and its host
 *   model of the console's kernel (run-host.sh), which can also make kernel
 *   calls fail to check that every failure unwinds;
 * - as a payload for the console (Makefile), against the real kernel, which
 *   answers what the model cannot: whether the console accepts no-access
 *   protection on these shared views, and how long backing the guest's
 *   physical memory takes. Every line also goes to
 *   /data/rexglue-memory-test.txt as it is measured.
 */

#include <rex/memory/utils.h>

#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>

#if !defined(__PROSPERO__)
extern "C" {
#include "host_kernel.h"
}
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
  if (FILE* file = std::fopen("/data/rexglue-memory-test.txt", "a")) {
    std::fprintf(file, "%s\n", line);
    std::fclose(file);
  }
#endif
}

void check(bool ok, const char* what) {
  say("%s %s", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    failures++;
  }
}

double now_ms() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

sigjmp_buf fault_jump;
void on_fault(int) {
  siglongjmp(fault_jump, 1);
}
bool faults(volatile uint8_t* p, bool write) {
  if (sigsetjmp(fault_jump, 1)) {
    return true;
  }
  if (write) {
    *p = 0x5a;
  } else {
    (void)*p;
  }
  return false;
}

uint64_t objects() {
  ps5_shm_stats stats;
  ps5_shm_live(&stats);
  return stats.objects;
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

}  // namespace

int main() {
  struct sigaction action {};
  action.sa_handler = on_fault;
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, nullptr);
  sigaction(SIGBUS, &action, nullptr);

  say("==== memory_ps5_test ====");
  const size_t page = page_size();
  const size_t granularity = allocation_granularity();
  say("page size %zu, allocation granularity %zu", page, granularity);

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
  check(base == reinterpret_cast<uint8_t*>(1ull << 36), "all nine views map at 0x10_0000_0000");
  if (!base) {
    say("==== %d failed (stopped: no arena) ====", failures);
    return 1;
  }
  uint8_t* const phys = base + 0x100000000ull;

  PageAccess access;
  size_t length;
  check(faults(base + 0x40000000, false), "an uncommitted page of a view faults");
  check(QueryProtect(base + 0x40000000, length, access) && access == PageAccess::kNoAccess,
        "QueryProtect says no access there");

  // Memory::Initialize commits all 512 MiB of physical memory
  const double commit_start = now_ms();
  check(AllocFixed(phys, 0x20000000, AllocationType::kCommit, PageAccess::kReadWrite) == phys,
        "commit the physical heap's 512 MiB");
  say("the 512 MiB commit took %.1f ms and made %llu direct-memory objects",
      now_ms() - commit_start, static_cast<unsigned long long>(objects()));
  check(objects() == 256, "it is backed in 256 chunks of 2 MiB");

  std::memcpy(phys + 0x123400, "physical", 9);
  check(!std::memcmp(base + 0xA0123400, "physical", 9), "0xA0000000 sees physical memory");
  check(!std::memcmp(base + 0xC0123400, "physical", 9), "0xC0000000 sees physical memory");
  check(!std::memcmp(base + 0xE0123400, "physical", 9), "0xE0000000 sees physical memory");
  check(!std::memcmp(base + 0x7F123400, "physical", 9), "0x7F000000 sees physical memory");
  base[0xC0200000] = 0x77;
  check(phys[0x200000] == 0x77, "a write through 0xC0000000 lands in physical memory");

  // A virtual heap commit backs only its own chunk
  const uint64_t before = objects();
  check(AllocFixed(base + 0x40010000, 0x10000, AllocationType::kCommit, PageAccess::kReadWrite) ==
            base + 0x40010000,
        "commit 64 KiB in the 0x40000000 heap");
  check(objects() == before + 1, "it backs one chunk");
  check(!faults(base + 0x40010000, true), "the committed page is writable");
  check(QueryProtect(base + 0x40010000, length, access) && access == PageAccess::kReadWrite,
        "QueryProtect says read-write");
  check(faults(base + 0x40400000, false), "a page in another, unbacked chunk still faults");

  check(AllocFixed(base + 0x80100000, 0x10000, AllocationType::kCommit, PageAccess::kReadWrite) !=
            nullptr,
        "commit in the 0x80000000 xex heap");
  std::memcpy(base + 0x80100000, "xex", 4);
  check(!std::memcmp(base + 0x90100000, "xex", 4), "0x90000000 sees the 0x80000000 heap's memory");

  // Protection is each view's own
  PageAccess old = PageAccess::kNoAccess;
  check(Protect(base + 0xA0300000, page, PageAccess::kReadOnly, &old) &&
            old == PageAccess::kReadWrite,
        "Protect a 0xA0000000 page read-only, reporting it was read-write");
  check(faults(base + 0xA0300000, true), "writing it faults");
  check(!faults(base + 0xA0300000, false), "reading it does not");
  check(!faults(phys + 0x300000, true), "the same memory is still writable through the raw view");
  check(QueryProtect(base + 0xA0300000, length, access) && access == PageAccess::kReadOnly &&
            length == page,
        "QueryProtect says read-only, for one page");
  check(Protect(base + 0xA0300000, page, old, nullptr) && !faults(base + 0xA0300000, true),
        "restoring the old protection makes it writable again");

  // No access on a view of shared memory: what MMIO windows and decommits need,
  // and what PS5_RPCS3 saw the console refuse on some views
  check(AllocFixed(base + 0x7FC80000, 0x80000, AllocationType::kCommit, PageAccess::kNoAccess) !=
                nullptr &&
            faults(base + 0x7FC80000, false),
        "an MMIO window committed with no access faults");
  std::memcpy(base + 0x40010000, "kept", 5);
  check(DeallocFixed(base + 0x40010000, 0x10000, DeallocationType::kDecommit) &&
            faults(base + 0x40010000, false),
        "decommit takes the access away");
  check(AllocFixed(base + 0x40010000, 0x10000, AllocationType::kCommit, PageAccess::kReadWrite) &&
            !std::memcmp(base + 0x40010000, "kept", 5),
        "recommitting keeps the contents");

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
    std::memcpy(base + 0x80400000, "again", 6);
    check(!std::memcmp(base + 0x90400000, "again", 6), "and both views see the chunk");
  }
#endif

  // Anonymous memory
  void* const anonymous =
      AllocFixed(nullptr, 1 << 20, AllocationType::kReserveCommit, PageAccess::kReadWrite);
  check(anonymous && !faults(static_cast<uint8_t*>(anonymous) + 4096, true),
        "anonymous reserve+commit is writable");
  check(DeallocFixed(anonymous, 0, DeallocationType::kRelease), "and released");
  void* const reserved = AllocFixed(nullptr, 1 << 20, AllocationType::kReserve, PageAccess::kNoAccess);
  check(reserved && faults(static_cast<uint8_t*>(reserved), false), "an anonymous reservation faults");
  check(AllocFixed(static_cast<uint8_t*>(reserved) + 0x10000, 0x10000, AllocationType::kCommit,
                   PageAccess::kReadWrite) &&
            !faults(static_cast<uint8_t*>(reserved) + 0x10000, true),
        "committing part of it makes that part writable");
  check(DeallocFixed(reserved, 0, DeallocationType::kRelease), "and released");

  // Memory's destructor
  bool unmapped = true;
  for (size_t view = 0; view < kViews; view++) {
    unmapped &= UnmapFileView(handle, views[view], map_info[view].end - map_info[view].start + 1);
  }
  check(unmapped, "every view unmaps");
  CloseFileMappingHandle(handle, "xenia_memory_test");
  ps5_shm_stats stats;
  ps5_shm_live(&stats);
  check(stats.objects == 0 && stats.views == 0 && stats.ranges == 0 && stats.committed_bytes == 0,
        "no direct-memory objects, views, ranges or committed units are left");

  say("==== %d failed ====", failures);
  return failures != 0;
}
