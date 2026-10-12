// The PS5 controller driver (include/rex/input/prospero/prospero_input_driver.h
// says where each part comes from). BSD 3-Clause License (see LICENSE).
#include <rex/input/prospero/prospero_input_driver.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <tuple>

#include <rex/chrono/clock.h>
#include <rex/input/prospero/pad_mapping.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/virtual_key.h>

#if !REX_PLATFORM_PROSPERO
#error The PS5 input backend requires a native PS5 build.
#endif

namespace rex::input::prospero {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kRepeatDelayMs = 400;
constexpr uint32_t kRepeatRateMs = 100;

// The XInput virtual keys, in PadKeyField's bit order (pad_mapping.h): the
// button field, then the triggers and the sticks' directions.
constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
    rex::ui::VirtualKey::kXInputPadDpadUp,
    rex::ui::VirtualKey::kXInputPadDpadDown,
    rex::ui::VirtualKey::kXInputPadDpadLeft,
    rex::ui::VirtualKey::kXInputPadDpadRight,
    rex::ui::VirtualKey::kXInputPadStart,
    rex::ui::VirtualKey::kXInputPadBack,
    rex::ui::VirtualKey::kXInputPadLThumbPress,
    rex::ui::VirtualKey::kXInputPadRThumbPress,
    rex::ui::VirtualKey::kXInputPadLShoulder,
    rex::ui::VirtualKey::kXInputPadRShoulder,
    rex::ui::VirtualKey::kNone, /* Guide has no VK */
    rex::ui::VirtualKey::kNone, /* Unknown */
    rex::ui::VirtualKey::kXInputPadA,
    rex::ui::VirtualKey::kXInputPadB,
    rex::ui::VirtualKey::kXInputPadX,
    rex::ui::VirtualKey::kXInputPadY,
    rex::ui::VirtualKey::kXInputPadLTrigger,
    rex::ui::VirtualKey::kXInputPadRTrigger,
    rex::ui::VirtualKey::kXInputPadLThumbUp,
    rex::ui::VirtualKey::kXInputPadLThumbDown,
    rex::ui::VirtualKey::kXInputPadLThumbRight,
    rex::ui::VirtualKey::kXInputPadLThumbLeft,
    rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
    rex::ui::VirtualKey::kXInputPadLThumbUpRight,
    rex::ui::VirtualKey::kXInputPadLThumbDownRight,
    rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
    rex::ui::VirtualKey::kXInputPadRThumbUp,
    rex::ui::VirtualKey::kXInputPadRThumbDown,
    rex::ui::VirtualKey::kXInputPadRThumbRight,
    rex::ui::VirtualKey::kXInputPadRThumbLeft,
    rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
    rex::ui::VirtualKey::kXInputPadRThumbUpRight,
    rex::ui::VirtualKey::kXInputPadRThumbDownRight,
    rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
};

enum class RepeatState { kIdle, kWaiting, kRepeating };

// One player slot: a logged-in user, their pad, and what the guest last saw.
struct Slot {
  int user_id = -1;
  int handle = -1;
  bool owned = false;  // opened here, so closed here
  bool connected = false;
  bool rumble = false;
  bool vibration_known = false;
  DeviceId id = DeviceId::kInvalid;  // a new one each time it connects
  uint32_t last_error = 0;
  X_INPUT_GAMEPAD gamepad{};
  PadVibration last_vibration{};
  Clock::time_point next_open{};
  // What GetDeviceState last reported (neutral while the overlays have input)
  X_INPUT_GAMEPAD reported{};
  uint32_t reported_packet = 0;
  // Keystrokes
  uint64_t key_buttons = 0;
  RepeatState repeat_state = RepeatState::kIdle;
  uint8_t repeat_index = 0;
  uint32_t repeat_time = 0;
};

