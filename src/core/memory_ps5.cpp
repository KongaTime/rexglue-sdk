/**
 * @file        rex/core/memory_ps5.cpp
 * @brief       PS5 backend for rex::memory's page allocation and file mappings
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * memory_posix.cpp, on the console, would put guest memory in POSIX shared
 * memory, and every view of a POSIX shared-memory object is charged to the
 * title's flexible budget (448 MiB configured; PS5_PayloadSDK's
 * platform/docs/PROBE.md). Everything here is direct memory instead (12 GiB),
 * through the payload SDK fork's platform layer (ps5platform/shm.h):
 *
 * - A file mapping (the guest's 4.5 GiB arena, which Memory::Initialize maps
 *   at several addresses) is backed in 2 MiB chunks, each a direct-memory
 *   object created the first time anything in it is committed, and mapped
 *   then into every view that covers it. Backing the whole mapping at once
 *   would take 4.5 GiB of the pool for an arena a game uses a fraction of.
 *   A chunk is mapped read-write in every view, as memory_posix.cpp's views
 *   are; a commit then sets the protection it asks for on its own range.
 *   Uncommitted parts of a view stay reserved, with no access, as on Windows.
 * - A view reserves its range exactly where it is asked for
 *   (ps5_vrange_reserve_at), so Memory::Initialize can try one base after
 *   another as it does on Linux.
 * - Each view keeps the protection of every page itself. The guest's memory
 *   code reads a protection back and later restores it, and the console's
 *   sceKernelQueryMemoryProtection does not always describe a view
 *   (PS5_RPCS3's util/vm_native_ps5.cpp found views it said nothing of).
 * - Anonymous allocations are reservations backed in 64 KiB units on their
 *   first commit (ps5_vrange_commit).
 * - Protection goes through sceKernelMprotect. Pages are 16 KiB (measured).
 *
 * Not yet run on a console: EdgeOfTimeRecomp's ps5/probe checks these calls.
 */

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <unistd.h>

#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>

namespace rex {
namespace memory {

namespace {

// The direct-memory unit, and the alignment ps5_vrange_reserve_at needs
constexpr size_t kUnit = 0x10000;
// The backing granularity of a file mapping: the physical heap's 512 MiB,
// committed at start-up and seen through five views, is 1280 mappings, where
// 64 KiB chunks would be 40960
constexpr size_t kChunk = 2 * 1024 * 1024;

size_t host_page() {
  return size_t(getpagesize());
}

struct View {
  uintptr_t base;
  size_t length;
  size_t file_offset;
  // Every page's protection, as this file last set it
  std::vector<PageAccess> access;

