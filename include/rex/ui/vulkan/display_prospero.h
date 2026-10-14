// Ported from TheSimpsonsGame-PS5-Builder (LoZazaMastro and contributors),
// tools/rexglue-sdk/include/rex/ui/vulkan/display_prospero.h at 59e7086, BSD
// 3-Clause License (see LICENSE).
#pragma once

#include <cstdint>

#include <rex/ui/vulkan/instance.h>

namespace rex::ui::vulkan {

// The output clock is separate from the guest game's 30/60 FPS clock. The
// request (the prospero_output_hz cvar) is captured after the configuration is
// loaded, before the driver opens VideoOut (VulkanInstance::Create calls this).
// The effective clock is published only once a surface exists.
bool InitializeProsperoDisplayPolicy();
uint32_t GetProsperoRequestedOutputHz();
// 59940 or 119880 once CreateProsperoDisplaySurface succeeded, 0 before.
uint32_t GetProsperoOutputMilliHertz();

// Selects an advertised 3840x2160 mode and a plane that can show it. A 120 Hz
// request prefers 119.88 Hz and falls back to 59.94 Hz when that is all the
// display offers; the default is 59.94 Hz. RADV's VideoOut WSI (PS5_Vulkan)
// offers the 119.88 Hz modes only when the title's param.json declares
// high-frame-rate output (attribute3 0x40) and VideoOut runs at it.
VkResult CreateProsperoDisplaySurface(const VulkanInstance& instance,
                                      VkPhysicalDevice physical_device,
                                      VkSurfaceKHR* surface_out);

}  // namespace rex::ui::vulkan
