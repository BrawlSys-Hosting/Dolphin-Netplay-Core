// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/LibretroSoundStream.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#include "Common/Logging/Log.h"
#include "Common/Thread.h"

namespace AudioCommon
{
namespace
{
std::atomic<LibretroAudioSampleBatch> s_audio_batch{nullptr};
constexpr std::size_t BUFFER_FRAMES = 512;
constexpr std::size_t CHANNELS = 2;

int16_t ApplyVolume(int16_t sample, int volume)
{
  const int scaled = static_cast<int>(sample) * volume / 100;
  return static_cast<int16_t>(std::clamp(scaled, -32768, 32767));
}
}  // namespace

void SetLibretroAudioSampleBatch(LibretroAudioSampleBatch cb)
{
  s_audio_batch.store(cb);
}

LibretroAudioSampleBatch GetLibretroAudioSampleBatch()
{
  return s_audio_batch.load();
}

LibretroSoundStream::LibretroSoundStream() = default;

LibretroSoundStream::~LibretroSoundStream()
{
  m_run_thread.Clear();
  if (m_thread.joinable())
    m_thread.join();
}

bool LibretroSoundStream::IsValid()
{
  return GetLibretroAudioSampleBatch() != nullptr;
}

bool LibretroSoundStream::Init()
{
  if (!IsValid())
  {
    WARN_LOG_FMT(AUDIO, "Libretro audio callback not set.");
    return false;
  }

  m_run_thread.Set();
  m_thread = std::thread(&LibretroSoundStream::SoundLoop, this);
  return true;
}

bool LibretroSoundStream::SetRunning(bool running)
{
  m_running.store(running);
  return true;
}

void LibretroSoundStream::SetVolume(int volume)
{
  m_volume.store(volume);
}

void LibretroSoundStream::SoundLoop()
{
  Common::SetCurrentThreadName("Audio thread - libretro");

  std::vector<int16_t> buffer(BUFFER_FRAMES * CHANNELS);
  const uint32_t sample_rate = m_mixer->GetSampleRate();
  const double buffer_seconds =
      sample_rate ? static_cast<double>(BUFFER_FRAMES) / sample_rate : 0.0;
  auto next_wake = std::chrono::steady_clock::now();

  while (m_run_thread.IsSet())
  {
    if (!m_running.load())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    auto cb = GetLibretroAudioSampleBatch();
    if (!cb)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    const std::size_t frames = m_mixer->Mix(buffer.data(), BUFFER_FRAMES);
    if (frames == 0)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    const int volume = m_volume.load();
    if (volume != 100)
    {
      const std::size_t samples = frames * CHANNELS;
      for (std::size_t i = 0; i < samples; ++i)
        buffer[i] = ApplyVolume(buffer[i], volume);
    }

    cb(buffer.data(), frames);

    if (buffer_seconds > 0.0)
    {
      next_wake += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(buffer_seconds));
      const auto now = std::chrono::steady_clock::now();
      if (next_wake > now)
        std::this_thread::sleep_until(next_wake);
      else
        next_wake = now;
    }
  }
}
}  // namespace AudioCommon