  size_t page_index(uintptr_t address) const { return (address - base) / host_page(); }
  void set_access(uintptr_t begin, uintptr_t end, PageAccess value) {
    std::fill(access.begin() + page_index(begin), access.begin() + page_index(end - 1) + 1, value);
  }
};

struct Mapping {
  size_t length = 0;
  std::vector<ps5_shm> chunks;  // bytes == 0: not backed yet
  std::vector<View> views;
};

struct Range {
  uintptr_t base;
  size_t length;
};

// Everything below is guarded by g_lock
std::mutex g_lock;
std::vector<std::unique_ptr<Mapping>> g_mappings;
std::vector<Range> g_ranges;  // anonymous reservations AllocFixed made

int ShmProtection(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return 0;
    case PageAccess::kReadOnly:
      return PS5_SHM_READ;
    case PageAccess::kReadWrite:
      return PS5_SHM_READ | PS5_SHM_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PS5_SHM_READ | PS5_SHM_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PS5_SHM_READ | PS5_SHM_WRITE | PS5_SHM_EXEC;
    default:
      assert_unhandled_case(access);
      return 0;
  }
}

int KernelProtection(PageAccess access) {
  const int shm = ShmProtection(access);
  return (shm & PS5_SHM_READ ? PS5_KERNEL_PROT_CPU_READ : 0) |
         (shm & PS5_SHM_WRITE ? PS5_KERNEL_PROT_CPU_WRITE : 0) |
         (shm & PS5_SHM_EXEC ? PS5_KERNEL_PROT_CPU_EXEC : 0);
}

PageAccess FromKernelProtection(uint32_t protection) {
  const bool read = protection & PS5_KERNEL_PROT_CPU_READ;
  const bool write = protection & PS5_KERNEL_PROT_CPU_WRITE;
  const bool execute = protection & PS5_KERNEL_PROT_CPU_EXEC;
  if (execute) {
    return write ? PageAccess::kExecuteReadWrite : PageAccess::kExecuteReadOnly;
  }
  if (write) {
    return PageAccess::kReadWrite;
  }
  return read ? PageAccess::kReadOnly : PageAccess::kNoAccess;
}

bool Contains(uintptr_t base, size_t length, uintptr_t address) {
  return address >= base && address - base < length;
}

Mapping* FindMapping(FileMappingHandle handle) {
  for (auto& mapping : g_mappings) {
    if (reinterpret_cast<FileMappingHandle>(mapping.get()) == handle) {
      return mapping.get();
    }
  }
  return nullptr;
}

// The view holding an address, and its mapping
bool FindView(uintptr_t address, Mapping** out_mapping, View** out_view) {
  for (auto& mapping : g_mappings) {
    for (auto& view : mapping->views) {
      if (Contains(view.base, view.length, address)) {
        *out_mapping = mapping.get();
        *out_view = &view;
        return true;
      }
    }
  }
  return false;
}

Range* FindRange(uintptr_t address) {
  for (auto& range : g_ranges) {
    if (Contains(range.base, range.length, address)) {
      return &range;
    }
  }
  return nullptr;
}

// The part of a chunk a view covers, as file offsets; empty when none
void ChunkInView(const Mapping& mapping, size_t chunk, const View& view, size_t* begin,
                 size_t* end) {
  const size_t chunk_begin = chunk * kChunk;
  const size_t chunk_end = std::min(chunk_begin + kChunk, mapping.length);
  *begin = std::max(chunk_begin, view.file_offset);
  *end = std::max(*begin, std::min(chunk_end, view.file_offset + view.length));
}

// Maps the part of a backed chunk that a view covers, read-write
bool MapChunkIntoView(const Mapping& mapping, size_t chunk, View& view) {
  size_t begin, end;
  ChunkInView(mapping, chunk, view, &begin, &end);
  if (begin == end) {
    return true;
  }
  const uintptr_t address = view.base + (begin - view.file_offset);
  void* mapped = nullptr;
  const int result = ps5_shm_map(&mapping.chunks[chunk], begin - chunk * kChunk, end - begin,
                                 reinterpret_cast<void*>(address), PS5_SHM_READ | PS5_SHM_WRITE,
                                 PS5_SHM_FIXED, &mapped);
  if (result != 0) {
    REXSYS_ERROR("ps5_shm_map(chunk {}, 0x{:X} bytes at {:#x}) failed: 0x{:08X}", chunk,
                 end - begin, address, uint32_t(result));
    return false;
  }
  view.set_access(address, address + (end - begin), PageAccess::kReadWrite);
  return true;
}

// Backs a chunk and maps it into every view of the mapping that covers it
bool BackChunk(Mapping& mapping, size_t chunk) {
  if (mapping.chunks[chunk].bytes) {
    return true;
  }
  const size_t bytes = std::min(kChunk, mapping.length - chunk * kChunk);
  const int result = ps5_shm_create(bytes, &mapping.chunks[chunk]);
  if (result != 0) {
    int64_t free_start = 0;
    size_t free_bytes = 0;
    sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), kUnit, &free_start,
                                       &free_bytes);
    REXSYS_ERROR(
        "ps5_shm_create(0x{:X}) for chunk {} failed: 0x{:08X} (largest free direct-memory "
        "block 0x{:X} of 0x{:X})",
        bytes, chunk, uint32_t(result), free_bytes, sceKernelGetDirectMemorySize());
    return false;
  }
  for (size_t mapped = 0; mapped < mapping.views.size(); ++mapped) {
    if (MapChunkIntoView(mapping, chunk, mapping.views[mapped])) {
      continue;
    }
    // Back out: the views mapped so far are reservations again, and the chunk
    // is unbacked, so a later commit starts over
    for (size_t undo = 0; undo < mapped; ++undo) {
      View& view = mapping.views[undo];
      size_t begin, end;
      ChunkInView(mapping, chunk, view, &begin, &end);
      if (begin != end) {
        const uintptr_t address = view.base + (begin - view.file_offset);
        ps5_shm_unmap(reinterpret_cast<void*>(address), end - begin, PS5_SHM_KEEP_RESERVED);
        view.set_access(address, address + (end - begin), PageAccess::kNoAccess);
      }
    }
    ps5_shm_destroy(&mapping.chunks[chunk]);
    return false;
  }
  return true;
}

