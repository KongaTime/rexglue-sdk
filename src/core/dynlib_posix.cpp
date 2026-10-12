#include <rex/platform.h>
#include <rex/platform/dynlib.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC || REX_PLATFORM_PS5, "This file is POSIX-only");

#include <dlfcn.h>

#if REX_PLATFORM_PS5
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#endif

namespace rex::platform {

#if REX_PLATFORM_PS5
namespace {
struct StaticLibrary {
  std::string name;
  std::vector<StaticSymbol> symbols;
};

std::mutex& StaticLibrariesMutex() {
  static std::mutex mutex;
  return mutex;
}

// Never freed: a handle points at its entry for the life of the title
std::vector<StaticLibrary*>& StaticLibraries() {
  static std::vector<StaticLibrary*> libraries;
  return libraries;
}

StaticLibrary* FindStaticLibrary(const std::string& name) {
  std::lock_guard lock(StaticLibrariesMutex());
  for (StaticLibrary* library : StaticLibraries())
    if (library->name == name)
      return library;
  return nullptr;
}
}  // namespace

void RegisterStaticLibrary(const char* name, const StaticSymbol* symbols, size_t count) {
  auto* library = new StaticLibrary{name, std::vector<StaticSymbol>(symbols, symbols + count)};
  std::lock_guard lock(StaticLibrariesMutex());
  StaticLibraries().push_back(library);
}
#endif

DynamicLibrary::~DynamicLibrary() {
  Close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
#if REX_PLATFORM_PS5
  static_ = other.static_;
  other.static_ = false;
#endif
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
#if REX_PLATFORM_PS5
    static_ = other.static_;
    other.static_ = false;
#endif
  }
  return *this;
}

bool DynamicLibrary::Load(const std::filesystem::path& path, SymbolResolution mode) {
  Close();
#if REX_PLATFORM_PS5
  if (StaticLibrary* library = FindStaticLibrary(path.filename().string())) {
    handle_ = library;
    static_ = true;
    return true;
  }
#endif
  int flags = (mode == SymbolResolution::kImmediate) ? RTLD_NOW : RTLD_LAZY;
  handle_ = dlopen(path.c_str(), flags);
  return handle_ != nullptr;
}

void DynamicLibrary::Close() {
#if REX_PLATFORM_PS5
  if (static_) {
    handle_ = nullptr;
    static_ = false;
    return;
  }
#endif
  if (handle_) {
    dlclose(handle_);
    handle_ = nullptr;
  }
}

void* DynamicLibrary::GetRawSymbol(const char* name) const {
  if (!handle_)
    return nullptr;
#if REX_PLATFORM_PS5
  if (static_) {
    for (const StaticSymbol& symbol : static_cast<const StaticLibrary*>(handle_)->symbols)
      if (std::strcmp(symbol.name, name) == 0)
        return symbol.address;
    return nullptr;
  }
#endif
  return dlsym(handle_, name);
}

}  // namespace rex::platform
