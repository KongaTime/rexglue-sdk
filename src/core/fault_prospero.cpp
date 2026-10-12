// Ported from TheSimpsonsGame-PS5-Builder (LoZazaMastro and contributors),
// tools/rexglue-sdk/src/core/fault_prospero.cpp and the record half of
// src/core/exception_handler_prospero.cpp at 59e7086, BSD 3-Clause License (see
// LICENSE). Without the port's trace ring, stack dump and heap-failure hook;
// the image bounds come from the caller (the code segment the fork makes
// readable) rather than dladdr; the backtrace is the fork's own.
#include <rex/platform/fault_prospero.h>

#include <atomic>
#include <cerrno>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <rex/logging.h>
#include <rex/ui/startup_prospero.h>

namespace rex::platform {
namespace {

constinit std::atomic<int> fault_fd{-1};
static_assert(std::atomic<int>::is_always_lock_free);
// atomic_flag is guaranteed lock-free. Do not spin or wait in a signal.
std::atomic_flag terminal_fault_started = ATOMIC_FLAG_INIT;
std::atomic_flag abort_caller_started = ATOMIC_FLAG_INIT;
uint64_t fault_image_base = 0;
uint64_t fault_image_end = 0;

// A guest thread's host stack is 16 MiB (xthread.cpp); frames further from the
// faulting RSP than this are not followed
constexpr uint64_t kStackSpan = 32ull << 20;
constexpr int kMaxFrames = 32;

struct Buffer {
  char* data;
  size_t capacity;
  size_t used = 0;

  void Character(char value) noexcept {
    if (used < capacity) data[used++] = value;
  }
  void Text(const char* text) noexcept {
    while (*text) Character(*text++);
  }
  void Hex(uint64_t value) noexcept {
    Text("0x");
    constexpr char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
      Character(digits[(value >> shift) & 15]);
    }
  }
  void Decimal(int32_t value) noexcept {
    uint64_t magnitude = value < 0 ? uint64_t(-int64_t(value)) : uint64_t(value);
    if (value < 0) Character('-');
    char digits[10];
    size_t count = 0;
    do {
      digits[count++] = char('0' + magnitude % 10);
      magnitude /= 10;
    } while (magnitude);
    while (count) Character(digits[--count]);
  }
  // An address, and where it is in the linked ELF when it is the title's code
  void Code(uint64_t address) noexcept {
    Hex(address);
    if (fault_image_end > fault_image_base && address >= fault_image_base &&
        address < fault_image_end) {
      Text(" (elf ");
      Hex(address - fault_image_base);
      Character(')');
    }
  }
};

bool WriteBounded(int fd, const char* data, size_t size) noexcept {
  size_t done = 0;
  // Account for EINTR/partial writes without an unbounded signal-handler loop.
  for (unsigned attempt = 0; attempt < 4 && done < size; ++attempt) {
    const ssize_t written = write(fd, data + done, size - done);
    if (written > 0) {
      done += size_t(written);
    } else if (written == 0 || errno != EINTR) {
      return false;
    }
  }
  return done == size;
}

void Emit(const char* data, size_t size) noexcept {
  const int descriptor = fault_fd.load(std::memory_order_acquire);
  if (descriptor < 0 || !WriteBounded(descriptor, data, size)) {
    // fd 2 is merely a backup: it may be closed or not retained by the loader.
    WriteBounded(STDERR_FILENO, data, size);
  }
}

// Return addresses up the frame-pointer chain (the PS5 build keeps frame
// pointers): each frame is [saved RBP, return address], and frames only move
// up the stack. Written after the record, so a frame that faults loses only
// the rest of the backtrace.
void WriteBacktrace(uint64_t rbp, uint64_t rsp) noexcept {
  char text[128];
  uint64_t frame = rbp;
  for (int depth = 0; depth < kMaxFrames; ++depth) {
    if (frame < rsp || frame - rsp > kStackSpan || (frame & 7)) {
      break;
    }
    const uint64_t* slot = reinterpret_cast<const uint64_t*>(frame);
    const uint64_t next = slot[0];
    const uint64_t ret = slot[1];
    if (!ret) {
      break;
    }
    Buffer out{text, sizeof(text)};
    out.Text("  #");
    out.Decimal(depth);
    out.Character(' ');
    out.Code(ret);
    out.Character('\n');
    Emit(text, out.used);
    if (next <= frame) {
      break;
    }
    frame = next;
  }
}

}  // namespace

bool OpenProsperoFaultLog(const char* path) noexcept {
  if (fault_fd.load(std::memory_order_acquire) >= 0) return true;
  if (!path || path[0] != '/') {
    errno = EINVAL;
    return false;
  }
  // O_NONBLOCK avoids waiting on a FIFO masquerading as the log. A regular
  // file is still allowed to block inside the kernel while performing I/O;
  // the bound here is on calls and bytes, not on storage latency.
  const int descriptor =
      open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0666);
  if (descriptor < 0) return false;
  struct stat status{};
  const int queried = fstat(descriptor, &status);
  if (queried != 0 || !S_ISREG(status.st_mode)) {
    const int failure = queried != 0 ? errno : EINVAL;
    close(descriptor);
    errno = failure;
    return false;
  }
  int unopened = -1;
  if (!fault_fd.compare_exchange_strong(unopened, descriptor, std::memory_order_release,
                                        std::memory_order_relaxed)) {
    close(descriptor);
  }
  return true;
}

