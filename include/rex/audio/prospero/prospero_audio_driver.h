// The PS5 audio driver: guest frames to sceAudioOut, 48 kHz S16 stereo.
//
// From TheSimpsonsGame-PS5-Builder (LoZazaMastro and contributors),
// tools/rexglue-sdk/{include,src}/rex/audio/prospero/prospero_audio_driver.* at
// 59e7086, BSD 3-Clause License (see LICENSE), without its diagnostics, trace
// checkpoints, dump files, live mute and thread priority.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/audio/prospero/pcm_conversion.h>
#include <rex/thread.h>

namespace rex::audio::prospero {

class ProsperoAudioDriver final : public AudioDriver {
 public:
  ProsperoAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~ProsperoAudioDriver() override;

  bool Initialize();
  void Shutdown();

  void SubmitFrame(uint32_t samples_ptr) override;

 private:
  void OutputThreadMain();

  // The client semaphore permits at most 64 outstanding frames
  static constexpr size_t kFrameCapacity = 64;

  rex::thread::Semaphore* semaphore_;
  int32_t port_ = -1;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> failed_{false};
  std::thread output_thread_;
  std::mutex frames_mutex_;
  std::array<PcmFrame, kFrameCapacity> frames_{};
  size_t read_position_ = 0;
  size_t frame_count_ = 0;
};

}  // namespace rex::audio::prospero
