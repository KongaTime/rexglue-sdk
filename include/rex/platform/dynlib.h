/**
 * @file        platform/dynlib.h
 * @brief       Platform-agnostic dynamic library loading
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <rex/platform.h>

namespace rex::platform {

enum class SymbolResolution {
  // Resolve symbols on first use. Maps to RTLD_LAZY on POSIX; the only mode on
  // Windows.
  kLazy,
  // Resolve all symbols at load time. Load fails if any unresolved symbol
  // exists. Maps to RTLD_NOW on POSIX; the only mode on Windows.
  kImmediate,
};

class DynamicLibrary {
 public:
  DynamicLibrary() = default;
  ~DynamicLibrary();

  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  DynamicLibrary(DynamicLibrary&& other) noexcept;
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

  bool Load(const std::filesystem::path& path, SymbolResolution mode = SymbolResolution::kLazy);
  void Close();
  explicit operator bool() const { return handle_ != nullptr; }

  void* GetRawSymbol(const char* name) const;

  template <typename T>
  T GetSymbol(const char* name) const {
    return reinterpret_cast<T>(GetRawSymbol(name));
  }

 private:
  void* handle_ = nullptr;
#if REX_PLATFORM_PS5
  // handle_ is a library the title links in (RegisterStaticLibrary), not dlopen's
  bool static_ = false;
#endif
};

#if REX_PLATFORM_PS5
// A title cannot dlopen code of its own: a library it links in instead (a game's
// recompiled DLL module) registers its symbols under the name Load is given, and
// Load finds them there by that path's file name.
struct StaticSymbol {
  const char* name;
  void* address;
};
void RegisterStaticLibrary(const char* name, const StaticSymbol* symbols, size_t count);
#endif

namespace lib_names {

#if REX_PLATFORM_WIN32

inline constexpr const char* kVulkanLoader = "vulkan-1.dll";
inline constexpr const char* kRenderDoc = "renderdoc.dll";
inline constexpr const char* kSpirvToolsSdkPath = "Bin/SPIRV-Tools-shared.dll";

#elif REX_PLATFORM_ANDROID

inline constexpr const char* kVulkanLoader = "libvulkan.so";
inline constexpr const char* kRenderDoc = "libVkLayer_GLES_RenderDoc.so";
inline constexpr const char* kSpirvToolsSdkPath = "bin/libSPIRV-Tools-shared.so";

#elif REX_PLATFORM_LINUX

inline constexpr const char* kVulkanLoader = "libvulkan.so.1";
inline constexpr const char* kRenderDoc = "librenderdoc.so";
inline constexpr const char* kSpirvToolsSdkPath = "bin/libSPIRV-Tools-shared.so";

#elif REX_PLATFORM_MAC

inline constexpr const char* kVulkanLoader = "libvulkan.1.dylib";
inline constexpr const char* kRenderDoc = "librenderdoc.dylib";
inline constexpr const char* kSpirvToolsSdkPath = "lib/libSPIRV-Tools-shared.dylib";

#elif REX_PLATFORM_PS5

// The console loads no libraries at run time: a title links everything it uses,
// RADV included. Lookups of these names fail, and callers fall back.
inline constexpr const char* kVulkanLoader = "";
inline constexpr const char* kRenderDoc = "";
inline constexpr const char* kSpirvToolsSdkPath = "";

#else
#error No library names provided for the target platform.
#endif

}  // namespace lib_names

}  // namespace rex::platform