class PadManager {
 public:
  ~PadManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
      CloseSlot(slot);
    }
  }

  bool Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    return InitializeLocked();
  }

  void Enumerate(std::vector<DeviceInfo>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!InitializeLocked()) {
      return;
    }
    RefreshUsersLocked();
    for (auto& slot : slots_) {
      if (slot.user_id < 0 || !ReadLocked(slot)) {
        continue;
      }
      DeviceInfo info;
      info.id = slot.id;
      info.name = "DualSense (user " + std::to_string(slot.user_id) + ")";
      info.guid = "ps5-pad-user-" + std::to_string(slot.user_id);
      info.synthetic = false;
      out.push_back(std::move(info));
    }
  }

  X_RESULT PollSlot(uint32_t index, X_INPUT_STATE* out_state) {
    if (!out_state || index >= kPadUserCount) {
      return X_ERROR_INVALID_PARAMETER;
    }
    *out_state = {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!InitializeLocked()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    RefreshUsersLocked();
    auto& slot = slots_[index];
    if (slot.user_id < 0 || !ReadLocked(slot)) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    out_state->packet_number = slot.reported_packet;
    out_state->gamepad = slot.gamepad;
    return X_ERROR_SUCCESS;
  }

  X_RESULT State(DeviceId id, bool active, X_INPUT_STATE* out_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = Find(id);
    if (!slot || !ReadLocked(*slot) || slot->id != id) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    const X_INPUT_GAMEPAD gamepad = active ? slot->gamepad : X_INPUT_GAMEPAD{};
    if (std::memcmp(&gamepad, &slot->reported, sizeof(gamepad)) != 0) {
      slot->reported = gamepad;
      ++slot->reported_packet;
    }
    out_state->packet_number = slot->reported_packet;
    out_state->gamepad = slot->reported;
    return X_ERROR_SUCCESS;
  }

  X_RESULT Capabilities(DeviceId id, X_INPUT_CAPABILITIES* out_caps) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = Find(id);
    if (!slot || !ReadLocked(*slot) || slot->id != id) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out_caps = {};
    out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
    out_caps->sub_type = 1;
    out_caps->flags = uint16_t(slot->rumble ? X_INPUT_CAPS_FFB_SUPPORTED : 0);
    out_caps->gamepad.buttons = 0xf3ff;
    out_caps->gamepad.left_trigger = 0xff;
    out_caps->gamepad.right_trigger = 0xff;
    out_caps->gamepad.thumb_lx = out_caps->gamepad.thumb_ly = int16_t(32767);
    out_caps->gamepad.thumb_rx = out_caps->gamepad.thumb_ry = int16_t(32767);
    out_caps->vibration.left_motor_speed = uint16_t(slot->rumble ? 0xffff : 0);
    out_caps->vibration.right_motor_speed = uint16_t(slot->rumble ? 0xffff : 0);
    return X_ERROR_SUCCESS;
  }

  X_RESULT Rumble(DeviceId id, uint16_t large, uint16_t small) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = Find(id);
    if (!slot || slot->handle < 0 || !slot->connected) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!slot->rumble) {
      return X_ERROR_FUNCTION_FAILED;
    }
    const PadVibration vibration{uint8_t(large >> 8), uint8_t(small >> 8)};
    if (slot->vibration_known && vibration.large_motor == slot->last_vibration.large_motor &&
        vibration.small_motor == slot->last_vibration.small_motor) {
      return X_ERROR_SUCCESS;
    }
    const int result = scePadSetVibration(slot->handle, &vibration);
    if (result != 0) {
      LogError(*slot, "scePadSetVibration", result);
      return X_ERROR_FUNCTION_FAILED;
    }
    slot->last_vibration = vibration;
    slot->vibration_known = true;
    return X_ERROR_SUCCESS;
  }

  X_RESULT Keystroke(DeviceId id, bool active, X_INPUT_KEYSTROKE* out_keystroke) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = Find(id);
    if (!slot || !ReadLocked(*slot) || slot->id != id) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    // While the overlays have input, every button reads as up: the keys go up
    // when they take it and down again when they give it back.
    const uint64_t current = active ? PadKeyField(slot->gamepad) : 0;
    const uint32_t now = uint32_t(rex::chrono::Clock::QueryGuestUptimeMillis());
    if (slot->repeat_state == RepeatState::kWaiting && slot->repeat_time + kRepeatDelayMs < now) {
      slot->repeat_state = RepeatState::kRepeating;
    }
    if (slot->repeat_state == RepeatState::kRepeating && slot->repeat_time + kRepeatRateMs < now) {
      slot->repeat_time = now;
      out_keystroke->virtual_key = uint16_t(kVkLookup.at(slot->repeat_index));
      out_keystroke->unicode = 0;
      // InputSystem stamps the guest user this device is assigned to.
      out_keystroke->user_index = 0;
      out_keystroke->hid_code = 0;
      out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
      return X_ERROR_SUCCESS;
    }
    const uint64_t changed = current ^ slot->key_buttons;
    if (!changed) {
      return X_ERROR_EMPTY;
    }
    // Keys going up first, as XInput does: THUMB_UPLEFT is up before
    // THUMB_LEFT is down.
    for (auto [clear_pass, pass] = std::tuple{true, 0}; pass < 2; clear_pass = false, pass++) {
      for (uint8_t i = 0; i < uint8_t(kVkLookup.size()); i++) {
        const uint64_t bit = uint64_t(1) << i;
        if (!(changed & bit) || kVkLookup[i] == rex::ui::VirtualKey::kNone) {
          continue;
        }
        out_keystroke->virtual_key = uint16_t(kVkLookup[i]);
        out_keystroke->unicode = 0;
        out_keystroke->user_index = 0;
        out_keystroke->hid_code = 0;
        const bool pressed = current & bit;
        if (clear_pass && !pressed) {
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
          slot->key_buttons &= ~bit;
          slot->repeat_state = RepeatState::kIdle;
          return X_ERROR_SUCCESS;
        }
        if (!clear_pass && pressed) {
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
          slot->key_buttons |= bit;
          slot->repeat_state = RepeatState::kWaiting;
          slot->repeat_index = i;
          slot->repeat_time = now;
          return X_ERROR_SUCCESS;
        }
      }
    }
    return X_ERROR_EMPTY;
  }

 private:
  Slot* Find(DeviceId id) {
    if (id == DeviceId::kInvalid) {
      return nullptr;
    }
    for (auto& slot : slots_) {
      if (slot.id == id) {
        return &slot;
      }
    }
    return nullptr;
  }

  // Opens the slot's pad when due and reads it: true when it is connected
  bool ReadLocked(Slot& slot) {
    if (slot.handle < 0 && Clock::now() >= slot.next_open) {
      slot.next_open = Clock::now() + std::chrono::milliseconds(500);
      slot.handle = scePadOpen(slot.user_id, kPadPortStandard, 0, nullptr);
      slot.owned = slot.handle >= 0;
      if (uint32_t(slot.handle) == kPadAlreadyOpened) {
        // The title opened it already (its platform layer's pad_open)
        slot.handle = scePadGetHandle(slot.user_id, kPadPortStandard, 0);
      }
      if (slot.handle < 0) {
        LogError(slot, "scePadOpen", slot.handle);
        return false;
      }
      slot.last_error = 0;
    }
    if (slot.handle < 0) {
      return false;
    }
    PadData native{};
    const int result = scePadReadState(slot.handle, &native);
    if (result != 0) {
      LogError(slot, "scePadReadState", result);
      CloseSlot(slot);
      return false;
    }
    if (!native.connected) {
      Disconnect(slot);
      return false;
    }
    if (!slot.connected) {
      slot.connected = true;
      slot.id = DeviceId(next_device_id_++);
      // Compatible mode exposes the two ordinary motors without creating a
      // second AudioOut vibration stream or changing the game's rumble model.
      const int mode = scePadSetVibrationMode(slot.handle, 2);
      slot.rumble = mode == 0;
      slot.vibration_known = false;
      if (mode != 0) {
        LogError(slot, "scePadSetVibrationMode", mode);
      }
      REXLOG_INFO("PS5 pad connected: user {}, handle {}", slot.user_id, slot.handle);
    }
    slot.gamepad = MapPadState(native);
    return true;
  }

  void Disconnect(Slot& slot) {
    if (slot.connected) {
      REXLOG_INFO("PS5 pad disconnected: user {}", slot.user_id);
    }
    slot.connected = false;
    slot.id = DeviceId::kInvalid;
    slot.rumble = false;
    slot.vibration_known = false;
    slot.gamepad = {};
    slot.last_vibration = {};
    slot.reported = {};
    slot.key_buttons = 0;
    slot.repeat_state = RepeatState::kIdle;
  }

  void CloseSlot(Slot& slot) {
    if (slot.handle >= 0) {
      if (slot.rumble) {
        const PadVibration zero{};
        scePadSetVibration(slot.handle, &zero);
      }
      if (slot.owned) {
        const int result = scePadClose(slot.handle);
        if (result != 0) {
          LogError(slot, "scePadClose", result);
        }
      }
    }
    slot.handle = -1;
    slot.owned = false;
    Disconnect(slot);
    slot.next_open = Clock::now() + std::chrono::milliseconds(500);
  }

  static void LogError(Slot& slot, const char* operation, int code) {
    if (slot.last_error != uint32_t(code)) {
      REXLOG_WARN("PS5 {} (user {}) failed: 0x{:08X}", operation, slot.user_id, uint32_t(code));
      slot.last_error = uint32_t(code);
    }
  }

  bool InitializeLocked() {
    if (initialization_attempted_) {
      return initialized_;
    }
    initialization_attempted_ = true;
    const int user_result = sceUserServiceInitialize(nullptr);
    if (user_result != 0 && uint32_t(user_result) != kUserServiceAlreadyInitialized) {
      REXLOG_ERROR("PS5 sceUserServiceInitialize failed: 0x{:08X}", uint32_t(user_result));
      return false;
    }
    const int pad_result = scePadInit();
    if (pad_result != 0) {
      REXLOG_ERROR("PS5 scePadInit failed: 0x{:08X}", uint32_t(pad_result));
      return false;
    }
    initialized_ = true;
    RefreshUsersLocked();
    return true;
  }

  // Player slots follow the logged-in users: the initial user first at start,
  // assignments kept across reordering, the system user 0xff never a player
  void RefreshUsersLocked() {
    const auto now = Clock::now();
    if (now < next_user_refresh_) {
      return;
    }
    next_user_refresh_ = now + std::chrono::milliseconds(500);
    std::array<int, kPadUserCount> users{-1, -1, -1, -1};
    const int result = sceUserServiceGetLoginUserIdList(users.data());
    if (result != 0) {
      if (last_user_error_ != uint32_t(result)) {
        REXLOG_WARN("PS5 sceUserServiceGetLoginUserIdList failed: 0x{:08X}", uint32_t(result));
        last_user_error_ = uint32_t(result);
      }
      return;
    }
    last_user_error_ = 0;
    int initial_user = -1;
    if (sceUserServiceGetInitialUser(&initial_user) == 0) {
      const auto initial = std::find(users.begin(), users.end(), initial_user);
      if (initial != users.end()) {
        std::iter_swap(users.begin(), initial);
      }
    }
    for (auto& slot : slots_) {
      if (slot.user_id >= 0 && std::find(users.begin(), users.end(), slot.user_id) == users.end()) {
        CloseSlot(slot);
        slot.user_id = -1;
      }
    }
    for (int user : users) {
      if (user < 0 || user == kPadSystemUser) {
        continue;
      }
      if (std::any_of(slots_.begin(), slots_.end(),
                      [user](const Slot& slot) { return slot.user_id == user; })) {
        continue;
      }
      const auto free = std::find_if(slots_.begin(), slots_.end(),
                                     [](const Slot& slot) { return slot.user_id < 0; });
      if (free != slots_.end()) {
        free->user_id = user;
        free->next_open = now;
        free->last_error = 0;
      }
    }
  }

  std::mutex mutex_;
  std::array<Slot, kPadUserCount> slots_{};
  bool initialization_attempted_ = false;
  bool initialized_ = false;
  uint32_t last_user_error_ = 0;
  Clock::time_point next_user_refresh_{};
  uint64_t next_device_id_ = 0x5053350000000001ull;  // "PS5", never reused
};

