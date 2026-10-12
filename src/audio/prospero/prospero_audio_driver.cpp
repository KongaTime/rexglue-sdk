// The PS5 audio driver (include/rex/audio/prospero/prospero_audio_driver.h says
// where it comes from). BSD 3-Clause License (see LICENSE).
#include <rex/audio/prospero/prospero_audio_driver.h>

#include <rex/audio/flags.h>
#include <rex/audio/prospero/audio_out.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/platform.h>

#if !REX_PLATFORM_PROSPERO
#error The PS5 audio backend requires a native PS5 build.
#endif

// The SDL driver defines it on the desktop; it is not built on the PS5
REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");

namespace rex::audio::prospero {

ProsperoAudioDriver::ProsperoAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

ProsperoAudioDriver::~ProsperoAudioDriver() {
  Shutdown();
}

bool ProsperoAudioDriver::Initialize() {
  const int32_t initialized = sceAudioOutInit();
  if (initialized < 0 && uint32_t(initialized) != kAudioAlreadyInitialized) {
    REXAPU_ERROR("PS5 sceAudioOutInit failed: 0x{:08X}", uint32_t(initialized));
    return false;
  }
  port_ = sceAudioOutOpen(kAudioSystemUser, kAudioPortMain, 0, kSamplesPerFrame, kSampleRate,
                          kAudioFormatS16Stereo);
  if (port_ < 0) {
    REXAPU_ERROR("PS5 sceAudioOutOpen (48 kHz, S16 stereo, 256 samples) failed: 0x{:08X}",
                 uint32_t(port_));
    return false;
  }
  // Verify output before publishing the driver to a guest client: a stub or
  // an unusable device must not report a successful silent audio backend.
  alignas(16) const PcmFrame initial_silence{};
  const int32_t output = sceAudioOutOutput(port_, initial_silence.data());
  if (output < 0) {
    REXAPU_ERROR("PS5 initial sceAudioOutOutput failed: 0x{:08X}", uint32_t(output));
    Shutdown();
    return false;
  }
  output_thread_ = std::thread([this] { OutputThreadMain(); });
  REXAPU_INFO("PS5 AudioOut port {} open: 48 kHz, S16 stereo, 256-sample periods", port_);
  return true;
}

void ProsperoAudioDriver::Shutdown() {
  stopping_ = true;
  // AudioOut paces each blocking output call to a 256/48000 s period. Join
  // before closing its handle or destroying its buffers.
  if (output_thread_.joinable()) {
    output_thread_.join();
  }
  if (port_ >= 0) {
    const int32_t result = sceAudioOutClose(port_);
    if (result < 0) {
      REXAPU_WARN("PS5 sceAudioOutClose failed: 0x{:08X}", uint32_t(result));
    }
    port_ = -1;
  }
  std::lock_guard<std::mutex> lock(frames_mutex_);
  frame_count_ = 0;
  read_position_ = 0;
}

void ProsperoAudioDriver::SubmitFrame(uint32_t samples_ptr) {
  if (stopping_ || failed_) {
    return;
  }
  // Copy the guest frame on submission: the mixer may reuse its planar source
  // buffer before AudioOut consumes the queued frame.
  const float* samples = memory_->TranslateVirtual<float*>(samples_ptr);
  const PcmFrame frame = REXCVAR_GET(audio_mute) ? PcmFrame{} : ConvertGuestFrame(samples);
  std::lock_guard<std::mutex> lock(frames_mutex_);
  if (stopping_ || failed_) {
    return;
  }
  if (frame_count_ == kFrameCapacity) {
    // A full queue is a broken accounting contract, not permission to
    // overwrite audio still in use or synthesize extra credits.
    REXAPU_ERROR("PS5 audio queue overflow; stopping this audio device");
    failed_ = true;
    return;
  }
  frames_[(read_position_ + frame_count_) % kFrameCapacity] = frame;
  ++frame_count_;
}

void ProsperoAudioDriver::OutputThreadMain() {
  while (!stopping_ && !failed_) {
    alignas(16) PcmFrame output{};
    bool had_frame = false;
    {
      std::lock_guard<std::mutex> lock(frames_mutex_);
      if (frame_count_) {
        output = frames_[read_position_];
        read_position_ = (read_position_ + 1) % kFrameCapacity;
        --frame_count_;
        had_frame = true;
      }
    }
    // Silence when the guest is late: AudioOut keeps its own pace
    const int32_t result = sceAudioOutOutput(port_, output.data());
    if (result < 0) {
      failed_ = true;
      REXAPU_ERROR("PS5 sceAudioOutOutput failed: 0x{:08X}; audio client stopped", uint32_t(result));
      break;
    }
    if (had_frame && !stopping_) {
      // A played frame frees one of the guest client's slots
      if (!semaphore_->Release(1, nullptr)) {
        failed_ = true;
        REXAPU_ERROR("PS5 audio frame semaphore overflow; audio client stopped");
        break;
      }
    }
  }
}

}  // namespace rex::audio::prospero