bool ProsperoFaultLogAvailable() noexcept {
  return fault_fd.load(std::memory_order_acquire) >= 0;
}

size_t FormatProsperoFaultRecord(const ProsperoFaultRecord& record, char* destination,
                                 size_t capacity) noexcept {
  if (!destination) return 0;
  Buffer out{destination, capacity};
  out.Text("[ReXGlue PS5] terminal fault: signal=");
  out.Decimal(record.signal_number);
  out.Text(" si_code=");
  out.Decimal(record.signal_code);
  out.Text(" context_present=");
  out.Character(record.context_present ? '1' : '0');
  out.Text(" fault=");
  out.Hex(record.fault_address);
  out.Text("\nRIP=");
  out.Code(record.rip);
  out.Text(" RSP=");
  out.Hex(record.rsp);
  out.Text(" RBP=");
  out.Hex(record.rbp);
  out.Text(" RFLAGS=");
  out.Hex(record.flags);
  out.Text(" trap=");
  out.Hex(record.trap_number);
  out.Text(" error=");
  out.Hex(record.page_fault_error);
  constexpr const char* names[] = {"RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
                                   "R8",  "R9",  "R10", "R11", "R12", "R13", "R14", "R15"};
  for (size_t index = 0; index < 16; ++index) {
    out.Character(index % 4 == 0 ? '\n' : ' ');
    out.Text(names[index]);
    out.Character('=');
    out.Hex(record.registers[index]);
  }
  out.Text("\ncode=");
  out.Hex(record.image_base);
  out.Character('-');
  out.Hex(record.image_end);
  out.Text(" (\"elf\" addresses are the linked ELF's, llvm-pie.elf)\nbacktrace:\n");
  return out.used;
}

void WriteProsperoFaultRecord(const ProsperoFaultRecord& record) noexcept {
  const int saved_errno = errno;
  if (terminal_fault_started.test_and_set(std::memory_order_relaxed)) {
    errno = saved_errno;
    return;
  }
  char text[1024];
  const size_t length = FormatProsperoFaultRecord(record, text, sizeof(text));
  Emit(text, length);
  if (record.context_present) {
    WriteBacktrace(record.rbp, record.rsp);
  }
  errno = saved_errno;
}

void WriteProsperoAbortCaller(uintptr_t return_address) noexcept {
  const int saved_errno = errno;
  if (abort_caller_started.test_and_set(std::memory_order_relaxed)) {
    errno = saved_errno;
    return;
  }
  char text[192];
  Buffer out{text, sizeof(text)};
  out.Text("[ReXGlue PS5] abort caller=");
  out.Code(return_address);
  out.Text(" (return address in linked code; separate from signal RIP)\nbacktrace:\n");
  Emit(text, out.used);
  WriteBacktrace(reinterpret_cast<uint64_t>(__builtin_frame_address(0)),
                 reinterpret_cast<uint64_t>(__builtin_frame_address(0)));
  errno = saved_errno;
}

void InitializeProsperoFaultDiagnostics(uint64_t image_base, uint64_t image_end) {
  fault_image_base = image_base;
  fault_image_end = image_end;
  const std::string logs = std::string(prospero_state_root()) + "/logs";
  mkdir(logs.c_str(), 0777);
  const std::string path = logs + "/fault.log";
  if (!OpenProsperoFaultLog(path.c_str())) {
    REXLOG_WARN("PS5: no fault log at {} (errno {}); fault records go to stderr", path, errno);
    return;
  }
  REXLOG_INFO("PS5: fault records go to {}", path);
}

void ReportProsperoFatalSignal(int signal_number, const siginfo_t* info,
                               const void* context) noexcept {
  ProsperoFaultRecord record{};
  record.signal_number = signal_number;
  if (info) {
    record.signal_code = info->si_code;
    record.fault_address = reinterpret_cast<uint64_t>(info->si_addr);
  }
  record.image_base = fault_image_base;
  record.image_end = fault_image_end;
  record.context_present = context != nullptr;
  if (context) {
    const auto& machine = static_cast<const ucontext_t*>(context)->uc_mcontext;
    record.rip = uint64_t(machine.mc_rip);
    record.rsp = uint64_t(machine.mc_rsp);
    record.rbp = uint64_t(machine.mc_rbp);
    record.flags = uint64_t(machine.mc_rflags);
    record.trap_number = uint64_t(machine.mc_trapno);
    record.page_fault_error = uint64_t(machine.mc_err);
    const uint64_t registers[] = {
        uint64_t(machine.mc_rax), uint64_t(machine.mc_rcx), uint64_t(machine.mc_rdx),
        uint64_t(machine.mc_rbx), uint64_t(machine.mc_rsp), uint64_t(machine.mc_rbp),
        uint64_t(machine.mc_rsi), uint64_t(machine.mc_rdi), uint64_t(machine.mc_r8),
        uint64_t(machine.mc_r9),  uint64_t(machine.mc_r10), uint64_t(machine.mc_r11),
        uint64_t(machine.mc_r12), uint64_t(machine.mc_r13), uint64_t(machine.mc_r14),
        uint64_t(machine.mc_r15)};
    for (size_t index = 0; index < 16; ++index) record.registers[index] = registers[index];
  }
  WriteProsperoFaultRecord(record);
}

}  // namespace rex::platform