// Calls fn(mapping, view, begin, end) for each view's part of [address, end);
// false if part of it is in no view, or fn fails
template <typename Fn>
bool ForEachViewPart(uintptr_t address, uintptr_t end, Fn fn) {
  for (uintptr_t at = address; at < end;) {
    Mapping* mapping;
    View* view;
    if (!FindView(at, &mapping, &view)) {
      return false;
    }
    const uintptr_t next = std::min<uintptr_t>(end, view->base + view->length);
    if (!fn(*mapping, *view, at, next)) {
      return false;
    }
    at = next;
  }
  return true;
}

// Backs the chunks under [address, end) of the views
bool BackViewRange(uintptr_t address, uintptr_t end) {
  return ForEachViewPart(address, end, [](Mapping& mapping, View& view, uintptr_t begin,
                                          uintptr_t stop) {
    const size_t first = (view.file_offset + (begin - view.base)) / kChunk;
    const size_t last = (view.file_offset + (stop - view.base) - 1) / kChunk;
    for (size_t chunk = first; chunk <= last; ++chunk) {
      if (!BackChunk(mapping, chunk)) {
        return false;
      }
    }
    return true;
  });
}

// Sets the protection of [address, end) of the views: through
// sceKernelMprotect where a chunk is backed; a reservation has no access
// whatever is asked
bool ProtectViewRange(uintptr_t address, uintptr_t end, PageAccess access) {
  return ForEachViewPart(address, end, [access](Mapping& mapping, View& view, uintptr_t begin,
                                                uintptr_t stop) {
    for (uintptr_t at = begin; at < stop;) {
      const size_t file_at = view.file_offset + (at - view.base);
      const size_t chunk = file_at / kChunk;
      const uintptr_t next = std::min<uintptr_t>(stop, at + ((chunk + 1) * kChunk - file_at));
      if (mapping.chunks[chunk].bytes) {
        const int32_t result = sceKernelMprotect(reinterpret_cast<void*>(at), size_t(next - at),
                                                 KernelProtection(access));
        if (result != 0) {
          REXSYS_ERROR("sceKernelMprotect({:#x}, 0x{:X}, {:#x}) failed: 0x{:08X}", at, next - at,
                       KernelProtection(access), uint32_t(result));
          return false;
        }
        view.set_access(at, next, access);
      }
      at = next;
    }
    return true;
  });
}

}  // namespace

size_t page_size() {
  return host_page();
}

size_t allocation_granularity() {
  return kUnit;
}

