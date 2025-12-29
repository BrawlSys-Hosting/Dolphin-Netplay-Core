// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <libretro.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(HAVE_X11)
#include <GL/glx.h>
#include <X11/Xlib.h>
#endif

#include "Common/Buffer.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/GL/GLInterface/Libretro.h"
#include "Common/HookableEvent.h"
#include "Common/IniFile.h"
#include "Common/Logging/LogManager.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "AudioCommon/LibretroSoundStream.h"
#include "Core/ARDecrypt.h"
#include "Core/ActionReplay.h"
#include "Core/Boot/Boot.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/BootManager.h"
#include "Core/ConfigManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/HW/Wiimote.h"
#include "Core/Host.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/TitleDatabase.h"
#include "UICommon/GameFile.h"
#include "UICommon/NetPlayIndex.h"
#include "UICommon/UICommon.h"
#include "VideoBackends/OGL/VideoBackend.h"
#include "VideoCommon/VideoBackendBase.h"
#include "InputCommon/LibretroInput.h"

namespace
{
retro_environment_t s_environment = nullptr;
retro_video_refresh_t s_video_refresh = nullptr;
retro_audio_sample_t s_audio_sample = nullptr;
retro_audio_sample_batch_t s_audio_sample_batch = nullptr;
retro_input_poll_t s_input_poll = nullptr;
retro_input_state_t s_input_state = nullptr;
retro_log_printf_t s_log = nullptr;

WindowSystemInfo s_wsi{};
std::optional<Common::EventHook> s_state_hook;

std::atomic<bool> s_stop_requested{false};
bool s_initialized = false;
bool s_game_loaded = false;
bool s_hw_render_enabled = false;
std::atomic<bool> s_hw_context_ready{false};
retro_hw_render_callback s_hw_callback{};

std::vector<retro_variable> s_core_options;
std::vector<std::string> s_core_option_strings;

Common::UniqueBuffer<u8> s_state_buffer;

std::atomic<bool> s_pending_present{false};
std::atomic<unsigned> s_present_width{0};
std::atomic<unsigned> s_present_height{0};
bool s_log_listener_registered = false;

enum class CheatBackend
{
  ActionReplay,
  Gecko,
};

struct LibretroCheat
{
  bool enabled = false;
  bool valid = false;
  CheatBackend backend = CheatBackend::ActionReplay;
  ActionReplay::ARCode ar_code{};
  Gecko::GeckoCode gecko_code{};
};

std::vector<LibretroCheat> s_cheats;

enum class NetPlayMode
{
  Disabled,
  Host,
  Join,
};

enum class NetPlayConnection
{
  Direct,
  Traversal,
  Lobby,
};

struct NetPlayOptionCache
{
  std::string mode;
  std::string connection;
  std::string room;
  std::string refresh_rooms;
  std::string start_game;
};

struct PendingBoot
{
  std::string path;
  std::optional<BootSessionData> session;
  bool is_netplay = false;
};

class LibretroNetPlayUI;

std::unique_ptr<NetPlay::NetPlayClient> s_netplay_client;
std::unique_ptr<NetPlay::NetPlayServer> s_netplay_server;
std::unique_ptr<LibretroNetPlayUI> s_netplay_ui;

std::shared_ptr<const UICommon::GameFile> s_loaded_game_file;
std::string s_loaded_game_path;

std::vector<NetPlaySession> s_netplay_rooms;
std::unordered_map<std::string, size_t> s_netplay_room_value_map;
std::mutex s_netplay_mutex;

NetPlay::SyncIdentifier s_netplay_selected_game{};
std::string s_netplay_selected_game_name;

std::atomic<bool> s_netplay_start_requested{false};

NetPlayOptionCache s_netplay_option_cache;
std::optional<PendingBoot> s_pending_boot;

constexpr unsigned kDummyWidth = 1;
constexpr unsigned kDummyHeight = 1;
std::array<uint32_t, kDummyWidth * kDummyHeight> s_dummy_frame{};

const char* GetCoreOptionValue(const char* key);
bool ApplyCheats();
bool BootGameInternal(std::string path, std::optional<BootSessionData> session, bool is_netplay);
void DeferBoot(std::string path, std::optional<BootSessionData> session, bool is_netplay);

void LogMessage(retro_log_level level, const char* fmt, ...)
{
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  if (s_log)
    s_log(level, "%s", buffer);
  else
    std::fprintf(stderr, "%s", buffer);
}

class LibretroLogListener final : public Common::Log::LogListener
{
public:
  void Log(Common::Log::LogLevel level, const char* msg) override
  {
    if (!s_log || !msg)
      return;

    retro_log_level retro_level = RETRO_LOG_INFO;
    switch (level)
    {
    case Common::Log::LogLevel::LERROR:
      retro_level = RETRO_LOG_ERROR;
      break;
    case Common::Log::LogLevel::LWARNING:
      retro_level = RETRO_LOG_WARN;
      break;
    case Common::Log::LogLevel::LNOTICE:
      retro_level = RETRO_LOG_INFO;
      break;
    case Common::Log::LogLevel::LINFO:
      retro_level = RETRO_LOG_INFO;
      break;
    case Common::Log::LogLevel::LDEBUG:
      retro_level = RETRO_LOG_DEBUG;
      break;
    }

    s_log(retro_level, "[dolphin] %s", msg);
  }
};

void SetupLibretroLogging()
{
  auto* log_manager = Common::Log::LogManager::GetInstance();
  if (!log_manager)
    return;

  if (!s_log_listener_registered)
  {
    log_manager->RegisterListener(Common::Log::LogListener::LOG_WINDOW_LISTENER,
                                  std::make_unique<LibretroLogListener>());
    s_log_listener_registered = true;
  }

  log_manager->EnableListener(Common::Log::LogListener::LOG_WINDOW_LISTENER, true);
  log_manager->SetConfigLogLevel(Common::Log::LogLevel::LINFO);
  log_manager->SetEnable(Common::Log::LogType::BOOT, true);
  log_manager->SetEnable(Common::Log::LogType::CORE, true);
  log_manager->SetEnable(Common::Log::LogType::VIDEO, true);
  log_manager->SetEnable(Common::Log::LogType::HOST_GPU, true);
  log_manager->SetEnable(Common::Log::LogType::COMMON, true);
}

size_t AudioSampleBatchShim(const int16_t* data, size_t frames)
{
  if (!s_audio_sample)
    return 0;

  for (size_t i = 0; i < frames; ++i)
  {
    const int16_t left = data[i * 2];
    const int16_t right = data[i * 2 + 1];
    s_audio_sample(left, right);
  }

  return frames;
}

void UpdateLibretroAudioCallback()
{
  if (s_audio_sample_batch)
  {
    AudioCommon::SetLibretroAudioSampleBatch(
        reinterpret_cast<AudioCommon::LibretroAudioSampleBatch>(s_audio_sample_batch));
    return;
  }

  if (s_audio_sample)
  {
    AudioCommon::SetLibretroAudioSampleBatch(AudioSampleBatchShim);
    return;
  }

  AudioCommon::SetLibretroAudioSampleBatch(nullptr);
}

void ForceLibretroVideoConfig()
{
  Config::SetBaseOrCurrent(Config::GFX_BACKEND_MULTITHREADING, false);
  Config::SetBaseOrCurrent(Config::GFX_SHADER_COMPILER_THREADS, 0);
  Config::SetBaseOrCurrent(Config::GFX_SHADER_PRECOMPILER_THREADS, 0);
  Config::SetBaseOrCurrent(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, false);
  Config::SetBaseOrCurrent(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::Synchronous);
}

std::string SanitizeCoreOptionValue(std::string value)
{
  value = ReplaceAll(std::move(value), "|", "/");
  value = ReplaceAll(std::move(value), ";", "/");
  value = ReplaceAll(std::move(value), "\n", " ");
  value = ReplaceAll(std::move(value), "\r", " ");
  return value;
}

std::string GetRetroUsername()
{
  if (!s_environment)
    return {};

  const char* username = nullptr;
  if (!s_environment(RETRO_ENVIRONMENT_GET_USERNAME, &username) || !username)
    return {};

  return username;
}

std::string GetNetPlayNickname()
{
  std::string username = GetRetroUsername();
  if (!username.empty())
    return username;

  return Config::Get(Config::NETPLAY_NICKNAME);
}

std::string GetNetPlayRoomName()
{
  const std::string configured = Config::Get(Config::NETPLAY_INDEX_NAME);
  if (!configured.empty())
    return configured;

  if (!s_loaded_game_file)
    return "NetPlay Session";

  Core::TitleDatabase title_database;
  return s_loaded_game_file->GetNetPlayName(title_database);
}

NetPlayMode GetNetPlayMode()
{
  const char* value = GetCoreOptionValue("dolphin_netplay_mode");
  if (!value)
    return NetPlayMode::Disabled;

  if (std::string_view(value) == "host")
    return NetPlayMode::Host;
  if (std::string_view(value) == "join")
    return NetPlayMode::Join;

  return NetPlayMode::Disabled;
}

NetPlayConnection GetNetPlayConnection()
{
  const char* value = GetCoreOptionValue("dolphin_netplay_connection");
  if (!value)
    return NetPlayConnection::Direct;

  if (std::string_view(value) == "traversal")
    return NetPlayConnection::Traversal;
  if (std::string_view(value) == "lobby")
    return NetPlayConnection::Lobby;

  return NetPlayConnection::Direct;
}

std::vector<std::string> BuildNetPlayRoomValues()
{
  std::vector<std::string> values;
  values.emplace_back("manual");

  s_netplay_room_value_map.clear();
  constexpr size_t kMaxRooms = 24;

  std::lock_guard lk(s_netplay_mutex);
  for (size_t i = 0; i < s_netplay_rooms.size() && i < kMaxRooms; ++i)
  {
    const NetPlaySession& session = s_netplay_rooms[i];
    std::string label = std::to_string(i + 1) + ": " + session.name;
    if (!session.game_id.empty())
      label += " (" + session.game_id + ")";
    label += " [" + std::to_string(session.player_count) + "]";
    if (session.has_password)
      label += " P";
    if (session.in_game)
      label += " InGame";

    label = SanitizeCoreOptionValue(std::move(label));
    s_netplay_room_value_map.emplace(label, i);
    values.push_back(std::move(label));
  }

  return values;
}

void AddUniqueValue(std::vector<std::string>& values, std::string value)
{
  if (value.empty())
    return;
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(std::move(value));
}

std::vector<std::string> BuildNetPlayAddressValues()
{
  std::vector<std::string> values;
  AddUniqueValue(values,
                 SanitizeCoreOptionValue(Config::Get(Config::NETPLAY_ADDRESS)));
  AddUniqueValue(values, "127.0.0.1");
  AddUniqueValue(values, "localhost");
  AddUniqueValue(values, "192.168.0.1");
  AddUniqueValue(values, "192.168.1.1");
  AddUniqueValue(values, "10.0.0.1");
  return values;
}

std::vector<std::string> BuildNetPlayHostCodeValues()
{
  std::vector<std::string> values;
  AddUniqueValue(values,
                 SanitizeCoreOptionValue(Config::Get(Config::NETPLAY_HOST_CODE)));
  AddUniqueValue(values, "00000000");
  return values;
}

std::vector<std::string> BuildNetPlayTraversalServerValues()
{
  std::vector<std::string> values;
  AddUniqueValue(values,
                 SanitizeCoreOptionValue(Config::Get(Config::NETPLAY_TRAVERSAL_SERVER)));
  AddUniqueValue(values, "stun.dolphin-emu.org");
  return values;
}

std::vector<std::string> BuildNetPlayPortValues(u16 configured)
{
  std::vector<std::string> values;
  auto add_port = [&values](u16 port) {
    if (port == 0)
      return;
    AddUniqueValue(values, std::to_string(port));
  };

  add_port(configured);
  add_port(2626);

  for (int delta = -2; delta <= 2; ++delta)
  {
    const int port = static_cast<int>(configured) + delta;
    if (port > 0 && port <= 65535)
      add_port(static_cast<u16>(port));
  }

  return values;
}

class LibretroNetPlayUI final : public NetPlay::NetPlayUI
{
public:
  void BootGame(const std::string& filename,
                std::unique_ptr<BootSessionData> boot_session_data) override
  {
    if (s_game_loaded)
      return;

    BootSessionData session = std::move(*boot_session_data);
    if (!s_hw_context_ready.load())
    {
      DeferBoot(filename, std::move(session), true);
      return;
    }

    BootGameInternal(filename, std::move(session), true);
  }

