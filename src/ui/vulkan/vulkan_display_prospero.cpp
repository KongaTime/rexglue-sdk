// Ported from TheSimpsonsGame-PS5-Builder (LoZazaMastro and contributors),
// tools/rexglue-sdk/src/ui/vulkan/vulkan_display_prospero.cpp at 59e7086, BSD
// 3-Clause License (see LICENSE). Changes: for the RADV of PS5_Vulkan, whose
// VideoOut WSI takes 119.88 Hz from the title's param.json and offers those modes
// only when VideoOut runs at them. So the request sets no environment variable,
// the output clock is read from the advertised modes (not the port's
// simpsons_radv_videoout_refresh_millihertz), and a 60 Hz request on a 119.88 Hz
// output shows each flip for two vblanks (wsi_videoout_set_flip_rate).
#include <rex/ui/vulkan/display_prospero.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/surface_prospero.h>

extern "C" int sceSystemServiceHideSplashScreen();
// RADV's VideoOut WSI (PS5_Vulkan): each flip shows for at least rate + 1 vblanks.
extern "C" int wsi_videoout_set_flip_rate(int rate);

REXCVAR_DEFINE_UINT32(prospero_output_hz, 60, "UI/PS5",
                      "PS5 display output: 60 or 120 Hz (experimental). 120 Hz needs the "
                      "title's param.json to declare high-frame-rate output. Requires a "
                      "restart; independent of the game's own frame rate.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::ui::vulkan {
namespace {
constexpr uint32_t kRefreshMilliHertz = 59940;
constexpr uint32_t kHighRefreshMilliHertz = 119880;

constinit std::atomic<uint32_t> requested_output_hz{60};
constinit std::atomic<uint32_t> effective_output_millihertz{0};

template <typename T, typename Enumerator>
VkResult Enumerate(Enumerator enumerate, std::vector<T>& items) {
  // Display topology is normally fixed for a title, but handle a count change
  // without reading unwritten entries or spinning forever on VK_INCOMPLETE.
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    uint32_t count = 0;
    VkResult result = enumerate(&count, nullptr);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
      return result;
    }
    items.resize(count);
    if (!count) {
      return VK_SUCCESS;
    }
    result = enumerate(&count, items.data());
    if (result == VK_INCOMPLETE) {
      continue;
    }
    if (result == VK_SUCCESS) {
      items.resize(count);
    }
    return result;
  }
  items.clear();
  return VK_INCOMPLETE;
}

bool FitsExtent(VkExtent2D extent, VkExtent2D minimum, VkExtent2D maximum) {
  return extent.width >= minimum.width && extent.height >= minimum.height &&
         extent.width <= maximum.width && extent.height <= maximum.height;
}

bool FitsOrigin(VkOffset2D minimum, VkOffset2D maximum) {
  return minimum.x <= 0 && minimum.y <= 0 && maximum.x >= 0 && maximum.y >= 0;
}

bool IsHighRefresh(uint32_t millihertz) {
  return millihertz >= 119000 && millihertz <= 121000;
}

bool IsStandardRefresh(uint32_t millihertz) {
  return millihertz >= 59000 && millihertz <= 61000;
}

}  // namespace

bool InitializeProsperoDisplayPolicy() {
  const uint32_t configured = REXCVAR_GET(prospero_output_hz);
  const uint32_t requested = configured == 120 ? 120 : 60;
  if (configured != requested) {
    REXLOG_WARN("PS5 display: unsupported output request {} Hz; using {} Hz", configured,
                requested);
  }
  requested_output_hz.store(requested, std::memory_order_relaxed);
  effective_output_millihertz.store(0, std::memory_order_relaxed);
  REXLOG_INFO("PS5 display: requested {} Hz; the game's frame rate is configured separately",
              requested);
  return true;
}

uint32_t GetProsperoRequestedOutputHz() {
  return requested_output_hz.load(std::memory_order_relaxed);
}

uint32_t GetProsperoOutputMilliHertz() {
  return effective_output_millihertz.load(std::memory_order_acquire);
}

