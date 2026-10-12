// Ported from TheSimpsonsGame-PS5-Builder (LoZazaMastro and contributors),
// tools/rexglue-sdk/include/rex/audio/prospero/pcm_conversion.h at 59e7086,
// BSD 3-Clause License (see LICENSE); the stereo fold and output gain are
// ReXGlue v0.10.0's (downmix.h), as its SDL driver uses them.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>

namespace rex::audio::prospero {

inline constexpr size_t kSamplesPerFrame = 256;
inline constexpr size_t kGuestChannels = 6;
inline constexpr size_t kOutputChannels = 2;
inline constexpr uint32_t kSampleRate = 48000;
using PcmFrame = std::array<int16_t, kSamplesPerFrame * kOutputChannels>;

inline int16_t FloatToSigned16(float value) {
  // Inspect the representation so fast-math cannot turn a non-finite value
  // into an undefined float-to-integer conversion.
  if ((std::bit_cast<uint32_t>(value) & 0x7f800000u) == 0x7f800000u) {
    return 0;
  }
  if (value >= 1.0f) {
    return 32767;
  }
  if (value <= -1.0f) {
    return -32768;
  }
  return static_cast<int16_t>(value >= 0.0f ? value * 32767.0f + 0.5f
                                           : value * 32768.0f - 0.5f);
}

inline PcmFrame ConvertGuestFrame(const float* guest_planar_be) {
  std::array<float, kSamplesPerFrame * kOutputChannels> stereo{};
  // Guest planes are FL, FR, FC, LFE, BL, BR, folded to stereo with ReXGlue's
  // configured fold and gain, as its SDL driver does. The verified AudioOut ABI
  // supports stereo; do not open an invented 6ch format or send six channels
  // into a stereo port.
  conversion::sequential_6_BE_to_interleaved_2_LE(stereo.data(), guest_planar_be,
                                                 kSamplesPerFrame, GetStereoFold(),
                                                 GetOutputGain());
  PcmFrame frame{};
  for (size_t i = 0; i < frame.size(); ++i) {
    frame[i] = FloatToSigned16(stereo[i]);
  }
  return frame;
}

}  // namespace rex::audio::prospero
