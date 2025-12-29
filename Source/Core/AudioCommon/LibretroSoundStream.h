// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "AudioCommon/SoundStream.h"
#include "Common/Flag.h"

namespace AudioCommon
{
using LibretroAudioSampleBatch = std::size_t (*)(const int16_t* data, std::size_t frames);

void SetLibretroAudioSampleBatch(LibretroAudioSampleBatch cb);
LibretroAudioSampleBatch GetLibretroAudioSampleBatch();

class LibretroSoundStream final : public SoundStream
{
public:
  LibretroSoundStream();
  ~LibretroSoundStream() override;

  static bool IsValid();
  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;

private:
  void SoundLoop();

  std::thread m_thread;
  Common::Flag m_run_thread{false};
  std::atomic<bool> m_running{false};
  std::atomic<int> m_volume{100};
};
}  // namespace AudioCommon
