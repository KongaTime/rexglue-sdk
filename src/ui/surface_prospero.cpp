// Ported from TheSimpsonsGame-PS5-Builder (LoZazaMastro and contributors),
// tools/rexglue-sdk/src/ui/surface_prospero.cpp at 59e7086, BSD 3-Clause
// License (see LICENSE).
#include <rex/ui/surface_prospero.h>

namespace rex::ui {

bool ProsperoDisplaySurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  width_out = kWidth;
  height_out = kHeight;
  return true;
}

}  // namespace rex::ui