VkResult CreateProsperoDisplaySurface(const VulkanInstance& instance,
                                      VkPhysicalDevice physical_device,
                                      VkSurfaceKHR* surface_out) {
  if (!surface_out || !physical_device || !instance.instance()) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  *surface_out = VK_NULL_HANDLE;
  effective_output_millihertz.store(0, std::memory_order_release);
  const uint32_t requested_hz = GetProsperoRequestedOutputHz();
  const auto& fn = instance.functions();
  if (!instance.extensions().ext_KHR_display || !fn.vkGetPhysicalDeviceDisplayPropertiesKHR ||
      !fn.vkGetPhysicalDeviceDisplayPlanePropertiesKHR ||
      !fn.vkGetDisplayPlaneSupportedDisplaysKHR || !fn.vkGetDisplayModePropertiesKHR ||
      !fn.vkGetDisplayPlaneCapabilitiesKHR || !fn.vkCreateDisplayPlaneSurfaceKHR) {
    REXLOG_ERROR("PS5 RADV does not provide the required VK_KHR_display functions");
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }

  std::vector<VkDisplayPropertiesKHR> displays;
  VkResult result = Enumerate<VkDisplayPropertiesKHR>(
      [&](uint32_t* count, VkDisplayPropertiesKHR* values) {
        return fn.vkGetPhysicalDeviceDisplayPropertiesKHR(physical_device, count, values);
      },
      displays);
  if (result != VK_SUCCESS || displays.empty()) {
    REXLOG_ERROR("PS5 RADV reports no display (result {})", int32_t(result));
    return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
  }

  std::vector<VkDisplayPlanePropertiesKHR> planes;
  result = Enumerate<VkDisplayPlanePropertiesKHR>(
      [&](uint32_t* count, VkDisplayPlanePropertiesKHR* values) {
        return fn.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical_device, count, values);
      },
      planes);
  if (result != VK_SUCCESS || planes.empty()) {
    REXLOG_ERROR("PS5 RADV reports no display plane (result {})", int32_t(result));
    return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
  }

  struct Candidate {
    VkDisplayKHR display;
    VkDisplayModePropertiesKHR mode;
    uint32_t distance_from_request;
    // VideoOut runs at 119.88 Hz: the display offers that mode.
    bool output_high_refresh;
  };
  std::vector<Candidate> candidates;
  for (const auto& display : displays) {
    if (!(display.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) {
      continue;
    }
    std::vector<VkDisplayModePropertiesKHR> modes;
    result = Enumerate<VkDisplayModePropertiesKHR>(
        [&](uint32_t* count, VkDisplayModePropertiesKHR* values) {
          return fn.vkGetDisplayModePropertiesKHR(physical_device, display.display, count, values);
        },
        modes);
    if (result != VK_SUCCESS) {
      continue;
    }
    const bool output_high_refresh =
        std::any_of(modes.begin(), modes.end(), [](const VkDisplayModePropertiesKHR& mode) {
          return IsHighRefresh(mode.parameters.refreshRate);
        });
    for (const auto& mode : modes) {
      const auto& p = mode.parameters;
      if (p.visibleRegion.width != ProsperoDisplaySurface::kWidth ||
          p.visibleRegion.height != ProsperoDisplaySurface::kHeight ||
          !(IsStandardRefresh(p.refreshRate) ||
            (requested_hz == 120 && IsHighRefresh(p.refreshRate)))) {
        continue;
      }
      const uint32_t target = requested_hz * 1000;
      const uint32_t distance =
          p.refreshRate > target ? p.refreshRate - target : target - p.refreshRate;
      candidates.push_back({display.display, mode, distance, output_high_refresh});
    }
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
    return a.distance_from_request < b.distance_from_request;
  });

  for (const auto& candidate : candidates) {
    for (uint32_t plane_index = 0; plane_index < planes.size(); ++plane_index) {
      const auto& plane = planes[plane_index];
      if (plane.currentDisplay && plane.currentDisplay != candidate.display) {
        continue;
      }
      std::vector<VkDisplayKHR> supported;
      result = Enumerate<VkDisplayKHR>(
          [&](uint32_t* count, VkDisplayKHR* values) {
            return fn.vkGetDisplayPlaneSupportedDisplaysKHR(physical_device, plane_index, count,
                                                            values);
          },
          supported);
      if (result != VK_SUCCESS ||
          std::find(supported.begin(), supported.end(), candidate.display) == supported.end()) {
        continue;
      }
      VkDisplayPlaneCapabilitiesKHR capabilities{};
      result = fn.vkGetDisplayPlaneCapabilitiesKHR(physical_device, candidate.mode.displayMode,
                                                   plane_index, &capabilities);
      const VkExtent2D extent = candidate.mode.parameters.visibleRegion;
      if (result != VK_SUCCESS ||
          !FitsExtent(extent, capabilities.minSrcExtent, capabilities.maxSrcExtent) ||
          !FitsExtent(extent, capabilities.minDstExtent, capabilities.maxDstExtent) ||
          !FitsOrigin(capabilities.minSrcPosition, capabilities.maxSrcPosition) ||
          !FitsOrigin(capabilities.minDstPosition, capabilities.maxDstPosition)) {
        continue;
      }
      VkDisplayPlaneAlphaFlagBitsKHR alpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
      if (!(capabilities.supportedAlpha & alpha)) {
        if (capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR) {
          alpha = VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR;
        } else {
          continue;
        }
      }
      VkDisplaySurfaceCreateInfoKHR create{};
      create.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
      create.displayMode = candidate.mode.displayMode;
      create.planeIndex = plane_index;
      create.planeStackIndex = plane.currentStackIndex;
      create.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
      create.globalAlpha = 1.0f;
      create.alphaMode = alpha;
      create.imageExtent = extent;
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      result = fn.vkCreateDisplayPlaneSurfaceKHR(instance.instance(), &create, nullptr, &surface);
      if (result != VK_SUCCESS || surface == VK_NULL_HANDLE) {
        continue;
      }
      // The output runs at 119.88 Hz whichever of its modes the surface takes.
      // For a 60 Hz request there, each flip is shown for two vblanks.
      const uint32_t output =
          candidate.output_high_refresh ? kHighRefreshMilliHertz : kRefreshMilliHertz;
      uint32_t effective = output;
      if (output == kHighRefreshMilliHertz && requested_hz != 120) {
        const int flip_rate_result = wsi_videoout_set_flip_rate(1);
        if (flip_rate_result == 0) {
          effective = kRefreshMilliHertz;
        } else {
          REXLOG_WARN("PS5 display: pacing a 119.88 Hz output to 59.94 Hz failed ({:08X})",
                      uint32_t(flip_rate_result));
        }
      }
      *surface_out = surface;
      effective_output_millihertz.store(effective, std::memory_order_release);
      REXLOG_INFO(
          "PS5 display: {}x{}, requested {} Hz, mode {:.3f} Hz, output {:.3f} Hz, "
          "presents at {:.3f} Hz, plane {}",
          extent.width, extent.height, requested_hz,
          double(candidate.mode.parameters.refreshRate) / 1000.0, double(output) / 1000.0,
          double(effective) / 1000.0, plane_index);
      if (requested_hz == 120 && effective != kHighRefreshMilliHertz) {
        REXLOG_WARN(
            "PS5 display: 120 Hz is unavailable for this session (the title's param.json "
            "must declare it, and the display take it); using 59.94 Hz");
      }
      const int splash_result = sceSystemServiceHideSplashScreen();
      if (splash_result != 0) {
        REXLOG_WARN("sceSystemServiceHideSplashScreen returned {:08X}", uint32_t(splash_result));
      }
      return VK_SUCCESS;
    }
  }
  REXLOG_ERROR(
      "PS5 display has no usable 3840x2160 mode for the {} Hz request and compatible plane",
      requested_hz);
  return VK_ERROR_INITIALIZATION_FAILED;
}

}  // namespace rex::ui::vulkan
