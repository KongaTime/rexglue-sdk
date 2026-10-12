// Ported from TheSimpsonsGame-PS5-Builder (LoZazaMastro and contributors),
// tools/rexglue-sdk/include/rex/ui/window_prospero.h at 59e7086, BSD 3-Clause
// License (see LICENSE).
#pragma once

#include <atomic>
#include <memory>

#include <rex/ui/window.h>

namespace rex::ui {

class ProsperoWindow final : public Window {
 public:
  ProsperoWindow(WindowedAppContext& app_context, std::string_view title,
                 uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~ProsperoWindow() override;

  // The one display the title presents to
  bool GetDisplayPixelSize(uint32_t& width, uint32_t& height) const override {
    width = GetActualPhysicalWidth();
    height = GetActualPhysicalHeight();
    return width && height;
  }

 protected:
  uint32_t GetLatestDpiImpl() const override { return 288; }
  bool OpenImpl() override;
  void RequestCloseImpl() override;
  void ApplyNewFullscreen() override;
  void FocusImpl() override;
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  void PollInput();

  struct DispatchState {
    // This pointer is read/written only on the UI thread. Painting and input
    // callbacks may destroy the window, so the context keeps a shared token.
    ProsperoWindow* window = nullptr;
    std::atomic<bool> paint_pending{false};
  };
  std::shared_ptr<DispatchState> dispatch_state_;
  uint64_t window_pump_token_ = 0;
  bool settings_chord_down_ = false;
};

}  // namespace rex::ui