bool IsWritableExecutableMemorySupported() {
  // Direct memory mapped read-write and then given execute runs (PROBE.md)
  return true;
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  const size_t page = host_page();
  const bool commit = allocation_type != AllocationType::kReserve;
  const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
  std::lock_guard lock(g_lock);

  if (!base_address) {
    void* placed = nullptr;
    const size_t rounded = rex::round_up(length, page);
    if (ps5_vrange_reserve(rounded, nullptr, kUnit, &placed) != 0) {
      return nullptr;
    }
    g_ranges.push_back({reinterpret_cast<uintptr_t>(placed), rounded});
    if (commit && ps5_vrange_commit(placed, rounded, ShmProtection(access)) != 0) {
      ps5_vrange_release(placed, rounded);
      g_ranges.pop_back();
      return nullptr;
    }
    return placed;
  }

  // Whole host pages only, as memory_posix.cpp's commits on macOS: a guest heap
  // reconciles the guest pages sharing a host page before it asks
  if (address % page != 0 || length % page != 0 || length == 0) {
    return nullptr;
  }

  Mapping* mapping;
  View* view;
  if (FindView(address, &mapping, &view)) {
    if (!commit) {
      // A view is reserved whole when it is mapped
      return Contains(view->base, view->length, address + length - 1) ? base_address : nullptr;
    }
    if (!BackViewRange(address, address + length) ||
        !ProtectViewRange(address, address + length, access)) {
      return nullptr;
    }
    return base_address;
  }

  if (Range* range = FindRange(address)) {
    if (!Contains(range->base, range->length, address + length - 1)) {
      return nullptr;
    }
    if (commit && ps5_vrange_commit(base_address, length, ShmProtection(access)) != 0) {
      return nullptr;
    }
    return base_address;
  }

  // A new reservation, exactly there or not at all
  if (ps5_vrange_reserve_at(base_address, length) != 0) {
    return nullptr;
  }
  g_ranges.push_back({address, length});
  if (commit && ps5_vrange_commit(base_address, length, ShmProtection(access)) != 0) {
    ps5_vrange_release(base_address, length);
    g_ranges.pop_back();
    return nullptr;
  }
  return base_address;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
  std::lock_guard lock(g_lock);

  Mapping* mapping;
  View* view;
  if (FindView(address, &mapping, &view)) {
    // A view's memory belongs to its mapping. Decommitting takes the access
    // away and keeps the contents, as memory_posix.cpp does with a shared
    // mapping; releasing (BaseHeap::Dispose, length 0) leaves it to
    // UnmapFileView.
    if (deallocation_type == DeallocationType::kDecommit && length) {
      return ProtectViewRange(address, address + length, PageAccess::kNoAccess);
    }
    return true;
  }

  Range* range = FindRange(address);
  if (!range) {
    return sceKernelMunmap(base_address, length) == 0;
  }
  if (deallocation_type == DeallocationType::kRelease && address == range->base &&
      (length == 0 || length >= range->length)) {
    const Range released = *range;
    g_ranges.erase(g_ranges.begin() + (range - g_ranges.data()));
    return ps5_vrange_release(reinterpret_cast<void*>(released.base), released.length) == 0;
  }
  // Decommit, or release of part of a range: its memory goes back and the
  // addresses stay reserved
  if (!length) {
    length = range->base + range->length - address;
  }
  return ps5_vrange_decommit(base_address, length) == 0;
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    size_t old_length = 0;
    if (!QueryProtect(base_address, old_length, *out_old_access)) {
      *out_old_access = PageAccess::kNoAccess;
    }
  }
  const size_t page = host_page();
  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address) & ~uintptr_t(page - 1);
  const uintptr_t end = rex::round_up(reinterpret_cast<uintptr_t>(base_address) + length, page);
  std::lock_guard lock(g_lock);

  Mapping* mapping;
  View* view;
  if (FindView(begin, &mapping, &view)) {
    // Access to an unbacked part of a view backs it, as mprotect gives a
    // reserved page of a shared mapping access on Linux
    if (access != PageAccess::kNoAccess && !BackViewRange(begin, end)) {
      return false;
    }
    return ProtectViewRange(begin, end, access);
  }
  if (FindRange(begin)) {
    if (access != PageAccess::kNoAccess) {
      return ps5_vrange_commit(reinterpret_cast<void*>(begin), size_t(end - begin),
                               ShmProtection(access)) == 0;
    }
    // No access over what is backed; unbacked units have none already
    for (uintptr_t at = begin; at < end;) {
      void* start = nullptr;
      void* stop = nullptr;
      uint32_t current = 0;
      const bool mapped =
          sceKernelQueryMemoryProtection(reinterpret_cast<void*>(at), &start, &stop, &current) ==
              0 &&
          reinterpret_cast<uintptr_t>(start) <= at && reinterpret_cast<uintptr_t>(stop) > at;
      const uintptr_t next =
          mapped ? std::min<uintptr_t>(end, reinterpret_cast<uintptr_t>(stop)) : at + page;
      if (mapped && sceKernelMprotect(reinterpret_cast<void*>(at), size_t(next - at), 0) != 0) {
        return false;
      }
      at = next;
    }
    return true;
  }
  return sceKernelMprotect(reinterpret_cast<void*>(begin), size_t(end - begin),
                           KernelProtection(access)) == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;
  length = 0;
  const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
  const size_t page = host_page();
  {
    std::lock_guard lock(g_lock);
    Mapping* mapping;
    View* view;
    if (FindView(address, &mapping, &view)) {
      // The view's own record: the run of pages with this page's protection
      const size_t first = view->page_index(address);
      size_t last = first;
      while (last + 1 < view->access.size() && view->access[last + 1] == view->access[first]) {
        ++last;
      }
      access_out = view->access[first];
      length = (view->base + (last + 1) * page) - address;
      return true;
    }
  }
  void* start = nullptr;
  void* stop = nullptr;
  uint32_t protection = 0;
  if (sceKernelQueryMemoryProtection(base_address, &start, &stop, &protection) == 0 &&
      reinterpret_cast<uintptr_t>(start) <= address && reinterpret_cast<uintptr_t>(stop) > address) {
    length = size_t(reinterpret_cast<uintptr_t>(stop) - address);
    access_out = FromKernelProtection(protection);
    return true;
  }
  // Reserved and unbacked (the query describes mappings only): no access, a
  // page at a time
  std::lock_guard lock(g_lock);
  if (FindRange(address)) {
    length = page - (address & (page - 1));
    return true;
  }
  return false;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
  (void)path;    // a mapping is the title's own: nothing else opens it by name
  (void)access;  // views set their protection
  auto mapping = std::make_unique<Mapping>();
  mapping->length = rex::round_up(length, host_page());
  mapping->chunks.resize((mapping->length + kChunk - 1) / kChunk);
  std::lock_guard lock(g_lock);
  if (commit) {
    for (size_t chunk = 0; chunk < mapping->chunks.size(); ++chunk) {
      if (!BackChunk(*mapping, chunk)) {
        for (auto& backed : mapping->chunks) {
          ps5_shm_destroy(&backed);
        }
        return kFileMappingHandleInvalid;
      }
    }
  }
  const auto handle = reinterpret_cast<FileMappingHandle>(mapping.get());
  g_mappings.push_back(std::move(mapping));
  return handle;
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  (void)path;
  std::lock_guard lock(g_lock);
  auto it = std::find_if(g_mappings.begin(), g_mappings.end(), [&](const auto& mapping) {
    return reinterpret_cast<FileMappingHandle>(mapping.get()) == handle;
  });
  if (it == g_mappings.end()) {
    return;
  }
  // Every view must be unmapped first (Memory's destructor does)
  assert_true((*it)->views.empty());
  for (auto& chunk : (*it)->chunks) {
    ps5_shm_destroy(&chunk);
  }
  g_mappings.erase(it);
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  (void)access;  // a view's chunks are mapped read-write; commits protect
  const size_t page = host_page();
  if (file_offset % page != 0 || length % page != 0 || length == 0) {
    return nullptr;
  }
  std::lock_guard lock(g_lock);
  Mapping* mapping = FindMapping(handle);
  if (!mapping || file_offset + length > mapping->length) {
    return nullptr;
  }

  void* placed = base_address;
  const int reserved = base_address ? ps5_vrange_reserve_at(base_address, length)
                                    : ps5_vrange_reserve(length, nullptr, kUnit, &placed);
  if (reserved != 0) {
    return nullptr;
  }
  View view{reinterpret_cast<uintptr_t>(placed), length, file_offset,
            std::vector<PageAccess>(length / page, PageAccess::kNoAccess)};
  const size_t first = file_offset / kChunk;
  const size_t last = (file_offset + length - 1) / kChunk;
  for (size_t chunk = first; chunk <= last; ++chunk) {
    if (mapping->chunks[chunk].bytes && !MapChunkIntoView(*mapping, chunk, view)) {
      ps5_vrange_release(placed, length);
      return nullptr;
    }
  }
  mapping->views.push_back(std::move(view));
  return placed;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  std::lock_guard lock(g_lock);
  Mapping* mapping = FindMapping(handle);
  if (!mapping) {
    return false;
  }
  auto view = std::find_if(mapping->views.begin(), mapping->views.end(), [&](const View& v) {
    return v.base == reinterpret_cast<uintptr_t>(base_address);
  });
  if (view == mapping->views.end()) {
    return false;
  }
  // Each backed chunk's part of the view, then the reservation around them
  const size_t first = view->file_offset / kChunk;
  const size_t last = (view->file_offset + view->length - 1) / kChunk;
  for (size_t chunk = first; chunk <= last; ++chunk) {
    size_t begin, end;
    ChunkInView(*mapping, chunk, *view, &begin, &end);
    if (mapping->chunks[chunk].bytes && begin != end) {
      ps5_shm_unmap(reinterpret_cast<void*>(view->base + (begin - view->file_offset)),
                    end - begin, 0);
    }
  }
  mapping->views.erase(view);
  return ps5_vrange_release(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex

#endif  // REX_PLATFORM_PS5