PadManager& Pads() {
  static PadManager pads;
  return pads;
}

}  // namespace

X_RESULT PollPadState(uint32_t user_index, X_INPUT_STATE* out_state) {
  return Pads().PollSlot(user_index, out_state);
}

ProsperoInputDriver::ProsperoInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

ProsperoInputDriver::~ProsperoInputDriver() = default;

X_STATUS ProsperoInputDriver::Setup() {
  return Pads().Initialize() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

void ProsperoInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  Pads().Enumerate(out);
}

X_RESULT ProsperoInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (!out_state) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  return Pads().State(id, is_active(), out_state);
}

X_RESULT ProsperoInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                                    X_INPUT_CAPABILITIES* out_caps) {
  // XAM forwards its any-user selector bit with the gamepad flag
  constexpr uint32_t kXamAnyUser = uint32_t(1) << 30;
  if (!out_caps || (flags & ~(uint32_t(X_INPUT_FLAG_GAMEPAD) | kXamAnyUser))) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  return Pads().Capabilities(id, out_caps);
}

X_RESULT ProsperoInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  const bool active = is_active();
  return Pads().Rumble(id, active ? uint16_t(vibration->left_motor_speed) : 0,
                       active ? uint16_t(vibration->right_motor_speed) : 0);
}

X_RESULT ProsperoInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                                 X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  return Pads().Keystroke(id, is_active(), out_keystroke);
}

}  // namespace rex::input::prospero