  void StopGame() override { s_stop_requested.store(true); }

  bool IsHosting() const override { return s_netplay_server != nullptr; }

  void Update() override {}

  void AppendChat(const std::string& msg) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay chat: %s\n", msg.c_str());
  }

  void OnMsgChangeGame(const NetPlay::SyncIdentifier& sync_identifier,
                       const std::string& netplay_name) override
  {
    {
      std::lock_guard lk(s_netplay_mutex);
      s_netplay_selected_game = sync_identifier;
      s_netplay_selected_game_name = netplay_name;
    }
    LogMessage(RETRO_LOG_INFO, "NetPlay game changed: %s\n", netplay_name.c_str());
  }

  void OnMsgChangeGBARom(int pad, const NetPlay::GBAConfig& config) override
  {
    if (config.has_rom)
    {
      LogMessage(RETRO_LOG_INFO, "NetPlay GBA%u ROM: %s\n", pad + 1, config.title.c_str());
    }
    else
    {
      LogMessage(RETRO_LOG_INFO, "NetPlay GBA%u ROM disabled\n", pad + 1);
    }
  }

  void OnMsgStartGame() override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay starting game\n");
    s_netplay_start_requested.store(true);
  }

  void OnMsgStopGame() override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay stopped game\n");
    s_stop_requested.store(true);
  }

  void OnMsgPowerButton() override
  {
    if (Core::IsRunning(Core::System::GetInstance()))
      UICommon::TriggerSTMPowerEvent();
  }

  void OnPlayerConnect(const std::string& player) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay player joined: %s\n", player.c_str());
  }

  void OnPlayerDisconnect(const std::string& player) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay player left: %s\n", player.c_str());
  }

  void OnPadBufferChanged(u32 buffer) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay buffer size: %u\n", buffer);
  }

  void OnHostInputAuthorityChanged(bool enabled) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay host input authority: %s\n",
               enabled ? "enabled" : "disabled");
  }

  void OnDesync(u32 frame, const std::string& player) override
  {
    LogMessage(RETRO_LOG_WARN, "NetPlay desync at frame %u (%s)\n", frame, player.c_str());
  }

  void OnConnectionLost() override { LogMessage(RETRO_LOG_WARN, "NetPlay connection lost\n"); }

  void OnConnectionError(const std::string& message) override
  {
    LogMessage(RETRO_LOG_ERROR, "NetPlay connection error: %s\n", message.c_str());
  }

  void OnTraversalError(Common::TraversalClient::FailureReason) override
  {
    LogMessage(RETRO_LOG_ERROR, "NetPlay traversal error\n");
  }

  void OnTraversalStateChanged(Common::TraversalClient::State) override {}

  void OnGameStartAborted() override
  {
    LogMessage(RETRO_LOG_WARN, "NetPlay start aborted\n");
  }

  void OnGolferChanged(bool is_golfer, const std::string& golfer_name) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay golfer: %s (%s)\n", golfer_name.c_str(),
               is_golfer ? "local" : "remote");
  }

  void OnTtlDetermined(u8 ttl) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay TTL determined: %u\n", ttl);
  }

  bool IsRecording() override { return Config::Get(Config::NETPLAY_RECORD_INPUTS); }

  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier& sync_identifier,
               NetPlay::SyncIdentifierComparison* found = nullptr) override
  {
    NetPlay::SyncIdentifierComparison temp;
    if (!found)
      found = &temp;

    *found = NetPlay::SyncIdentifierComparison::DifferentGame;

    if (!s_loaded_game_file)
      return nullptr;

    *found = s_loaded_game_file->CompareSyncIdentifier(sync_identifier);
    if (*found == NetPlay::SyncIdentifierComparison::SameGame)
      return s_loaded_game_file;

    return nullptr;
  }

  std::string FindGBARomPath(const std::array<u8, 20>&, std::string_view, int device_number) override
  {
    if (device_number < 0 || device_number >= 4)
      return {};

#ifdef HAS_LIBMGBA
    const std::string path = Config::Get(Config::MAIN_GBA_ROM_PATHS[device_number]);
    if (path.empty() || !File::Exists(path))
      return {};

    return path;
#else
    return {};
#endif
  }

  void ShowGameDigestDialog(const std::string& title) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay digest: %s\n", title.c_str());
  }

  void SetGameDigestProgress(int pid, int progress) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay digest progress (pid %d): %d%%\n", pid, progress);
  }

  void SetGameDigestResult(int pid, const std::string& result) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay digest result (pid %d): %s\n", pid, result.c_str());
  }

  void AbortGameDigest() override {}

  void OnIndexAdded(bool success, std::string error) override
  {
    LogMessage(success ? RETRO_LOG_INFO : RETRO_LOG_WARN,
               "NetPlay index add: %s%s\n", success ? "ok" : "failed",
               success ? "" : (" (" + error + ")").c_str());
  }

  void OnIndexRefreshFailed(std::string error) override
  {
    LogMessage(RETRO_LOG_WARN, "NetPlay index refresh failed: %s\n", error.c_str());
  }

  void ShowChunkedProgressDialog(const std::string& title, u64 data_size,
                                 const std::vector<int>& players) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay chunked data: %s (%llu bytes, %zu players)\n",
               title.c_str(), static_cast<unsigned long long>(data_size), players.size());
  }

  void HideChunkedProgressDialog() override {}

  void SetChunkedProgress(int pid, u64 progress) override
  {
    LogMessage(RETRO_LOG_INFO, "NetPlay chunked progress (pid %d): %llu\n", pid,
               static_cast<unsigned long long>(progress));
  }

  void SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder) override
  {
    if (s_netplay_client)
      s_netplay_client->SetWiiSyncData(nullptr, std::move(titles), std::move(redirect_folder));
  }
};

bool LibretroMsgAlertHandler(const char* caption, const char* text, bool yes_no,
                             Common::MsgType style)
{
  const char* severity = "info";
  retro_log_level level = RETRO_LOG_INFO;
  switch (style)
  {
  case Common::MsgType::Information:
    break;
  case Common::MsgType::Question:
    severity = "question";
    level = RETRO_LOG_WARN;
    break;
  case Common::MsgType::Warning:
    severity = "warning";
    level = RETRO_LOG_WARN;
    break;
  case Common::MsgType::Critical:
    severity = "error";
    level = RETRO_LOG_ERROR;
    break;
  }

  LogMessage(level, "[%s] %s: %s\n", severity, caption, text);
  return !yes_no;
}

void SetUserDirectoryFromEnvironment()
{
  const char* base_dir = nullptr;
  if (s_environment)
  {
    if (!s_environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &base_dir) || !base_dir)
      s_environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &base_dir);
  }

  std::string user_dir = base_dir ? base_dir : ".";
  if (!user_dir.empty() && user_dir.back() == DIR_SEP_CHR)
    user_dir.pop_back();
  user_dir += std::string(1, DIR_SEP_CHR) + "Dolphin";

  UICommon::SetUserDirectory(user_dir);
  UICommon::CreateDirectories();
}

void SetSystemDirectoryFromEnvironment()
{
  const char* system_dir = nullptr;
  if (s_environment)
    s_environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);

  std::string base_dir;
  if (system_dir && system_dir[0])
  {
    base_dir = system_dir;
  }
  else if (s_environment)
  {
    const char* libretro_path = nullptr;
    if (s_environment(RETRO_ENVIRONMENT_GET_LIBRETRO_PATH, &libretro_path) && libretro_path &&
        libretro_path[0])
    {
      std::filesystem::path core_path = libretro_path;
      std::filesystem::path core_dir = core_path.parent_path();
      if (core_dir.filename() == "cores")
        base_dir = core_dir.parent_path().string();
      else
        base_dir = core_dir.string();
      base_dir += std::string(1, DIR_SEP_CHR) + "system";
    }
  }

  if (base_dir.empty())
    return;

  if (!base_dir.empty() && base_dir.back() == DIR_SEP_CHR)
    base_dir.pop_back();
  base_dir += std::string(1, DIR_SEP_CHR) + "Dolphin" + DIR_SEP_CHR + "Sys";
  LogMessage(RETRO_LOG_INFO, "Using Sys directory: %s\n", base_dir.c_str());
  File::SetSysDirectory(std::move(base_dir));
}

void StopCore()
{
  auto& system = Core::System::GetInstance();
  if (!Core::IsUninitialized(system))
    Core::Stop(system);
  Core::Shutdown(system);
}

void PresentFrame(unsigned width, unsigned height)
{
  s_present_width.store(width);
  s_present_height.store(height);
  s_pending_present.store(true);
}

void UpdateLibretroGLCallbacks()
{
  LibretroGLCallbacks callbacks{};
  callbacks.get_proc_address =
      reinterpret_cast<LibretroGLCallbacks::GetProcAddress>(s_hw_callback.get_proc_address);
  callbacks.get_current_framebuffer =
      reinterpret_cast<LibretroGLCallbacks::GetFramebuffer>(s_hw_callback.get_current_framebuffer);
  callbacks.present = PresentFrame;
  callbacks.base_width = 640;
  callbacks.base_height = 528;
  callbacks.is_gles = (s_hw_callback.context_type == RETRO_HW_CONTEXT_OPENGLES2 ||
                       s_hw_callback.context_type == RETRO_HW_CONTEXT_OPENGLES3);
#if defined(_WIN32)
  callbacks.native_display = wglGetCurrentDC();
  callbacks.native_context = wglGetCurrentContext();
#elif defined(HAVE_X11)
  callbacks.native_display = glXGetCurrentDisplay();
  callbacks.native_context = glXGetCurrentContext();
  callbacks.native_drawable = glXGetCurrentDrawable();
#endif
  LibretroSetGLCallbacks(callbacks);
}

void OnHWContextReset()
{
  s_hw_context_ready.store(true);
  UpdateLibretroGLCallbacks();
}

void OnHWContextDestroy()
{
  s_hw_context_ready.store(false);
  LibretroSetGLCallbacks(LibretroGLCallbacks{});
}

bool SetupHardwareRendering()
{
  if (!s_environment)
    return false;

  s_hw_context_ready.store(false);
  bool shared_context = true;
  s_environment(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, &shared_context);

  s_hw_callback = {};
  s_hw_callback.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
  s_hw_callback.context_reset = OnHWContextReset;
  s_hw_callback.context_destroy = OnHWContextDestroy;
  s_hw_callback.version_major = 3;
  s_hw_callback.version_minor = 3;
  s_hw_callback.cache_context = false;
  s_hw_callback.debug_context = false;
  s_hw_callback.bottom_left_origin = true;

  if (!s_environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &s_hw_callback))
    return false;

  return true;
}

void SubmitDummyFrame()
{
  if (!s_video_refresh)
    return;
  s_video_refresh(s_dummy_frame.data(), kDummyWidth, kDummyHeight,
                  kDummyWidth * sizeof(uint32_t));
}

bool BootGameInternal(std::string path, std::optional<BootSessionData> session, bool is_netplay)
{
  auto boot = session ? BootParameters::GenerateFromFile(std::move(path), std::move(*session)) :
                        BootParameters::GenerateFromFile(std::move(path));
  if (!boot)
  {
    LogMessage(RETRO_LOG_ERROR, "%s boot failed: invalid boot parameters\n",
               is_netplay ? "NetPlay" : "Game");
    return false;
  }

  auto& system = Core::System::GetInstance();
  if (!s_state_hook)
  {
    s_state_hook = Core::AddOnStateChangedCallback([](Core::State state) {
      if (state == Core::State::Uninitialized)
        s_game_loaded = false;
    });
  }

  DolphinAnalytics::Instance().ReportDolphinStart("libretro");
  if (!BootManager::BootCore(system, std::move(boot), s_wsi))
  {
    LogMessage(RETRO_LOG_ERROR, "%s failed to boot\n", is_netplay ? "NetPlay" : "Game");
    return false;
  }

  s_game_loaded = true;
  ApplyCheats();
  return true;
}

void DeferBoot(std::string path, std::optional<BootSessionData> session, bool is_netplay)
{
  PendingBoot pending;
  pending.path = std::move(path);
  pending.session = std::move(session);
  pending.is_netplay = is_netplay;
  s_pending_boot = std::move(pending);
}

std::string GetEnabledDisabled(bool enabled)
{
  return enabled ? "enabled" : "disabled";
}

std::string GetWiimoteSourceString(WiimoteSource source)
{
  switch (source)
  {
  case WiimoteSource::Emulated:
    return "emulated";
  case WiimoteSource::Real:
    return "real";
  case WiimoteSource::None:
  default:
    return "none";
  }
}

std::string GetInternalResolutionDefault()
{
  static constexpr std::array<const char*, 9> values = {
      "1x", "2x", "3x", "4x", "5x", "6x", "8x", "10x", "12x"};

  const int scale = Config::Get(Config::GFX_EFB_SCALE);
  const std::string candidate = std::to_string(scale) + "x";

  if (std::find(std::begin(values), std::end(values), candidate) != std::end(values))
    return candidate;

  return "1x";
}

std::string GetOptionDefault(const char* key, const std::string& fallback, bool use_current_values)
{
  if (!use_current_values)
    return fallback;

  const char* value = GetCoreOptionValue(key);
  if (value && *value)
    return value;

  return fallback;
}

void AddCoreOption(const char* key, const char* description,
                   std::vector<std::string> values, const std::string& default_value)
{
  auto it = std::find(values.begin(), values.end(), default_value);
  if (it != values.end())
    std::rotate(values.begin(), it, std::next(it));
  else
    values.insert(values.begin(), default_value);

  std::string option_string = description;
  option_string += "; ";
  for (size_t i = 0; i < values.size(); ++i)
  {
    if (i != 0)
      option_string += "|";
    option_string += values[i];
  }

  s_core_option_strings.push_back(std::move(option_string));
  s_core_options.push_back({key, s_core_option_strings.back().c_str()});
}

void BuildCoreOptions(bool use_current_values = false)
{
  if (!s_environment)
    return;

  s_core_options.clear();
  s_core_option_strings.clear();

  constexpr size_t option_count = 41;
  s_core_options.reserve(option_count + 1);
  s_core_option_strings.reserve(option_count);

  AddCoreOption("dolphin_internal_resolution", "Internal resolution",
                {"1x", "2x", "3x", "4x", "5x", "6x", "8x", "10x", "12x"},
                GetOptionDefault("dolphin_internal_resolution", GetInternalResolutionDefault(),
                                 use_current_values));
  AddCoreOption("dolphin_widescreen_hack", "Widescreen hack", {"disabled", "enabled"},
                GetOptionDefault("dolphin_widescreen_hack",
                                 GetEnabledDisabled(Config::Get(Config::GFX_WIDESCREEN_HACK)),
                                 use_current_values));
  AddCoreOption("dolphin_vsync", "VSync", {"disabled", "enabled"},
                GetOptionDefault("dolphin_vsync",
                                 GetEnabledDisabled(Config::Get(Config::GFX_VSYNC)),
                                 use_current_values));
  AddCoreOption("dolphin_dual_core", "Dual core (CPU thread)", {"disabled", "enabled"},
                GetOptionDefault("dolphin_dual_core",
                                 GetEnabledDisabled(Config::Get(Config::MAIN_CPU_THREAD)),
                                 use_current_values));
  AddCoreOption("dolphin_dsp_hle", "DSP HLE", {"enabled", "disabled"},
                GetOptionDefault("dolphin_dsp_hle",
                                 GetEnabledDisabled(Config::Get(Config::MAIN_DSP_HLE)),
                                 use_current_values));
  AddCoreOption("dolphin_sync_on_skip_idle", "Sync on skip idle", {"enabled", "disabled"},
                GetOptionDefault(
                    "dolphin_sync_on_skip_idle",
                    GetEnabledDisabled(Config::Get(Config::MAIN_SYNC_ON_SKIP_IDLE)),
                    use_current_values));
  AddCoreOption("dolphin_cheats", "Enable cheats", {"disabled", "enabled"},
                GetOptionDefault("dolphin_cheats",
                                 GetEnabledDisabled(Config::Get(Config::MAIN_ENABLE_CHEATS)),
                                 use_current_values));
  AddCoreOption("dolphin_savestates", "Enable savestates", {"disabled", "enabled"},
                GetOptionDefault("dolphin_savestates",
                                 GetEnabledDisabled(Config::Get(Config::MAIN_ENABLE_SAVESTATES)),
                                 use_current_values));
  AddCoreOption("dolphin_wiimote_speaker", "Wiimote speaker", {"disabled", "enabled"},
                GetOptionDefault(
                    "dolphin_wiimote_speaker",
                    GetEnabledDisabled(Config::Get(Config::MAIN_WIIMOTE_ENABLE_SPEAKER)),
                    use_current_values));
  AddCoreOption("dolphin_wiimote_1", "Wiimote 1 source", {"emulated", "real", "none"},
                GetOptionDefault(
                    "dolphin_wiimote_1",
                    GetWiimoteSourceString(Config::Get(Config::GetInfoForWiimoteSource(0))),
                    use_current_values));
  AddCoreOption("dolphin_wiimote_2", "Wiimote 2 source", {"emulated", "real", "none"},
                GetOptionDefault(
                    "dolphin_wiimote_2",
                    GetWiimoteSourceString(Config::Get(Config::GetInfoForWiimoteSource(1))),
                    use_current_values));
  AddCoreOption("dolphin_wiimote_3", "Wiimote 3 source", {"emulated", "real", "none"},
                GetOptionDefault(
                    "dolphin_wiimote_3",
                    GetWiimoteSourceString(Config::Get(Config::GetInfoForWiimoteSource(2))),
                    use_current_values));
  AddCoreOption("dolphin_wiimote_4", "Wiimote 4 source", {"emulated", "real", "none"},
                GetOptionDefault(
                    "dolphin_wiimote_4",
                    GetWiimoteSourceString(Config::Get(Config::GetInfoForWiimoteSource(3))),
                    use_current_values));

  AddCoreOption("dolphin_netplay_mode", "NetPlay mode", {"disabled", "host", "join"},
                GetOptionDefault("dolphin_netplay_mode", "disabled", use_current_values));
  AddCoreOption("dolphin_netplay_connection", "NetPlay connection",
                {"direct", "traversal", "lobby"},
                GetOptionDefault("dolphin_netplay_connection", "direct", use_current_values));
  AddCoreOption("dolphin_netplay_address", "NetPlay address (direct join)",
                BuildNetPlayAddressValues(),
                GetOptionDefault("dolphin_netplay_address",
                                 Config::Get(Config::NETPLAY_ADDRESS),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_connect_port", "NetPlay connect port",
                BuildNetPlayPortValues(Config::Get(Config::NETPLAY_CONNECT_PORT)),
                GetOptionDefault("dolphin_netplay_connect_port",
                                 std::to_string(Config::Get(Config::NETPLAY_CONNECT_PORT)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_host_port", "NetPlay host port",
                BuildNetPlayPortValues(Config::Get(Config::NETPLAY_HOST_PORT)),
                GetOptionDefault("dolphin_netplay_host_port",
                                 std::to_string(Config::Get(Config::NETPLAY_HOST_PORT)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_listen_port", "NetPlay traversal listen port",
                BuildNetPlayPortValues(Config::Get(Config::NETPLAY_LISTEN_PORT)),
                GetOptionDefault("dolphin_netplay_listen_port",
                                 std::to_string(Config::Get(Config::NETPLAY_LISTEN_PORT)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_host_code", "NetPlay host code (traversal join)",
                BuildNetPlayHostCodeValues(),
                GetOptionDefault("dolphin_netplay_host_code",
                                 Config::Get(Config::NETPLAY_HOST_CODE),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_traversal_server", "NetPlay traversal server",
                BuildNetPlayTraversalServerValues(),
                GetOptionDefault("dolphin_netplay_traversal_server",
                                 Config::Get(Config::NETPLAY_TRAVERSAL_SERVER),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_traversal_port", "NetPlay traversal port",
                BuildNetPlayPortValues(Config::Get(Config::NETPLAY_TRAVERSAL_PORT)),
                GetOptionDefault("dolphin_netplay_traversal_port",
                                 std::to_string(Config::Get(Config::NETPLAY_TRAVERSAL_PORT)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_traversal_port_alt", "NetPlay traversal port alt",
                BuildNetPlayPortValues(Config::Get(Config::NETPLAY_TRAVERSAL_PORT_ALT)),
                GetOptionDefault("dolphin_netplay_traversal_port_alt",
                                 std::to_string(Config::Get(Config::NETPLAY_TRAVERSAL_PORT_ALT)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_lobby_refresh", "NetPlay lobby refresh", {"no", "yes"},
                GetOptionDefault("dolphin_netplay_lobby_refresh", "no", use_current_values));
  AddCoreOption("dolphin_netplay_lobby_room", "NetPlay lobby room",
                BuildNetPlayRoomValues(),
                GetOptionDefault("dolphin_netplay_lobby_room", "manual", use_current_values));
  AddCoreOption("dolphin_netplay_lobby_advertise", "NetPlay lobby advertise",
                {"disabled", "enabled"},
                GetOptionDefault("dolphin_netplay_lobby_advertise",
                                 GetEnabledDisabled(Config::Get(Config::NETPLAY_USE_INDEX)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_lobby_region", "NetPlay lobby region",
                {"EA", "CN", "EU", "NA", "SA", "OC", "AF"},
                GetOptionDefault("dolphin_netplay_lobby_region",
                                 Config::Get(Config::NETPLAY_INDEX_REGION).empty() ?
                                     "NA" :
                                     Config::Get(Config::NETPLAY_INDEX_REGION),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_start", "NetPlay start game", {"no", "yes"},
                GetOptionDefault("dolphin_netplay_start", "no", use_current_values));
  AddCoreOption("dolphin_netplay_network_mode", "NetPlay network mode",
                {"fixeddelay", "hostinputauthority", "golf"},
                GetOptionDefault("dolphin_netplay_network_mode",
                                 Config::Get(Config::NETPLAY_NETWORK_MODE),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_buffer_size", "NetPlay buffer size",
                {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "12", "15"},
                GetOptionDefault("dolphin_netplay_buffer_size",
                                 std::to_string(Config::Get(Config::NETPLAY_BUFFER_SIZE)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_client_buffer_size", "NetPlay client buffer size",
                {"1", "2", "3", "4", "5"},
                GetOptionDefault("dolphin_netplay_client_buffer_size",
                                 std::to_string(Config::Get(Config::NETPLAY_CLIENT_BUFFER_SIZE)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_savedata_load", "NetPlay load save data",
                {"enabled", "disabled"},
                GetOptionDefault("dolphin_netplay_savedata_load",
                                 GetEnabledDisabled(Config::Get(Config::NETPLAY_SAVEDATA_LOAD)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_savedata_write", "NetPlay write save data",
                {"enabled", "disabled"},
                GetOptionDefault("dolphin_netplay_savedata_write",
                                 GetEnabledDisabled(Config::Get(Config::NETPLAY_SAVEDATA_WRITE)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_savedata_sync_all_wii", "NetPlay sync all Wii saves",
                {"disabled", "enabled"},
                GetOptionDefault(
                    "dolphin_netplay_savedata_sync_all_wii",
                    GetEnabledDisabled(Config::Get(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII)),
                    use_current_values));
  AddCoreOption("dolphin_netplay_sync_codes", "NetPlay sync cheats", {"enabled", "disabled"},
                GetOptionDefault("dolphin_netplay_sync_codes",
                                 GetEnabledDisabled(Config::Get(Config::NETPLAY_SYNC_CODES)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_strict_settings_sync", "NetPlay strict settings sync",
                {"disabled", "enabled"},
                GetOptionDefault("dolphin_netplay_strict_settings_sync",
                                 GetEnabledDisabled(Config::Get(Config::NETPLAY_STRICT_SETTINGS_SYNC)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_record_inputs", "NetPlay record inputs", {"disabled", "enabled"},
                GetOptionDefault("dolphin_netplay_record_inputs",
                                 GetEnabledDisabled(Config::Get(Config::NETPLAY_RECORD_INPUTS)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_golf_overlay", "NetPlay golf overlay", {"enabled", "disabled"},
                GetOptionDefault("dolphin_netplay_golf_overlay",
                                 GetEnabledDisabled(Config::Get(Config::NETPLAY_GOLF_MODE_OVERLAY)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_hide_remote_gbas", "NetPlay hide remote GBAs",
                {"disabled", "enabled"},
                GetOptionDefault("dolphin_netplay_hide_remote_gbas",
                                 GetEnabledDisabled(Config::Get(Config::NETPLAY_HIDE_REMOTE_GBAS)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_use_upnp", "NetPlay use UPNP", {"disabled", "enabled"},
                GetOptionDefault("dolphin_netplay_use_upnp",
                                 GetEnabledDisabled(Config::Get(Config::NETPLAY_USE_UPNP)),
                                 use_current_values));
  AddCoreOption("dolphin_netplay_enable_qos", "NetPlay enable QoS", {"enabled", "disabled"},
                GetOptionDefault("dolphin_netplay_enable_qos",
                                 GetEnabledDisabled(Config::Get(Config::NETPLAY_ENABLE_QOS)),
                                 use_current_values));

  s_core_options.push_back({nullptr, nullptr});
  s_environment(RETRO_ENVIRONMENT_SET_VARIABLES, s_core_options.data());
}

const char* GetCoreOptionValue(const char* key)
{
  if (!s_environment)
    return nullptr;

  retro_variable var{key, nullptr};
  if (!s_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value)
    return nullptr;

  return var.value;
}

template <typename T>
bool SetConfigIfChanged(const Config::Info<T>& info, const T& value)
{
  if (Config::Get(info) == value)
    return false;

  Config::SetBaseOrCurrent(info, value);
  return true;
}

bool ApplyBoolOption(const char* key, const Config::Info<bool>& info)
{
  const char* value = GetCoreOptionValue(key);
  if (!value)
    return false;

  const bool enabled = std::string_view(value) == "enabled";
  return SetConfigIfChanged(info, enabled);
}

bool ApplyInternalResolutionOption(const char* key)
{
  const char* value = GetCoreOptionValue(key);
  if (!value)
    return false;

  std::string scale_string = value;
  if (!scale_string.empty() && scale_string.back() == 'x')
    scale_string.pop_back();

  int scale = 0;
  if (!TryParse(scale_string, &scale))
    return false;

  return SetConfigIfChanged(Config::GFX_EFB_SCALE, scale);
}

bool ApplyWiimoteSourceOption(const char* key, int index)
{
  const char* value = GetCoreOptionValue(key);
  if (!value)
    return false;

  WiimoteSource source = WiimoteSource::None;
  if (std::string_view(value) == "emulated")
    source = WiimoteSource::Emulated;
  else if (std::string_view(value) == "real")
    source = WiimoteSource::Real;
  else if (std::string_view(value) != "none")
    return false;

  return SetConfigIfChanged(Config::GetInfoForWiimoteSource(index), source);
}

void ApplyCoreOptions()
{
  bool changed = false;
  changed |= ApplyInternalResolutionOption("dolphin_internal_resolution");
  changed |= ApplyBoolOption("dolphin_widescreen_hack", Config::GFX_WIDESCREEN_HACK);
  changed |= ApplyBoolOption("dolphin_vsync", Config::GFX_VSYNC);
  changed |= ApplyBoolOption("dolphin_dual_core", Config::MAIN_CPU_THREAD);
  changed |= ApplyBoolOption("dolphin_dsp_hle", Config::MAIN_DSP_HLE);
  changed |= ApplyBoolOption("dolphin_sync_on_skip_idle", Config::MAIN_SYNC_ON_SKIP_IDLE);
  changed |= ApplyBoolOption("dolphin_cheats", Config::MAIN_ENABLE_CHEATS);
  changed |= ApplyBoolOption("dolphin_savestates", Config::MAIN_ENABLE_SAVESTATES);
  changed |= ApplyBoolOption("dolphin_wiimote_speaker", Config::MAIN_WIIMOTE_ENABLE_SPEAKER);
  changed |= ApplyWiimoteSourceOption("dolphin_wiimote_1", 0);
  changed |= ApplyWiimoteSourceOption("dolphin_wiimote_2", 1);
  changed |= ApplyWiimoteSourceOption("dolphin_wiimote_3", 2);
  changed |= ApplyWiimoteSourceOption("dolphin_wiimote_4", 3);

  if (changed)
    Config::Save();
}

bool ApplyStringOption(const char* key, const Config::Info<std::string>& info,
                       std::initializer_list<std::string_view> allowed)
{
  const char* value = GetCoreOptionValue(key);
  if (!value)
    return false;

  std::string_view candidate = value;
  if (!allowed.size())
    return SetConfigIfChanged(info, std::string(candidate));

  for (std::string_view allowed_value : allowed)
  {
    if (candidate == allowed_value)
      return SetConfigIfChanged(info, std::string(candidate));
  }

  return false;
}

bool ApplyU16Option(const char* key, const Config::Info<u16>& info, u16 min, u16 max)
{
  const char* value = GetCoreOptionValue(key);
  if (!value)
    return false;

  unsigned parsed = 0;
  if (!TryParse(value, &parsed))
    return false;

  u16 clamped = static_cast<u16>(std::clamp(parsed, static_cast<unsigned>(min),
                                            static_cast<unsigned>(max)));
  return SetConfigIfChanged(info, clamped);
}

bool ApplyU32Option(const char* key, const Config::Info<u32>& info, u32 min, u32 max)
{
  const char* value = GetCoreOptionValue(key);
  if (!value)
    return false;

  unsigned parsed = 0;
  if (!TryParse(value, &parsed))
    return false;

  u32 clamped = static_cast<u32>(std::clamp(parsed, static_cast<unsigned>(min),
                                            static_cast<unsigned>(max)));
  return SetConfigIfChanged(info, clamped);
}

bool ApplyNetPlayOptions()
{
  bool changed = false;
  changed |= ApplyStringOption("dolphin_netplay_address", Config::NETPLAY_ADDRESS, {});
  changed |= ApplyU16Option("dolphin_netplay_connect_port", Config::NETPLAY_CONNECT_PORT, 1, 65535);
  changed |= ApplyU16Option("dolphin_netplay_host_port", Config::NETPLAY_HOST_PORT, 1, 65535);
  changed |= ApplyU16Option("dolphin_netplay_listen_port", Config::NETPLAY_LISTEN_PORT, 1, 65535);
  changed |= ApplyStringOption("dolphin_netplay_host_code", Config::NETPLAY_HOST_CODE, {});
  changed |= ApplyStringOption("dolphin_netplay_traversal_server",
                               Config::NETPLAY_TRAVERSAL_SERVER, {});
  changed |= ApplyU16Option("dolphin_netplay_traversal_port", Config::NETPLAY_TRAVERSAL_PORT, 1,
                            65535);
  changed |= ApplyU16Option("dolphin_netplay_traversal_port_alt",
                            Config::NETPLAY_TRAVERSAL_PORT_ALT, 1, 65535);
  changed |= ApplyBoolOption("dolphin_netplay_lobby_advertise", Config::NETPLAY_USE_INDEX);
  changed |= ApplyStringOption("dolphin_netplay_lobby_region", Config::NETPLAY_INDEX_REGION,
                               {"EA", "CN", "EU", "NA", "SA", "OC", "AF"});
  changed |= ApplyBoolOption("dolphin_netplay_savedata_load", Config::NETPLAY_SAVEDATA_LOAD);
  changed |= ApplyBoolOption("dolphin_netplay_savedata_write", Config::NETPLAY_SAVEDATA_WRITE);
  changed |= ApplyBoolOption("dolphin_netplay_savedata_sync_all_wii",
                             Config::NETPLAY_SAVEDATA_SYNC_ALL_WII);
  changed |= ApplyBoolOption("dolphin_netplay_sync_codes", Config::NETPLAY_SYNC_CODES);
  changed |= ApplyBoolOption("dolphin_netplay_strict_settings_sync",
                             Config::NETPLAY_STRICT_SETTINGS_SYNC);
  changed |= ApplyBoolOption("dolphin_netplay_record_inputs", Config::NETPLAY_RECORD_INPUTS);
  changed |= ApplyBoolOption("dolphin_netplay_golf_overlay", Config::NETPLAY_GOLF_MODE_OVERLAY);
  changed |= ApplyBoolOption("dolphin_netplay_hide_remote_gbas", Config::NETPLAY_HIDE_REMOTE_GBAS);
  changed |= ApplyBoolOption("dolphin_netplay_use_upnp", Config::NETPLAY_USE_UPNP);
  changed |= ApplyBoolOption("dolphin_netplay_enable_qos", Config::NETPLAY_ENABLE_QOS);
  changed |= ApplyStringOption("dolphin_netplay_network_mode", Config::NETPLAY_NETWORK_MODE,
                               {"fixeddelay", "hostinputauthority", "golf"});
  changed |= ApplyU32Option("dolphin_netplay_buffer_size", Config::NETPLAY_BUFFER_SIZE, 1, 20);
  changed |= ApplyU32Option("dolphin_netplay_client_buffer_size", Config::NETPLAY_CLIENT_BUFFER_SIZE,
                            1, 5);

  if (changed)
    Config::Save();
  return changed;
}

void RefreshNetPlayRooms()
{
  NetPlayIndex index;
  std::map<std::string, std::string> filters;
  const std::string region = Config::Get(Config::NETPLAY_INDEX_REGION);
  if (!region.empty())
    filters.emplace("region", region);

  auto rooms = index.List(filters);
  if (!rooms)
  {
    LogMessage(RETRO_LOG_WARN, "NetPlay lobby refresh failed: %s\n",
               index.GetLastError().c_str());
    return;
  }

  {
    std::lock_guard lk(s_netplay_mutex);
    s_netplay_rooms = std::move(*rooms);
  }

  LogMessage(RETRO_LOG_INFO, "NetPlay lobby rooms: %zu\n", s_netplay_rooms.size());
  BuildCoreOptions(true);
}

struct NetPlayJoinTarget
{
  std::string address;
  u16 port = 0;
  bool use_traversal = false;
};

std::optional<NetPlayJoinTarget> ResolveNetPlayJoinTarget(NetPlayConnection connection)
{
  NetPlayJoinTarget target;

  if (connection == NetPlayConnection::Lobby)
  {
    const char* room_value = GetCoreOptionValue("dolphin_netplay_lobby_room");
    if (!room_value || std::string_view(room_value) == "manual")
    {
      LogMessage(RETRO_LOG_WARN, "NetPlay lobby room not selected\n");
      return std::nullopt;
    }

    const auto it = s_netplay_room_value_map.find(room_value);
    if (it == s_netplay_room_value_map.end())
    {
      LogMessage(RETRO_LOG_WARN, "NetPlay lobby room not found\n");
      return std::nullopt;
    }

    NetPlaySession session;
    {
      std::lock_guard lk(s_netplay_mutex);
      if (it->second >= s_netplay_rooms.size())
        return std::nullopt;
      session = s_netplay_rooms[it->second];
    }

    std::string server_id = session.server_id;
    if (session.has_password)
    {
      const std::string password = Config::Get(Config::NETPLAY_INDEX_PASSWORD);
      const auto decrypted = session.DecryptID(password);
      if (!decrypted)
      {
        LogMessage(RETRO_LOG_WARN, "NetPlay lobby password missing or invalid\n");
        return std::nullopt;
      }
      server_id = *decrypted;
    }

    if (session.method == "traversal")
    {
      target.use_traversal = true;
      target.address = std::move(server_id);
      target.port = Config::Get(Config::NETPLAY_CONNECT_PORT);
    }
    else
    {
      target.use_traversal = false;
      target.address = std::move(server_id);
      target.port = static_cast<u16>(session.port);
      Config::SetBaseOrCurrent(Config::NETPLAY_ADDRESS, target.address);
      Config::SetBaseOrCurrent(Config::NETPLAY_CONNECT_PORT, target.port);
    }

    return target;
  }

  target.use_traversal = (connection == NetPlayConnection::Traversal);
  if (target.use_traversal)
  {
    target.address = Config::Get(Config::NETPLAY_HOST_CODE);
    target.port = Config::Get(Config::NETPLAY_CONNECT_PORT);
  }
  else
  {
    target.address = Config::Get(Config::NETPLAY_ADDRESS);
    target.port = Config::Get(Config::NETPLAY_CONNECT_PORT);
  }

  return target;
}

void ShutdownNetPlay()
{
  if (s_netplay_client)
    s_netplay_client->Stop();

  s_netplay_client.reset();
  s_netplay_server.reset();
  s_netplay_ui.reset();

  s_netplay_rooms.clear();
  s_netplay_room_value_map.clear();
  s_netplay_selected_game = {};
  s_netplay_selected_game_name.clear();
  s_netplay_start_requested.store(false);
}

bool StartNetPlaySession()
{
  const NetPlayMode mode = GetNetPlayMode();
  if (mode == NetPlayMode::Disabled)
    return false;

  if (!s_loaded_game_file || !s_loaded_game_file->IsValid())
  {
    LogMessage(RETRO_LOG_ERROR, "NetPlay requires a valid game file\n");
    return false;
  }

  if (!s_netplay_ui)
    s_netplay_ui = std::make_unique<LibretroNetPlayUI>();

  const std::string nickname = GetNetPlayNickname();
  if (!nickname.empty())
    Config::SetBaseOrCurrent(Config::NETPLAY_NICKNAME, nickname);

  const std::string traversal_host = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
  const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
  const u16 traversal_port_alt = Config::Get(Config::NETPLAY_TRAVERSAL_PORT_ALT);

  if (mode == NetPlayMode::Host)
  {
    NetPlayConnection connection = GetNetPlayConnection();
    const bool use_traversal = (connection == NetPlayConnection::Traversal);

    Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE,
                             use_traversal ? "traversal" : "direct");

    if (Config::Get(Config::NETPLAY_USE_INDEX))
    {
      Config::SetBaseOrCurrent(Config::NETPLAY_INDEX_NAME, GetNetPlayRoomName());
    }

    const u16 host_port =
        use_traversal ? Config::Get(Config::NETPLAY_LISTEN_PORT) :
                        Config::Get(Config::NETPLAY_HOST_PORT);

    s_netplay_server = std::make_unique<NetPlay::NetPlayServer>(
        host_port, Config::Get(Config::NETPLAY_USE_UPNP), s_netplay_ui.get(),
        NetPlay::NetTraversalConfig{use_traversal, traversal_host, traversal_port,
                                    traversal_port_alt});

    if (!s_netplay_server->is_connected)
    {
      LogMessage(RETRO_LOG_ERROR, "NetPlay host failed to listen on port %u\n", host_port);
      ShutdownNetPlay();
      return false;
    }

    const std::string netplay_name = GetNetPlayRoomName();
    {
      std::lock_guard lk(s_netplay_mutex);
      s_netplay_selected_game = s_loaded_game_file->GetSyncIdentifier();
      s_netplay_selected_game_name = netplay_name;
    }
    s_netplay_server->ChangeGame(s_netplay_selected_game, netplay_name);

    s_netplay_client = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", s_netplay_server->GetPort(), s_netplay_ui.get(), nickname,
        NetPlay::NetTraversalConfig{false, traversal_host, traversal_port});

    if (!s_netplay_client->IsConnected())
    {
      LogMessage(RETRO_LOG_ERROR, "NetPlay host failed to connect local client\n");
      ShutdownNetPlay();
      return false;
    }

    return true;
  }

  NetPlayConnection connection = GetNetPlayConnection();
  auto target = ResolveNetPlayJoinTarget(connection);
  if (!target)
    return false;

  Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE,
                           target->use_traversal ? "traversal" : "direct");
  if (target->use_traversal)
    Config::SetBaseOrCurrent(Config::NETPLAY_HOST_CODE, target->address);

  s_netplay_client = std::make_unique<NetPlay::NetPlayClient>(
      target->address, target->port, s_netplay_ui.get(), nickname,
      NetPlay::NetTraversalConfig{target->use_traversal, traversal_host, traversal_port});

  if (!s_netplay_client->IsConnected())
  {
    LogMessage(RETRO_LOG_ERROR, "NetPlay join failed to connect\n");
    ShutdownNetPlay();
    return false;
  }

  return true;
}

void StartNetPlayGame()
{
  if (!s_netplay_client)
    return;

  if (s_game_loaded)
    return;

  if (s_loaded_game_path.empty() || !s_loaded_game_file)
  {
    LogMessage(RETRO_LOG_ERROR, "NetPlay start failed: no game path\n");
    return;
  }

  NetPlay::SyncIdentifier selected;
  {
    std::lock_guard lk(s_netplay_mutex);
    selected = s_netplay_selected_game;
  }

  if (!selected.game_id.empty())
  {
    if (s_loaded_game_file->CompareSyncIdentifier(selected) !=
        NetPlay::SyncIdentifierComparison::SameGame)
    {
      LogMessage(RETRO_LOG_ERROR, "NetPlay start failed: game mismatch\n");
      return;
    }
  }

  s_netplay_client->StartGame(s_loaded_game_path);
}

void UpdateNetPlayOptions()
{
  ApplyNetPlayOptions();

  const auto update_cached = [](const char* key, std::string* cache, std::string* out_value) {
    const char* value = GetCoreOptionValue(key);
    std::string next = value ? value : "";
    const bool changed = (*cache != next);
    *cache = next;
    if (out_value)
      *out_value = std::move(next);
    return changed;
  };

  std::string refresh;
  const bool refresh_changed = update_cached("dolphin_netplay_lobby_refresh",
                                             &s_netplay_option_cache.refresh_rooms, &refresh);
  if (refresh_changed && refresh == "yes")
    RefreshNetPlayRooms();

  std::string start_game;
  const bool start_changed =
      update_cached("dolphin_netplay_start", &s_netplay_option_cache.start_game, &start_game);
  if (start_changed && start_game == "yes" && s_netplay_server)
  {
    s_netplay_server->RequestStartGame();
  }

  std::string mode;
  update_cached("dolphin_netplay_mode", &s_netplay_option_cache.mode, &mode);
  if (mode == "disabled" && (s_netplay_client || s_netplay_server))
  {
    if (s_game_loaded)
      s_stop_requested.store(true);
    else
      ShutdownNetPlay();
  }
}

void UpdateCoreOptions()
{
  if (!s_environment)
    return;

  bool updated = false;
  if (s_environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
  {
    ApplyCoreOptions();
    UpdateNetPlayOptions();
  }
}

std::vector<std::string> SplitCheatLines(const char* code)
{
  std::vector<std::string> lines;
  if (!code)
    return lines;

  std::string current;
  for (const char ch : std::string(code))
  {
    if (ch == '\r')
      continue;
    if (ch == '\n' || ch == ';')
    {
      std::string trimmed(StripWhitespace(current));
      if (!trimmed.empty())
        lines.push_back(std::move(trimmed));
      current.clear();
      continue;
    }
    current.push_back(ch);
  }

  std::string trimmed(StripWhitespace(current));
  if (!trimmed.empty())
    lines.push_back(std::move(trimmed));

  return lines;
}

bool ApplyCheats()
{
  if (!s_game_loaded)
    return false;

  auto& config = SConfig::GetInstance();
  const std::string game_id = config.GetGameID();
  const u16 revision = config.GetRevision();

  Common::IniFile global_ini = config.LoadDefaultGameIni();
  Common::IniFile local_ini = config.LoadLocalGameIni();

  std::vector<ActionReplay::ARCode> ar_codes = ActionReplay::LoadCodes(global_ini, local_ini);
  std::vector<Gecko::GeckoCode> gecko_codes = Gecko::LoadCodes(global_ini, local_ini);

  bool any_enabled = false;
  for (const LibretroCheat& cheat : s_cheats)
  {
    if (!cheat.valid)
      continue;

    any_enabled |= cheat.enabled;

    if (cheat.backend == CheatBackend::ActionReplay)
      ar_codes.push_back(cheat.ar_code);
    else
      gecko_codes.push_back(cheat.gecko_code);
  }

  if (any_enabled)
    Config::SetBaseOrCurrent(Config::MAIN_ENABLE_CHEATS, true);

  ActionReplay::ApplyCodes(ar_codes, game_id, revision);
  Gecko::SetActiveCodes(gecko_codes, game_id, revision);
  return true;
}

bool ParseActionReplayCheat(const std::vector<std::string>& lines, ActionReplay::ARCode* out_code)
{
  if (!out_code)
    return false;

  std::vector<std::string> encrypted_lines;
  for (const std::string& line : lines)
  {
    const auto result = ActionReplay::DeserializeLine(line);
    if (std::holds_alternative<ActionReplay::AREntry>(result))
    {
      out_code->ops.push_back(std::get<ActionReplay::AREntry>(result));
    }
    else if (std::holds_alternative<ActionReplay::EncryptedLine>(result))
    {
      encrypted_lines.push_back(std::get<ActionReplay::EncryptedLine>(result));
    }
    else
    {
      return false;
    }
  }

  if (!encrypted_lines.empty())
    ActionReplay::DecryptARCode(encrypted_lines, &out_code->ops);

  return !out_code->ops.empty();
}

bool ParseGeckoCheat(const std::vector<std::string>& lines, Gecko::GeckoCode* out_code)
{
  if (!out_code)
    return false;

  for (const std::string& line : lines)
  {
    Gecko::GeckoCode::Code code_entry;
    if (std::optional<Gecko::GeckoCode::Code> parsed = Gecko::DeserializeLine(line))
      code_entry = *parsed;
    else
      code_entry.original_line = line;
    out_code->codes.push_back(std::move(code_entry));
  }

  return !out_code->codes.empty();
}

LibretroCheat BuildCheat(unsigned index, bool enabled, const char* code)
{
  LibretroCheat cheat;
  cheat.enabled = enabled;

  std::vector<std::string> lines = SplitCheatLines(code);
  if (lines.empty())
    return cheat;

  std::string prefix = lines.front();
  std::string prefix_lower = prefix;
  Common::ToLower(&prefix_lower);
  if (prefix_lower.starts_with("gecko:"))
  {
    lines.front() = std::string(StripWhitespace(prefix.substr(6)));
    if (lines.front().empty())
      lines.erase(lines.begin());
    cheat.backend = CheatBackend::Gecko;
  }
  else if (prefix_lower.starts_with("ar:") || prefix_lower.starts_with("actionreplay:"))
  {
    const size_t prefix_len = prefix_lower.starts_with("ar:") ? 3 : 13;
    lines.front() = std::string(StripWhitespace(prefix.substr(prefix_len)));
    if (lines.front().empty())
      lines.erase(lines.begin());
    cheat.backend = CheatBackend::ActionReplay;
  }
  else
  {
    bool ar_ok = true;
    bool gecko_ok = true;
    bool has_encrypted = false;

    for (const std::string& line : lines)
    {
      const auto ar_result = ActionReplay::DeserializeLine(line);
      if (std::holds_alternative<ActionReplay::EncryptedLine>(ar_result))
        has_encrypted = true;
      else if (!std::holds_alternative<ActionReplay::AREntry>(ar_result))
        ar_ok = false;

      if (!Gecko::DeserializeLine(line))
        gecko_ok = false;
    }

    if (has_encrypted)
      cheat.backend = CheatBackend::ActionReplay;
    else if (ar_ok)
      cheat.backend = CheatBackend::ActionReplay;
    else if (gecko_ok)
      cheat.backend = CheatBackend::Gecko;
    else
      return cheat;
  }

  if (cheat.backend == CheatBackend::ActionReplay)
  {
    cheat.ar_code.name = "Libretro Cheat " + std::to_string(index + 1);
    cheat.ar_code.enabled = enabled;
    cheat.ar_code.default_enabled = enabled;
    cheat.ar_code.user_defined = true;
    cheat.valid = ParseActionReplayCheat(lines, &cheat.ar_code);
  }
  else
  {
    cheat.gecko_code.name = "Libretro Cheat " + std::to_string(index + 1);
    cheat.gecko_code.enabled = enabled;
    cheat.gecko_code.default_enabled = enabled;
    cheat.gecko_code.user_defined = true;
    cheat.valid = ParseGeckoCheat(lines, &cheat.gecko_code);
  }

  return cheat;
}
}  // namespace

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

void Host_PPCSymbolsChanged()
{
}

void Host_PPCBreakpointsChanged()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

void Host_Message(const HostMessageID id)
{
  if (id == HostMessageID::WMUserStop)
    s_stop_requested.store(true);
}

void Host_UpdateTitle(const std::string& title)
{
  LogMessage(RETRO_LOG_INFO, "Title: %s\n", title.c_str());
}

void Host_UpdateDisasmDialog()
{
}

void Host_JitCacheInvalidation()
{
}

void Host_JitProfileDataWiped()
{
}

void Host_RequestRenderWindowSize(int, int)
{
}

bool Host_RendererHasFocus()
{
  return true;
}

bool Host_RendererHasFullFocus()
{
  return true;
}

bool Host_RendererIsFullscreen()
{
  return false;
}

bool Host_TASInputHasFocus()
{
  return false;
}

void Host_YieldToUI()
{
}

void Host_TitleChanged()
{
}

void Host_UpdateDiscordClientID(const std::string&)
{
}

bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&, const std::string&,
                                   const std::string&, const std::string&, const std::string&,
                                   const int64_t, const int64_t, const int, const int)
{
  return false;
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>)
{
  return nullptr;
}

extern "C" {

RETRO_API void retro_set_environment(retro_environment_t cb)
{
  s_environment = cb;

  bool no_game = false;
  if (s_environment)
    s_environment(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
  s_video_refresh = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)
{
  s_audio_sample = cb;
  UpdateLibretroAudioCallback();
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
  s_audio_sample_batch = cb;
  UpdateLibretroAudioCallback();
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
  s_input_poll = cb;
  InputCommon::SetLibretroInputPoll(reinterpret_cast<InputCommon::LibretroInputPoll>(cb));
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
  s_input_state = cb;
  InputCommon::SetLibretroInputState(reinterpret_cast<InputCommon::LibretroInputState>(cb));
}

RETRO_API unsigned retro_api_version(void)
{
  return RETRO_API_VERSION;
}

RETRO_API void retro_init(void)
{
  retro_log_callback log_cb{};
  if (s_environment && s_environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_cb))
    s_log = log_cb.log;

  retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
  if (s_environment && !s_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    LogMessage(RETRO_LOG_WARN, "Failed to set pixel format.\n");

  s_wsi.type = WindowSystemType::Headless;
  s_wsi.display_connection = nullptr;
  s_wsi.render_window = nullptr;
  s_wsi.render_surface = nullptr;

  SetSystemDirectoryFromEnvironment();
  SetUserDirectoryFromEnvironment();

  UICommon::Init();
  SetupLibretroLogging();
  BuildCoreOptions(false);
  ApplyCoreOptions();
  ApplyNetPlayOptions();
  ForceLibretroVideoConfig();
  ForceLibretroVideoConfig();
  UICommon::InitControllers(s_wsi);
  Common::RegisterMsgAlertHandler(LibretroMsgAlertHandler);

  Config::SetBaseOrCurrent(Config::MAIN_GFX_BACKEND, OGL::VideoBackend::CONFIG_NAME);
  Config::SetBaseOrCurrent(Config::MAIN_AUDIO_BACKEND, BACKEND_LIBRETRO);
  Config::SetBaseOrCurrent(Config::MAIN_DPL2_DECODER, false);
  VideoBackendBase::ActivateBackend(Config::Get(Config::MAIN_GFX_BACKEND));

  s_initialized = true;
}

RETRO_API void retro_deinit(void)
{
  ShutdownNetPlay();

  if (s_game_loaded)
    StopCore();

  if (s_initialized)
  {
    UICommon::ShutdownControllers();
    UICommon::Shutdown();
  }

  AudioCommon::SetLibretroAudioSampleBatch(nullptr);
  LibretroSetGLCallbacks(LibretroGLCallbacks{});
  s_state_hook.reset();
  InputCommon::SetLibretroInputPoll(nullptr);
  InputCommon::SetLibretroInputState(nullptr);
  s_audio_sample = nullptr;
  s_audio_sample_batch = nullptr;
  s_core_options.clear();
  s_core_option_strings.clear();
  s_state_buffer.reset(0);
  s_cheats.clear();
  s_loaded_game_file.reset();
  s_loaded_game_path.clear();
  s_netplay_option_cache = {};
  s_pending_boot.reset();
  s_game_loaded = false;
  s_initialized = false;
  s_hw_render_enabled = false;
  s_hw_context_ready.store(false);
}

RETRO_API void retro_get_system_info(struct retro_system_info* info)
{
  static std::string s_library_version;
  if (s_library_version.empty())
    s_library_version = Common::GetScmDescStr();

  info->library_name = "Dolphin";
  info->library_version = s_library_version.c_str();
  info->valid_extensions = "iso;gcm;gcz;wbfs;ciso;wad;elf;dol";
  info->need_fullpath = true;
  info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info)
{
  info->timing.fps = 60.0;
  info->timing.sample_rate = 48000.0;
  info->geometry.base_width = 640;
  info->geometry.base_height = 528;
  info->geometry.max_width = 640;
  info->geometry.max_height = 528;
  info->geometry.aspect_ratio = 4.0f / 3.0f;
}

RETRO_API void retro_set_controller_port_device(unsigned, unsigned)
{
}

RETRO_API void retro_reset(void)
{
}

RETRO_API bool retro_load_game(const struct retro_game_info* info)
{
  if (!s_initialized || !info || !info->path)
    return false;

  SetSystemDirectoryFromEnvironment();

  s_hw_render_enabled = SetupHardwareRendering();
  if (!s_hw_render_enabled)
  {
    LogMessage(RETRO_LOG_ERROR, "Failed to initialize libretro hardware rendering.\n");
    return false;
  }

  s_wsi.type = WindowSystemType::Libretro;

  ApplyCoreOptions();
  ApplyNetPlayOptions();

  const NetPlayMode netplay_mode = GetNetPlayMode();
  s_loaded_game_path = info->path;
  s_loaded_game_file = std::make_shared<UICommon::GameFile>(s_loaded_game_path);
  if (netplay_mode != NetPlayMode::Disabled)
  {
    if (!StartNetPlaySession())
      return false;

    return true;
  }

  if (!s_hw_context_ready.load())
  {
    DeferBoot(s_loaded_game_path, std::nullopt, false);
    return true;
  }

  if (!BootGameInternal(s_loaded_game_path, std::nullopt, false))
    return false;
  return true;
}

RETRO_API void retro_unload_game(void)
{
  ShutdownNetPlay();
  if (s_game_loaded)
    StopCore();
  s_game_loaded = false;
  s_hw_render_enabled = false;
  s_pending_boot.reset();
  s_loaded_game_file.reset();
  s_loaded_game_path.clear();
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t)
{
  return false;
}

RETRO_API unsigned retro_get_region(void)
{
  return RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned)
{
  return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned)
{
  return 0;
}

RETRO_API void retro_run(void)
{
  UpdateCoreOptions();

  if (s_input_poll)
    s_input_poll();

  if (s_stop_requested.exchange(false))
    StopCore();

  if (!s_game_loaded && GetNetPlayMode() == NetPlayMode::Disabled &&
      (s_netplay_client || s_netplay_server))
  {
    ShutdownNetPlay();
  }

  if (s_netplay_start_requested.exchange(false))
    StartNetPlayGame();

  if (!s_game_loaded && s_pending_boot && s_hw_context_ready.load())
  {
    PendingBoot pending = std::move(*s_pending_boot);
    s_pending_boot.reset();
    BootGameInternal(std::move(pending.path), std::move(pending.session), pending.is_netplay);
  }

  if (s_game_loaded)
    Core::HostDispatchJobs(Core::System::GetInstance());

  if (s_pending_present.exchange(false) && s_video_refresh)
  {
    const unsigned width = s_present_width.load();
    const unsigned height = s_present_height.load();
    s_video_refresh(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
  }

  if (!s_game_loaded || !s_hw_render_enabled)
    SubmitDummyFrame();
}

RETRO_API size_t retro_serialize_size(void)
{
  if (!s_game_loaded)
    return 0;

  auto& system = Core::System::GetInstance();
  s_state_buffer.reset(0);
  State::SaveToBuffer(system, s_state_buffer);
  return s_state_buffer.size();
}

RETRO_API void retro_cheat_reset(void)
{
  s_cheats.clear();
  ApplyCheats();
}

RETRO_API bool retro_serialize(void* data, size_t size)
{
  if (!s_game_loaded || !data)
    return false;

  auto& system = Core::System::GetInstance();
  s_state_buffer.reset(0);
  State::SaveToBuffer(system, s_state_buffer);

  if (s_state_buffer.empty() || s_state_buffer.size() > size)
    return false;

  std::memcpy(data, s_state_buffer.data(), s_state_buffer.size());
  return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size)
{
  if (!s_game_loaded || !data || size == 0)
    return false;

  Common::UniqueBuffer<u8> buffer(size);
  std::memcpy(buffer.data(), data, size);

  auto& system = Core::System::GetInstance();
  State::LoadFromBuffer(system, buffer);
  return true;
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
  if (index >= s_cheats.size())
    s_cheats.resize(index + 1);

  s_cheats[index] = BuildCheat(index, enabled, code);
  ApplyCheats();
}

}  // extern "C"
